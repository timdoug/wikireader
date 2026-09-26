#!/usr/bin/env python3
"""Attribute a `run.py --profile` run's cycles to functions and source lines.

The application is linked stripped, so symbols come from the objects: each
object's .text base from gameboy.map, and its functions, static ones
included, from the object's own symbol table.
"""
import argparse
import collections
from pathlib import Path
import re
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
BIN = ROOT / 'host-tools/toolchain-c33/work/install/bin'


def symbols():
    mapfile = (HERE / 'gameboy.map').read_text()
    spans = []
    for base, size, obj in re.findall(
            r'^ \.text\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S+)$', mapfile, re.M):
        base, size = int(base, 16), int(size, 16)
        path = re.sub(r'.*\((.*)\)$', r'\1', obj)
        local = HERE / 'build' / Path(path).name
        names = []
        if local.exists():
            table = subprocess.run([str(BIN / 'c33-epson-elf-nm'), '-n', str(local)],
                                   capture_output=True, text=True).stdout
            for line in table.splitlines():
                parts = line.split()
                if len(parts) == 3 and parts[1] in 'tT':
                    names.append((base + int(parts[0], 16), parts[2]))
        if not names:
            names.append((base, Path(path).name))
        spans.append((base, base + size, names))
    for address, name in re.findall(r'^\s+(0x[0-9a-f]+)\s+([A-Za-z_]\w*)$', mapfile, re.M):
        address = int(address, 16)
        if not any(lo <= address < hi for lo, hi, _ in spans):
            spans.append((address, address + 1, [(address, name)]))
    flat = sorted(n for _, _, names in spans for n in names)
    return flat


def lines_for(obj, base):
    dump = subprocess.run([str(BIN / 'c33-epson-elf-objdump'), '-dl', '--section=.text', str(obj)],
                          capture_output=True, text=True).stdout
    mapping, current = {}, '?'
    for line in dump.splitlines():
        source = re.match(r'^(?:\S*/)?([\w.]+\.[ch]:\d+)', line)
        if source:
            current = source[1]
            continue
        code = re.match(r'^\s+([0-9a-f]+):', line)
        if code:
            mapping[base + int(code[1], 16)] = current
    return mapping


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path, default=HERE / 'build/profile.txt')
    parser.add_argument('--top', type=int, default=25)
    parser.add_argument('--lines', action='store_true',
                        help='attribute gb.c by source line instead of function')
    args = parser.parse_args()

    table = symbols()
    starts = [a for a, _ in table]
    import bisect
    rows = []
    for line in args.profile.read_text().splitlines():
        fields = line.split()
        if len(fields) == 5:
            rows.append([int(fields[0], 16)] + [int(f) for f in fields[1:]])
    line_of = {}
    if args.lines:
        mapfile = (HERE / 'gameboy.map').read_text()
        base = int(re.search(r'^ \.text\s+(0x[0-9a-f]+)\s+\S+\s+\S*\(gb\.o\)$', mapfile, re.M)[1], 16)
        line_of = lines_for(HERE / 'build/gb.o', base)

    total = collections.Counter()
    by = collections.defaultdict(collections.Counter)
    for address, insns, cycles, fetch, rows_opened in rows:
        if args.lines and address in line_of:
            key = line_of[address]
        else:
            i = bisect.bisect_right(starts, address) - 1
            key = table[i][1] if i >= 0 else hex(address)
            if address < 0x10000000:
                key = f'internal {hex(address)}'
        for name, value in (('insns', insns), ('cycles', cycles), ('fetch', fetch), ('rows', rows_opened)):
            by[key][name] += value
            total[name] += value
    print(f'{"":36} {"cycles":>7} {"insns":>7} {"CPI":>5} {"fetch%":>6} {"rows":>9}')
    for key, c in sorted(by.items(), key=lambda kv: -kv[1]['cycles'])[:args.top]:
        print(f'{key[:36]:36} {100 * c["cycles"] / total["cycles"]:6.1f}% '
              f'{100 * c["insns"] / total["insns"]:6.1f}% {c["cycles"] / max(1, c["insns"]):5.2f} '
              f'{100 * c["fetch"] / max(1, c["cycles"]):5.1f}% {c["rows"]:9d}')
    print(f'total {total["cycles"]} cycles, {total["insns"]} instructions, '
          f'CPI {total["cycles"] / total["insns"]:.2f}, {total["rows"]} row activations')


if __name__ == '__main__':
    main()
