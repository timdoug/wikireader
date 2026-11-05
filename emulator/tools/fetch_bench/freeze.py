#!/usr/bin/env python3
"""Freeze actual Grifo-path forecasts before any physical fetchq collection."""
import argparse
import hashlib
import json
from pathlib import Path
import statistics

from report import KEYS, validate

ROOT = Path(__file__).resolve().parents[3]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, action='append', required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if args.out.exists():
        raise ValueError('Refusing to overwrite prospective predictions')
    if Path(__file__).with_name('device.json').exists():
        raise ValueError('Hardware evidence already exists; this cannot be prospective')
    models, identities = {}, []
    shared_manifest = None
    for run in args.run:
        record = json.loads((run / 'results.json').read_text())
        manifest = record['manifest']
        if shared_manifest is not None and manifest != shared_manifest:
            raise ValueError('Prediction runs use different probe inputs')
        shared_manifest = manifest
        if (not record['complete'] or sha(run / 'wremu') != record['emulator_sha256'] or
                sha(run / 'fetchq/fetchq.log') != record['report_sha256']):
            raise ValueError('Incomplete or changed native forecast')
        if record['inputs']['fetchq']['bench.app'] != manifest['app_sha256']:
            raise ValueError('Native run did not execute the declared probe')
        console = (run / 'fetchq/boot.log').read_text(errors='replace')
        if (console.count('Grifo starting') != 1 or
                any(s in console for s in ('C33 exception', '[watchdog reset]', 'Panic:'))):
            raise ValueError('Forecast has an incomplete or faulted boot')
        header, rows = validate((run / 'fetchq/fetchq.log').read_text(), manifest)
        identities.append(record['inputs']['fetchq'])
        label = record['setting'] or 'default'
        if label in models:
            raise ValueError('Duplicate model setting')
        models[label] = dict(run=str(run.resolve().relative_to(ROOT)), header=header,
                             emulator_sha256=record['emulator_sha256'],
                             report_sha256=record['report_sha256'],
                             cases=[dict(key=dict(zip(KEYS, key)), samples=list(map(int, row['ticks'].split(','))),
                                         median_ticks=statistics.median(map(int, row['ticks'].split(','))),
                                         result=int(row['result'])) for key, row in sorted(rows.items())])
    if any(i != identities[0] for i in identities[1:]):
        raise ValueError('Forecast firmware, launcher or FLASH inputs differ')
    if len({m['emulator_sha256'] for m in models.values()}) != 1:
        raise ValueError('Use the same frozen core for contrasting model settings')
    tools = Path(__file__).parent
    sources = {str(p.relative_to(ROOT)): sha(p) for p in sorted(tools.glob('*'))
               if p.suffix in ('.c', '.py')}
    record = dict(version=1, classification='Prospective emulator predictions, no hardware timing collected',
                  complete=True, manifest=shared_manifest, inputs=identities[0], models=models,
                  source_sha256=sources, raw_elapsed_ticks=True, fitted_offset_subtracted=False,
                  interpretation='Each interval includes the identical call/return, prologue and timer boundary. Short runs expose startup/exit cost; raw whole-interval timing remains the comparison metric.')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open('x') as stream:
        stream.write(json.dumps(record, indent=2) + '\n')
    print(f'Frozen {len(models)} settings x 600 cases: {args.out}; sha256 {sha(args.out)}')


if __name__ == '__main__':
    main()
