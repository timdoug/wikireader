#!/usr/bin/env python3
"""Build the placement variants and run them all, then print one table.

The emulator's answer is a modelled guest cycle count, so running the
variants concurrently does not perturb them; the wall clock of the whole
comparison is the slowest single run.
"""
import argparse
import concurrent.futures
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent

VARIANTS = {
    'sdram':      ['FAST=0', 'STATE=0', 'ASM=0'],
    'code':       ['FAST=1', 'STATE=0', 'ASM=0'],
    'code+state': ['FAST=1', 'STATE=1', 'ASM=0'],
    'asm':        ['FAST=1', 'STATE=1', 'ASM=1'],
}


def section_size(mapfile, name):
    match = re.search(rf'^{re.escape(name)}\s+0x\S+\s+(0x\S+)', mapfile, re.M)
    return int(match[1], 16) if match else 0


def build(name, flags, scale):
    # Deleting the objects rather than touching the sources: consecutive
    # builds land inside one filesystem timestamp tick, and make then decides
    # the object is current and silently gives every variant the same binary.
    for stale in ('build/riscv.o', 'build/rv32.o',
                  'lib/libapplication.a', 'riscv.app'):
        (HERE / stale).unlink(missing_ok=True)
    subprocess.run(['make', f'SCALE={scale}', *flags], cwd=HERE, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    mapfile = (HERE / 'riscv.map').read_text()
    got = (section_size(mapfile, '.fastcode') > 0,
           section_size(mapfile, '.fastbss') > 0)
    want = ('FAST=1' in flags or 'ASM=1' in flags, 'STATE=1' in flags)
    if got != want:
        raise SystemExit(f'{name}: built with (code, state) in internal RAM '
                         f'= {got}, asked for {want}')
    target = HERE / 'build' / f'riscv-{name}.app'
    shutil.copyfile(HERE / 'riscv.app', target)
    return target


def run(name, app):
    out = subprocess.run([sys.executable, str(HERE / 'run.py'), '--app', str(app),
                          '--limit', '4000000000', '--timeout', '1800'],
                         cwd=HERE, capture_output=True, text=True)
    return name, out.stdout


def parse(text):
    rows = {}
    for line in text.splitlines():
        m = re.match(r'rv32: (\w+)\s+(\d+) (\d+)\s+([\d.]+)\s+(\d+)$', line)
        if m:
            rows[m[1]] = (int(m[2]), int(m[3]), float(m[4]), int(m[5]))
        m = re.match(r'rv32: total\s+(\d+) insns, (\d+) cycles, ([\d.]+) cyc/insn, (\d+) kIPS', line)
        if m:
            rows['total'] = (int(m[1]), int(m[2]), float(m[3]), int(m[4]))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scale', type=int, default=1)
    args = parser.parse_args()

    apps = {name: build(name, flags, args.scale)
            for name, flags in VARIANTS.items()}
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(apps)) as pool:
        results = dict(pool.map(lambda kv: run(*kv), apps.items()))

    tables = {name: parse(text) for name, text in results.items()}
    missing = [name for name, rows in tables.items() if 'total' not in rows]
    if missing:
        for name in missing:
            print(results[name], file=sys.stderr)
        raise SystemExit(f'no result from: {", ".join(missing)}')

    names = list(VARIANTS)
    kernels = [k for k in tables[names[0]] if k != 'total'] + ['total']
    width = max(len(k) for k in kernels) + 1
    print(f'{"":{width}}' + ''.join(f'{n:>14}' for n in names))
    print(f'{"":{width}}' + ''.join(f'{"cyc/insn":>14}' for _ in names))
    for kernel in kernels:
        row = f'{kernel:{width}}'
        for name in names:
            row += f'{tables[name][kernel][2]:>14.2f}'
        print(row)
    print()
    for name in names:
        total = tables[name]['total']
        print(f'{name:12} {total[3]:6d} kIPS   {total[0]:9d} insns'
              f'   {total[1] / 60e6:7.2f} s of guest time')


if __name__ == '__main__':
    sys.exit(main() or 0)
