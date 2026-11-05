#!/usr/bin/env python3
"""Validate the dependency-control probe through FLASH, Grifo and the launcher."""
import argparse
import json
import os
from pathlib import Path
import re
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from score_model import ROOT, Runner, fields, sha


def validate(text, manifest):
    if not text.startswith('BUSPIPE v1 '):
        raise ValueError('Not a bus dependency report')
    rows = fields(text, ('probe', 'code', 'offset', 'data'))
    expected = {(name, code, str(offset), data)
                for name in manifest['probes'] for code, offset in manifest['placements']
                for data in manifest['data']}
    if manifest['version'] != 1 or manifest['cases'] != len(expected):
        raise ValueError('Invalid bus probe manifest')
    end = re.search(r'END BUSPIPE cases=(\d+) failures=0\b', text)
    if not end or int(end[1]) != len(expected) or rows.keys() != expected:
        raise ValueError('Incomplete bus probe coverage')
    for key, row in rows.items():
        samples = [int(value) for value in row['ticks'].split(',')]
        if (row['ok'] != '1' or int(row['result']) != manifest['probes'][key[0]] or
                len(samples) != 3 or min(samples) <= 0 or int(row['passes']) != 20000 or
                int(row['bytes']) <= 0):
            raise ValueError(f'Unverified bus probe {key}')
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wremu', type=Path, default=ROOT / 'emulator/wremu')
    parser.add_argument('--out', type=Path)
    parser.add_argument('--timeout', type=int, default=600)
    args = parser.parse_args()
    if args.out:
        args.out = args.out.resolve()
        args.out.mkdir(parents=True, exist_ok=False)
    else:
        args.out = Path(tempfile.mkdtemp(prefix='wremu-buspipe.'))
    stage = ROOT / 'build/wr128/buspipe'
    manifest = json.loads((stage / 'manifest.json').read_text())
    if sha(stage / 'buspipe.app') != manifest['app_sha256']:
        raise ValueError('Bus probe app no longer matches its manifest')
    env = {**os.environ, 'WREMU_BOARD_REV': '7', 'WR_FAT_TIME': '1793836800', 'WR_FAT_FRAGMENT': '0'}
    runner = Runner(args, env)
    text = runner.run('buspipe', stage / 'buspipe.app', stage / 'buspipe.ico', 'off', 'buspipe.log')
    rows = validate(text, manifest)
    (args.out / 'results.json').write_text(json.dumps({
        'manifest': manifest, 'inputs': runner.hashes, 'emulator_sha256': sha(args.wremu),
        'setting': env.get('WREMU_MODEL', ''), 'cases': len(rows)}, indent=2) + '\n')
    print(f'{len(rows)} verified matched bus cases: {args.out}')


if __name__ == '__main__':
    main()
