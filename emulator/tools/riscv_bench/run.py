#!/usr/bin/env python3
"""Run all four calibration variants with one FLASH/Grifo/launcher boot."""
import argparse
import json
import os
from pathlib import Path
import re
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from score_model import ROOT, Runner, sha


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
        args.out = Path(tempfile.mkdtemp(prefix='wremu-rv-calibration.'))
    stage = ROOT / 'build/wr128/rv-calibration'
    manifest = json.loads((stage / 'manifest.json').read_text())
    env = {**os.environ, 'WREMU_BOARD_REV': '7', 'WR_FAT_TIME': '1793836800',
           'WR_FAT_FRAGMENT': '0'}
    runner = Runner(args, env)
    extra = {row['app']: (stage / row['app']).read_bytes()
             for row in manifest['variants'].values()}
    extra['rvbench.bin'] = (stage / 'rvbench.bin').read_bytes()
    runner.run('rv-calibration', stage / 'rvsdr.app', stage / 'rvcal.ico',
               'rvbench.bin rvsdr.txt calibrate', 'rvsdr.txt', extra=extra, keep_card=True)
    card = args.out / 'rv-calibration/card.img'
    reports = {}
    for variant, row in manifest['variants'].items():
        data = runner.fat.read_file(card, row['report'])
        if not data or b'rv32: console output:' not in data or b'rv32: total' not in data:
            raise ValueError(f'{variant}: incomplete calibration report')
        text = data.decode()
        checksums = [word for pair in re.findall(r'^(\w+) (0x[0-9a-f]{8})$', text, re.M)
                     for word in pair]
        if checksums != (stage / 'ref.txt').read_text().split() or 'truncated' in text:
            raise ValueError(f'{variant}: workload checksum mismatch')
        (args.out / row['report']).write_text(text)
        reports[variant] = row['report']
    result = {'reports': reports, 'manifest': manifest, 'inputs': runner.hashes,
              'emulator_sha256': sha(args.wremu), 'setting': env.get('WREMU_MODEL', '')}
    (args.out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    card.unlink()
    print(f'All four placement runs passed; reports: {args.out}')


if __name__ == '__main__':
    main()
