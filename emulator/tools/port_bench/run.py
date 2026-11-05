#!/usr/bin/env python3
"""Run the local-port probe through FLASH, Grifo and the launcher."""
import argparse
import json
import os
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from score_model import ROOT, Runner, fields, sha


def validate(text, manifest):
    if not text.startswith('PORTQ v1 '):
        raise ValueError('Not a local-port report')
    expected = {(name, code, str(offset), data)
                for name in manifest['probes'] for code, offset in manifest['placements']
                for data in manifest['data']}
    if (manifest['version'] != 1 or manifest['passes'] != 20000 or manifest['repeats'] != 3 or
            manifest['cases'] != len(expected)):
        raise ValueError('Invalid local-port manifest')
    if (len(re.findall(r'^PORTQ ', text, re.M)) != 1 or
            len(re.findall(r'^END PORTQ ', text, re.M)) != 1 or
            not re.search(rf'^END PORTQ cases={len(expected)} failures=0\s*\Z', text, re.M)):
        raise ValueError('Incomplete or failed local-port run')
    header = dict(re.findall(r'(\w+)=(\S+)', text.splitlines()[0]))
    if any(header.get(k) != v for k, v in {'passes': '20000', 'repeats': '3', 'unroll': 'variable'}.items()):
        raise ValueError('Local-port header differs from manifest')
    for key in ('sdram_ctl', 'refresh', 'clkcntl', 'pll', 'a0', 'sdram', 'code'):
        if not re.fullmatch(r'[0-9a-f]{8}', header.get(key, '')):
            raise ValueError(f'Missing local-port context: {key}')
    rows = fields(text, ('probe', 'code', 'offset', 'data'))
    if rows.keys() != expected:
        raise ValueError('Incomplete local-port coverage')
    for key, row in rows.items():
        probe = manifest['probes'][key[0]]
        samples = [int(x) for x in row['ticks'].split(',')]
        if (row['ok'] != '1' or int(row['result']) != probe['result'] or
                int(row['unroll']) != probe['unroll'] or int(row['bytes']) != probe['bytes'] or
                int(row['passes']) != manifest['passes'] or len(samples) != 3 or min(samples) <= 0):
            raise ValueError(f'Unverified local-port case: {key}')
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wremu', type=Path, default=ROOT / 'emulator/wremu')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=900)
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    stage = ROOT / 'build/wr128/portq'
    manifest = json.loads((stage / 'manifest.json').read_text())
    if sha(stage / 'portq.app') != manifest['app_sha256'] or sha(stage / 'portq.c') != manifest['source_sha256']:
        raise ValueError('Local-port build changed since validation')
    env = {**os.environ, 'WREMU_BOARD_REV': '7', 'WR_FAT_TIME': '1793836800',
           'WR_FAT_FRAGMENT': '0', 'WREMU_UART_TRACE': ''}
    runner = Runner(args, env)
    text = runner.run('portq', stage / 'portq.app', stage / 'portq.ico', 'off', 'portq.log')
    rows = validate(text, manifest)
    (args.out / 'results.json').write_text(json.dumps({
        'manifest': manifest, 'inputs': runner.hashes, 'emulator_sha256': sha(args.wremu),
        'setting': env.get('WREMU_MODEL', ''), 'report_sha256': sha(args.out / 'portq/portq.log'),
        'cases': len(rows)}, indent=2) + '\n')
    print(f'{len(rows)} verified local-port cases: {args.out}')


if __name__ == '__main__':
    main()
