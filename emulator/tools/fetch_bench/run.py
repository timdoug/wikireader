#!/usr/bin/env python3
"""Run the controlled fetch probe through real FLASH, Grifo and init."""
import argparse
import json
import os
from pathlib import Path
import sys

from report import validate

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from score_model import ROOT, Runner, sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wremu', type=Path, default=ROOT / 'emulator/wremu')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=900)
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    stage = ROOT / 'build/wr128/fetchq'
    manifest = json.loads((stage / 'manifest.json').read_text())
    for path, key in [(stage / 'fetchq.app', 'app_sha256'), (stage / 'fetchq.ico', 'icon_sha256'),
                      (Path(__file__).with_name('fetch.c'), 'source_sha256'),
                      (stage / 'disassembly.txt', 'disassembly_sha256')]:
        if sha(path) != manifest[key]:
            raise ValueError(f'External-fetch build changed: {path}')
    env = {**os.environ, 'WREMU_BOARD_REV': '7', 'WR_FAT_TIME': '1793836800',
           'WR_FAT_FRAGMENT': '0'}
    env.pop('WREMU_UART_TRACE', None)
    runner = Runner(args, env)
    text = runner.run('fetchq', stage / 'fetchq.app', stage / 'fetchq.ico', 'off', 'fetchq.log', keep_card=True)
    console = (args.out / 'fetchq/boot.log').read_text(errors='replace')
    chain = all(marker in console for marker in ('load: kernel.elf', 'Grifo starting', 'init starting'))
    if (not chain or console.count('Grifo starting') != 1 or
            any(marker in console for marker in ('C33 exception', '[watchdog reset]', 'Panic:'))):
        raise ValueError('Missing actual FLASH/Grifo/init boot')
    header, rows = validate(text, manifest)
    record = dict(complete=True, manifest=manifest, inputs=runner.hashes,
                  emulator_sha256=sha(args.wremu), setting=env.get('WREMU_MODEL', ''),
                  report_sha256=sha(args.out / 'fetchq/fetchq.log'), cases=len(rows),
                  header=header, scope='Native probe execution and raw timing samples; hardware comparison is a separate verified step')
    (args.out / 'results.json').write_text(json.dumps(record, indent=2) + '\n')
    print(f'{len(rows)} controlled external-fetch cases verified: {args.out}')


if __name__ == '__main__':
    main()
