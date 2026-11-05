#!/usr/bin/env python3
"""Compare a verified hardware run with an emulator run and matched controls."""
import argparse
import json
from pathlib import Path
import re
import statistics

from run import validate, sha
from accuracy_report import summarize

HERE = Path(__file__).resolve().parent
PAIRS = (
    ('load_use0', 'h_indep0'), ('load_use1', 'h_indep1'), ('load_use2', 'h_indep2'),
    ('word_use0', 'w_indep0'), ('word_use1', 'w_indep1'), ('word_use2', 'w_indep2'),
    ('h_cmp_dep', 'h_cmp_indep'), ('w_cmp_dep', 'w_cmp_indep'),
    ('h_mov_dep', 'h_mov_indep'), ('w_mov_dep', 'w_mov_indep'),
    ('load_ext', 'h_ext_indep'),
)


def samples(row):
    return [int(tick) / (int(row['passes']) * 8) for tick in row['ticks'].split(',')]


def contrast(rows, dependent, independent, context):
    dep, ind = (rows[(name, *context)] for name in (dependent, independent))
    if dep['bytes'] != ind['bytes']:
        raise ValueError(f'Matched control sizes differ: {dependent}, {independent}')
    d, i = samples(dep), samples(ind)
    return {'cycles_per_pair': statistics.median(d) - statistics.median(i),
            'observed_range': [min(d) - max(i), max(d) - min(i)]}


def compare(model_text, device_text, manifest):
    m, d = (validate(text, manifest) for text in (model_text, device_text))
    # Refresh includes read-only status bits. Compare the other programmed
    # timing registers and memory placements exactly, rather than fitting
    # instruction costs across different clocks or data layouts.
    mh, dh = (dict(re.findall(r'(\w+)=(\S+)', text.splitlines()[0]))
              for text in (model_text, device_text))
    for field in ('sdram_ctl', 'clkcntl', 'pll', 'a0', 'sdram', 'code'):
        if mh[field] != dh[field]:
            raise ValueError(f'Probe contexts differ: {field}')
    for key in d:
        for field in ('bytes', 'passes', 'result'):
            if m[key][field] != d[key][field]:
                raise ValueError(f'Probe workloads differ: {key}, {field}')
    accuracy = summarize([(':'.join(key), statistics.median(samples(m[key])),
                           statistics.median(samples(d[key]))) for key in d])
    contrasts = []
    for dependent, independent in PAIRS:
        for code, offset in manifest['placements']:
            for data in manifest['data']:
                context = (code, str(offset), data)
                model = contrast(m, dependent, independent, context)
                device = contrast(d, dependent, independent, context)
                contrasts.append({'dependent': dependent, 'independent': independent,
                                  'code': code, 'offset': offset, 'data': data,
                                  'model': model, 'device': device,
                                  'error_cycles_per_pair': model['cycles_per_pair'] - device['cycles_per_pair']})
    return {'accuracy': accuracy, 'matched_contrasts': contrasts}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, required=True, help='completed bus_bench/run.py directory')
    parser.add_argument('--device', type=Path, default=HERE / 'device.json')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    record = json.loads(args.device.read_text())
    run = json.loads((args.run / 'results.json').read_text())
    device_log = args.device.parent / record['report']['file']
    model_log = args.run / 'buspipe/buspipe.log'
    if sha(device_log) != record['report']['sha256']:
        raise ValueError('Recorded hardware report changed')
    if run['manifest'] != record['manifest']:
        raise ValueError('Hardware and emulator probe builds differ')
    if sha(args.run / 'wremu') != run['emulator_sha256']:
        raise ValueError('Emulator snapshot changed')
    for name in ('kernel.elf', 'init.app', 'buspipe.app', 'buspipe.ico'):
        model_name = name.replace('buspipe.', 'bench.')
        if record['inputs'][name] != run['inputs']['buspipe'][model_name]:
            raise ValueError(f'Hardware/emulator input differs: {name}')
    result = compare(model_log.read_text(), device_log.read_text(), record['manifest'])
    result.update({'device_sha256': sha(device_log), 'model_sha256': sha(model_log),
                   'emulator_sha256': run['emulator_sha256'], 'setting': run['setting']})
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    print('dependent       code   off data   device delta  observed range    model delta')
    for row in result['matched_contrasts']:
        device = row['device']
        lo, hi = device['observed_range']
        print(f'{row["dependent"]:15} {row["code"]:6} {row["offset"]:3} {row["data"]:6}'
              f' {device["cycles_per_pair"]:10.5f} [{lo:8.5f},{hi:8.5f}]'
              f' {row["model"]["cycles_per_pair"]:10.5f}')
    print(f'{len(result["matched_contrasts"])} matched comparisons; full evidence: {args.out}')


if __name__ == '__main__':
    main()
