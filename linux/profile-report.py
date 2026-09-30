#!/usr/bin/env python3
"""Attribute runtime-bench.py PC cycles using guest maps and unstripped ELF files."""
import argparse
import bisect
from collections import defaultdict
from pathlib import Path
import re
import struct

ROOT = Path(__file__).resolve().parent.parent


def elf_symbols(path):
    data = path.read_bytes()
    if data[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError(f'{path}: expected little-endian ELF32')
    h = struct.unpack_from('<HHIIIIIHHHHHH', data, 16)
    sections = [struct.unpack_from('<10I', data, h[5] + i * h[10]) for i in range(h[11])]
    names = sections[h[12]]
    strings = data[names[4]:names[4] + names[5]]
    section_names = [strings[s[0]:strings.index(0, s[0])].decode() for s in sections]
    functions = {}
    for s in sections:
        if s[1] != 2:  # SHT_SYMTAB
            continue
        names = sections[s[6]]
        strings = data[names[4]:names[4] + names[5]]
        for offset in range(s[4], s[4] + s[5], s[9]):
            name, address, size, info, other, index = struct.unpack_from('<IIIBBH', data, offset)
            if info & 15 == 2 and index:
                functions.setdefault(address, (strings[name:strings.index(0, name)].decode(), size))
    loads = [struct.unpack_from('<8I', data, h[4] + i * h[8]) for i in range(h[9])]
    return sections, section_names, functions, sorted(functions), loads


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('profile', type=Path)
    parser.add_argument('--binary', type=Path, default=ROOT / 'linux/artifacts/runtime-bench')
    parser.add_argument('--symbols', type=Path, default=ROOT / 'linux/artifacts/symbols')
    parser.add_argument('--iterations', type=int, default=10000)
    args = parser.parse_args()
    maps = []
    for line in (args.profile / 'profile.log').read_text().splitlines():
        m = re.match(r'BENCH MAP ([0-9a-f]+)-([0-9a-f]+) (\S+) ([0-9a-f]+) \S+ \d+[ \t]+(/\S+)', line)
        if not m:
            continue
        start, end, flags, offset, name = m.groups()
        if 'x' not in flags:
            continue
        path = args.binary if name == '/mnt/sd/bench.bin' else args.symbols / name.lstrip('/')
        if path.is_file():
            maps.append((int(start, 16), int(end, 16), int(offset, 16), name, elf_symbols(path)))
    totals = defaultdict(lambda: [0, 0, 0, 0])
    for line in (args.profile / 'pc.txt').read_text().splitlines():
        address, *counts = line.split()
        pc = int(address, 16)
        name = 'kernel/internal RAM/other'
        for start, end, offset, module, elf in maps:
            if not start <= pc < end:
                continue
            sections, section_names, funcs, starts, loads = elf
            file_offset = offset + pc - start
            load = next((p for p in loads if p[0] == 1 and p[1] <= file_offset < p[1] + p[4]), None)
            if load is None:
                break
            relative = load[2] + file_offset - load[1]
            name = module
            for s, label in zip(sections, section_names):
                if s[3] <= relative < s[3] + s[5] and s[2] & 4:
                    name = f'{module}:{label}'
                    break
            i = bisect.bisect_right(starts, relative) - 1
            if i >= 0 and relative < starts[i] + funcs[starts[i]][1]:
                name = funcs[starts[i]][0]
            break
        for i, count in enumerate(counts):
            totals[name][i] += int(count)
    cycles = sum(c[1] for c in totals.values())
    if not cycles or args.iterations <= 0:
        raise SystemExit('Expected nonempty profile and positive iteration count')
    print(' share   cycles/op   insns/op   fetch wait/op   function or section')
    for name, c in sorted(totals.items(), key=lambda item: -item[1][1])[:25]:
        print(f'{100*c[1]/cycles:5.1f}% {c[1]/args.iterations:11.1f} {c[0]/args.iterations:10.1f} '
              f'{c[2]/args.iterations:15.1f}   {name}')
    print(f'Total: {cycles / args.iterations:.1f} cycles/op')


if __name__ == '__main__':
    main()
