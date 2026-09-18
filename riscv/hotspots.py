#!/usr/bin/env python3
"""Attribute the interpreter's cycles to source lines.

The emulator's per-address profile gives instructions, MCLK cycles and SDRAM
row activations per bucket; addr2line maps each bucket back through the
object's debug info. Everything is reported per retired guest instruction,
which is the number that has to come down.
"""
import argparse
import collections
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
OBJDUMP = ROOT / 'host-tools/toolchain-c33/work/install/bin/c33-epson-elf-objdump'


def line_map(obj):
    """Offset in .fastcode -> source line.

    addr2line is no use here: .text and .fastcode both start at zero in the
    object, so an offset is ambiguous and it answers with whichever section
    it finds first. Disassembling the one section is unambiguous.
    """
    dump = subprocess.run([str(OBJDUMP), '-dl', '--section=.fastcode', str(obj)],
                          capture_output=True, text=True).stdout
    mapping, current = {}, '?'
    for line in dump.splitlines():
        source = re.match(r'^(?:\S*/)?(rv32\.[ch]:\d+)', line)
        if source:
            current = source[1]
            continue
        code = re.match(r'^\s+([0-9a-f]+):', line)
        if code:
            mapping[int(code[1], 16)] = current
    return mapping


def symbol(mapfile, name):
    """A global's link address, out of the map file.

    The profile has to be windowed to the run, and these are what bracket
    it.  Without a window it covers the whole boot -- and grifo runs its own
    code from the same A0 RAM the interpreter is later loaded into, so its
    SPI loop and the hot path share addresses and the profile cannot tell
    them apart.  That put 2.2 million executions of grifo's card reader onto
    four instructions of the dispatch macro, and made the interpreter look
    twenty percent dearer than the run it was measured in.
    """
    match = re.search(r'^\s+(0x[0-9a-f]+)\s+' + re.escape(name) + r'$',
                      mapfile, re.M)
    if not match:
        raise SystemExit(f'no {name} in riscv.map')
    return match[1]


def fastcode_span(mapfile):
    match = re.search(r'^\.fastcode\s+(0x\S+)\s+(0x\S+)', mapfile, re.M)
    start, size = int(match[1], 16), int(match[2], 16)
    return start, start + size


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, default=HERE / 'riscv.app')
    parser.add_argument('--top', type=int, default=16)
    args = parser.parse_args()

    mapfile = (HERE / 'riscv.map').read_text()
    window = f'{symbol(mapfile, "rv32_run")},{symbol(mapfile, "power_off")}'
    run = subprocess.run([sys.executable, str(HERE / 'run.py'), '--app', str(args.app),
                          '--limit', '4000000000', '--timeout', '300', '--profile',
                          '--window', window],
                         cwd=HERE, capture_output=True, text=True)
    profile = re.search(r'^profile: (.*)$', run.stdout, re.M)
    guest = re.search(r'rv32: total\s+(\d+) insns, (\d+) cycles, ([\d.]+) cyc/insn', run.stdout)
    if not profile or not guest:
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
        raise SystemExit('run produced no profile')
    insns, cycles, cpi = int(guest[1]), int(guest[2]), float(guest[3])

    lo, hi = fastcode_span(mapfile)
    rows = []
    for line in Path(profile[1]).read_text().split('\n'):
        if line.strip():
            addr, n, clk, fetch, act = line.split()
            rows.append((int(addr, 16), int(n), int(clk), int(fetch), int(act)))
    hot = [r for r in rows if lo <= r[0] < hi]

    mapping = line_map(HERE / 'build/rv32.o')
    agg = collections.defaultdict(lambda: [0, 0, 0])
    for row in hot:
        key = mapping.get(row[0] - lo, '?')
        agg[key][0] += row[1]
        agg[key][1] += row[2]
        agg[key][2] += row[4]

    # Where the cycles outside .fastcode go.  Grifo's own address range
    # covers the loader's .bss clear and the power-off spin, which bracket
    # the measured window rather than falling inside it.
    outside = collections.defaultdict(lambda: [0, 0])
    symbols = {}
    for name, path in (('grifo', ROOT / 'samo-lib/grifo/grifo.elf'),):
        listing = subprocess.run(
            [str(OBJDUMP).replace('objdump', 'nm'), '-n', str(path)],
            capture_output=True, text=True).stdout
        for entry in listing.splitlines():
            parts = entry.split()
            if len(parts) == 3:
                symbols[int(parts[0], 16)] = parts[2]
    ordered = sorted(symbols)
    import bisect
    for row in rows:
        if lo <= row[0] < hi:
            continue
        if ordered and row[0] >= ordered[0]:
            index = bisect.bisect_right(ordered, row[0]) - 1
            key = symbols[ordered[index]]
        else:
            key = f'{row[0] & ~0xfff:08x}'
        outside[key][0] += row[1]
        outside[key][1] += row[2]

    source = (HERE / 'rv32.c').read_text().split('\n')
    inside = sum(v[1] for v in agg.values())
    print(f'{insns} guest instructions, {cpi:.2f} cyc/insn overall; '
          f'{inside / insns:.1f} of those are in .fastcode, '
          f'{(cycles - inside) / insns:.1f} outside it')
    print(f'{"line":12}{"insns":>8}{"cycles":>9}{"rows":>8}{"share":>8}  source')
    for key, (i, c, act) in sorted(agg.items(), key=lambda kv: -kv[1][1])[:args.top]:
        number = int(key.split(':')[1]) if ':' in key else 0
        text = source[number - 1].strip()[:46] if number else ''
        print(f'{key:12}{i / insns:8.1f}{c / insns:9.1f}{act / insns:8.2f}'
              f'{100 * c / inside:7.1f}%  {text}')
    print(f'\noutside .fastcode (includes startup and shutdown, which are '
          f'not in the measured window):')
    for key, (i, c) in sorted(outside.items(), key=lambda kv: -kv[1][1])[:10]:
        print(f'  {key:28}{i:12d} insns{c:12d} cycles{c / insns:9.1f} per guest insn')


if __name__ == '__main__':
    main()
