#!/usr/bin/env python3
"""Compare raw loop timings and variable-unroll curves without fitted offsets."""
import argparse
import json
from pathlib import Path
import re
import statistics

from run import validate, sha
from accuracy_report import summarize

HERE = Path(__file__).resolve().parent


def time(row):
    return statistics.median(map(int, row['ticks'].split(','))) / int(row['passes'])


def compare(model_text, device_text, manifest):
    model, device = (validate(text, manifest) for text in (model_text, device_text))
    headers = [dict(re.findall(r'(\w+)=(\S+)', text.splitlines()[0]))
               for text in (model_text, device_text)]
    for field in ('sdram_ctl', 'clkcntl', 'pll', 'a0', 'sdram', 'code'):
        if headers[0][field] != headers[1][field]:
            raise ValueError(f'Probe contexts differ: {field}')
    accuracy = summarize([(':'.join(key), time(model[key]), time(device[key])) for key in device])
    curves = {}
    for key in device:
        variant = manifest['probes'][key[0]]
        curve = ':'.join((variant['shape'], *key[1:]))
        curves.setdefault(curve, []).append({
            'unroll': variant['unroll'], 'bytes': variant['bytes'],
            'model_cycles_per_pass': time(model[key]), 'device_cycles_per_pass': time(device[key]),
            'model_ticks': list(map(int, model[key]['ticks'].split(','))),
            'device_ticks': list(map(int, device[key]['ticks'].split(',')))})
    for curve in curves.values():
        curve.sort(key=lambda row: row['unroll'])
        for previous, current in zip(curve, curve[1:]):
            gap = current['unroll'] - previous['unroll']
            for source in ('model', 'device'):
                field = f'{source}_cycles_per_pass'
                current[f'{source}_incremental_cycles_per_copy'] = (current[field] - previous[field]) / gap
    return {'accuracy': accuracy, 'curves': curves,
            'scope': 'Raw cycles/pass; adjacent-unroll slopes are diagnostic only. No fitted overhead subtraction.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, required=True)
    parser.add_argument('--device', type=Path, default=HERE / 'device.json')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    record = json.loads(args.device.read_text())
    run = json.loads((args.run / 'results.json').read_text())
    if not record.get('valid'):
        raise ValueError('Latest hardware report failed validation')
    device_log = args.device.parent / record['report']['file']
    model_log = args.run / 'portq/portq.log'
    if sha(device_log) != record['report']['sha256']:
        raise ValueError('Hardware report changed')
    if run['manifest'] != record['manifest']:
        raise ValueError('Hardware and emulator builds differ')
    if sha(args.run / 'wremu') != run['emulator_sha256']:
        raise ValueError('Emulator snapshot changed')
    if sha(model_log) != run['report_sha256']:
        raise ValueError('Model report changed')
    for name in ('kernel.elf', 'init.app', 'portq.app', 'portq.ico'):
        model_name = name.replace('portq.', 'bench.')
        if record['inputs'][name] != run['inputs']['portq'][model_name]:
            raise ValueError(f'Hardware/emulator input differs: {name}')
    result = compare(model_log.read_text(), device_log.read_text(), record['manifest'])
    result.update(device_sha256=sha(device_log), model_sha256=sha(model_log),
                  emulator_sha256=run['emulator_sha256'], setting=run['setting'])
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    print(f'{len(result["curves"])} unroll curves: {args.out}')


if __name__ == '__main__':
    main()
