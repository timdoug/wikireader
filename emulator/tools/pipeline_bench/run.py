#!/usr/bin/env python3
"""Run the launcher pipeline probe; optionally compare pipeline.log from hardware."""
import argparse
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from score_model import ROOT, Runner, pipeline_rows as parse, paired, sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', type=Path)
    parser.add_argument('--model-log', type=Path, help='reuse a completed emulator report')
    parser.add_argument('--wremu', type=Path, default=ROOT / 'emulator/wremu')
    parser.add_argument('--out', type=Path)
    parser.add_argument('--timeout', type=int, default=600)
    parser.add_argument('--no-build', action='store_true')
    args = parser.parse_args()
    if args.model_log:
        rows = parse(args.model_log.read_text())
        print(f'{len(rows)} verified cases; model report: {args.model_log}')
        if args.device:
            compare(rows, parse(args.device.read_text()))
        return
    if not args.no_build:
        subprocess.run(['make', '-s', '-C', str(ROOT / 'emulator')], check=True)
        subprocess.run(['make', '-s', '-C', str(Path(__file__).parent)], check=True)
    if args.out:
        args.out = args.out.resolve()
        args.out.mkdir(parents=True, exist_ok=False)
    else:
        args.out = Path(tempfile.mkdtemp(prefix='wremu-pipeline.'))
    env = {**os.environ, 'WREMU_BOARD_REV': '7', 'WR_FAT_TIME': '1793836800',
           'WR_FAT_FRAGMENT': '0'}
    runner = Runner(args, env)
    app = ROOT / 'build/wr128/pipeline/pipeline.app'
    text = runner.run('pipeline', app, app.with_suffix('.ico'), 'off', 'pipeline.log')
    rows = parse(text)
    (args.out / 'inputs.json').write_text(json.dumps({
        'files': runner.hashes, 'emulator_sha256': sha(args.wremu),
        'setting': env.get('WREMU_MODEL', '')}, indent=2) + '\n')
    print(f'{len(rows)} verified cases; report: {args.out}/pipeline/pipeline.log')
    if args.device:
        compare(rows, parse(args.device.read_text()))


def compare(rows, devices):
    print('probe code offset data model_cycles/pass device_cycles/pass ratio')
    for m, d in paired(rows, devices):
        for field in ('bytes', 'passes', 'result'):
            if m[field] != d[field]:
                raise ValueError(f'probe workloads differ: {field}')
        mt, dt = (statistics.median(map(int, r['ticks'].split(','))) /
                  int(r['passes']) for r in (m, d))
        print(m['probe'], m['code'], m['offset'], m['data'],
              f'{mt:.3f} {dt:.3f} {mt/dt:.4f}')


if __name__ == '__main__':
    main()
