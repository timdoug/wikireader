#!/usr/bin/env python3
"""Compare verified reader reports, retaining physical-layout limitations."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics

from build import ROOT, OUT
from run import sha, fat_helper
from report import compare, startup, validate

HERE = Path(__file__).resolve().parent


def analyze(run, device_file):
    device = json.loads(device_file.read_text())
    model = json.loads((run / 'results.json').read_text())
    inputs = model['inputs']
    if not model['complete'] or inputs['manifest'] != device['manifest']:
        raise ValueError('Incomplete run or different reader builds')
    for name, key in (('wremu', 'emulator_sha256'), ('flash.rom', 'flash_sha256'),
                      ('zimbench.txt', 'report_sha256'), ('zimboot.log', 'startup_sha256')):
        if sha(run / name) != model[key]:
            raise ValueError(f'Recorded emulator evidence changed: {name}')
    for name, expected in {**device['manifest']['files'], **device['manifest']['boot_sha256']}.items():
        if inputs['files'].get(name) != expected or device['files'].get(name) != expected:
            raise ValueError(f'Hardware/emulator build differs: {name}')
    if inputs['archives'] != device['archives']:
        raise ValueError('Hardware/emulator archive identities differ')
    helper = fat_helper()
    for name, expected in inputs['files'].items():
        data = helper.read_file(run / 'card.img', name)
        if data is None or hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f'Executed reader input changed: {name}')
    texts = {}
    for key in ('report', 'startup_report'):
        path = device_file.parent / device[key]['file']
        if sha(path) != device[key]['sha256']:
            raise ValueError(f'Hardware evidence changed: {key}')
        texts[key] = path.read_text()
    for name, expected in device.get('resources', {}).items():
        data = helper.read_file(run / 'card.img', name)
        if data is None or hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f'Reader resource differs: {name}')
    mtext = (run / 'zimbench.txt').read_text()
    result = compare(mtext, texts['report'], device['manifest'])
    ma, _ = validate(mtext, device['manifest'])
    da, _ = validate(texts['report'], device['manifest'])
    io = []
    for key in sorted(da):
        for phase in ('lookup', 'retrieve'):
            m, d = ma[key], da[key]
            # Time spent inside reads is already part of elapsed phase time.
            io.append({'pass': key[0], 'entry': key[1], 'phase': phase,
                       'reads': int(d[phase + '_reads']), 'sectors': int(d[phase + '_sectors']),
                       'model_read_us': int(m[phase + '_read_ticks']) / 60,
                       'device_read_us': int(d[phase + '_read_ticks']) / 60,
                       'model_outside_reads_us': (int(m[phase + '_ticks']) - int(m[phase + '_read_ticks'])) / 60,
                       'device_outside_reads_us': (int(d[phase + '_ticks']) - int(d[phase + '_read_ticks'])) / 60})
    mb, db = startup((run / 'zimboot.log').read_text()), startup(texts['startup_report'])
    boot = []
    for phase in ('file', 'indexes', 'ui'):
        m, d = mb['phases'][phase], db['phases'][phase]
        if any(m[k] != d[k] for k in ('read_calls', 'read_sectors', 'dma32_bytes', 'dma8_bytes', 'bypass_bytes')):
            raise ValueError(f'Startup phase work differs: {phase}')
        boot.append({'phase': phase, 'model_us': m['elapsed_us'], 'device_us': d['elapsed_us'],
                     'relative_error': m['elapsed_us'] / d['elapsed_us'] - 1})
    app = {'model_us': mb['totals']['app_to_keyboard_us'],
           'device_us': db['totals']['app_to_keyboard_us']}
    app['relative_error'] = app['model_us'] / app['device_us'] - 1
    errors = [abs(row['median_relative_error']) for row in result['phases']]
    result.update({'comparison_scope': 'matched reader phase work/output; diagnostic with unmatched card allocation',
                   'physical_layout_verified': device['physical_layout_verified'],
                   'reader_resources_verified': bool(device.get('resources')),
                   'resources': device.get('resources', {}),
                   'emulator_sha256': model['emulator_sha256'],
                   'hardware_report_sha256': device['report']['sha256'],
                   'hardware_startup_sha256': device['startup_report']['sha256'],
                   'model_report_sha256': model['report_sha256'],
                   'model_startup_sha256': model['startup_sha256'],
                   'emulator_run': str(run.resolve()), 'io_samples': io,
                   'startup_phases': boot, 'app_to_keyboard': app,
                   'cold_boot_repetitions': 1,
                   'median_absolute_phase_error': statistics.median(errors),
                   'phases_within_5_percent': sum(e <= .05 for e in errors),
                   'phases_within_10_percent': sum(e <= .10 for e in errors),
                   'limitations': ['Physical filesystem allocation is unmatched',
                                   'Kernel boot timing excluded: launcher contents and file-read work differ',
                                   'One cold boot; article passes are not repeated boots',
                                   'Typing, touch parsing, coasting and suspend/wake are unmeasured']})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, default=ROOT / 'build/wr128/idma-deadline-review/workflow')
    parser.add_argument('--device', type=Path, default=HERE / 'device.json')
    parser.add_argument('--out', type=Path, default=OUT / 'hardware-comparison.json')
    args = parser.parse_args()
    result = analyze(args.run, args.device)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    for row in result['phases']:
        print(f'{row["title"]:6} {row["phase"]:12} '
              f'{row["model_us"] / 1000:9.3f} / {row["device_us"] / 1000:9.3f} ms '
              f'{100 * row["median_relative_error"]:+7.2f}%')
    print(f'Verified phase comparison, full workflow unqualified: {args.out}')


if __name__ == '__main__':
    main()
