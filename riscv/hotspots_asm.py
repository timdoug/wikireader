#!/usr/bin/env python3
"""Attribute the assembly hot path's cycles to the instructions of rv32_hot.s.

hotspots.py answers the same question for the C interpreter, through the
compiler's line table.  The hand-written path has no line table worth the
name -- and would not want one, because every body ends in its own copy of
DISPATCH and the interesting number is what that macro costs summed over all
of them, not what one copy of it costs.

So the map comes from the assembler's own listing, which records the offset
and the expansion of every macro invocation.  Each address is attributed to
the macro it came from and its position inside that macro, which gathers the
thirty-odd copies of DISPATCH back into one row per instruction.

The emulator's profile gives, per two-byte slot: instructions, MCLK cycles,
cycles spent waiting for the fetch, and SDRAM row activations.  Everything is
reported per retired guest instruction, which is the number that has to come
down.
"""
import argparse
import collections
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from hotspots import symbol            # noqa: E402  the window, and why


def listing_map(path):
    """Offset in rv32_hot.o's .fastcode -> (macro, index, text, label).

    A listing line is `<line> <offset> <bytes> <text>`, with `>` marking text
    the assembler produced by expanding a macro.  Continuation lines for a
    long instruction repeat the line number and the bytes but no offset, and
    are skipped: the profile attributes an instruction to its first slot.

    Offsets are per section and both sections here start at zero, so the
    dispatch table in .ivram_code would otherwise be mapped on top of the
    code -- which is how the table's rows first appeared inside A0 RAM.
    """
    source = Path(path).read_text().splitlines()
    macros = set(re.findall(r'^\s*\d+\s+\.macro\s+(\S+)', '\n'.join(source), re.M))

    entry = re.compile(r'^\s*(\d+)\s+([0-9a-f]{4})?\s*'
                       r'((?:[0-9A-F]{4}\s?)+)?\s*(>*)\s*(\S.*)$')
    mapping = {}
    section, label = None, '?'
    stack, index = {}, {}      # macro invoked at each expansion depth
    for line in source:
        hit = entry.match(line)
        if not hit:
            continue
        _, offset, _, markers, text = hit.groups()
        depth, text = len(markers), text.strip()

        directive = re.match(r'\.section\s+(\S+?),', text)
        if directive:
            section, stack = directive[1], {}
            continue

        # A label may share a line with the macro invocation that follows it,
        # and a macro's own expansion may open with one: "8:xjp .Lleave".
        named = re.match(r'^(\.L\w+):\s*(.*)$', text)
        if named:
            label, text = named[1], named[2].strip()
        text = re.sub(r'^\d+:', '', text).strip()
        if not text or text.startswith(';'):
            continue

        head = text.split()[0]
        if head in macros and offset is None:
            stack[depth], index[depth] = head, 0
            continue
        if offset is None or section != '.fastcode':
            continue
        if depth:
            macro = stack.get(depth - 1, '?')
            mapping[int(offset, 16)] = (macro, index.get(depth - 1, 0), text, label)
            index[depth - 1] = index.get(depth - 1, 0) + 1
        else:
            mapping[int(offset, 16)] = (None, 0, text, label)
    return mapping


def fill_gaps(mapping, end):
    """Give every 2-byte slot the key of the instruction that owns it.

    The profile has a bucket per two bytes and counts an `ext` prefix as an
    instruction of its own, so the second and third words of an `xjp` arrive
    at addresses the listing never names.  They belong to the instruction
    they prefix.
    """
    filled, offsets = {}, sorted(mapping)
    for i, offset in enumerate(offsets):
        stop = offsets[i + 1] if i + 1 < len(offsets) else end
        for slot in range(offset, stop, 2):
            filled[slot] = mapping[offset]
    return filled


def fastcode_base(mapfile, obj='rv32_hot.o'):
    match = re.search(r'^ \.fastcode\s+(0x\S+)\s+(0x\S+).*' + re.escape(obj),
                      mapfile, re.M)
    if not match:
        raise SystemExit(f'no .fastcode contribution from {obj} in the map')
    start, size = int(match[1], 16), int(match[2], 16)
    return start, start + size


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', type=Path,
                        help='a profile.txt from a previous run; omit to run one')
    parser.add_argument('--app', type=Path, default=HERE / 'riscv.app')
    parser.add_argument('--insns', type=int,
                        help='retired guest instructions, when --profile is given')
    parser.add_argument('--top', type=int, default=20)
    args = parser.parse_args()

    if args.profile:
        profile, insns = args.profile, args.insns
        if not insns:
            raise SystemExit('--profile needs --insns')
        cpi = None
        cycles = None
    else:
        mapfile = (HERE / 'riscv.map').read_text()
        run = subprocess.run([sys.executable, str(HERE / 'run.py'), '--app', str(args.app),
                              '--limit', '4000000000', '--timeout', '300', '--profile',
                              '--window', f'{symbol(mapfile, "rv32_run")},'
                                          f'{symbol(mapfile, "power_off")}'],
                             cwd=HERE, capture_output=True, text=True)
        found = re.search(r'^profile: (.*)$', run.stdout, re.M)
        guest = re.search(r'rv32: total\s+(\d+) insns, (\d+) cycles, ([\d.]+) cyc/insn',
                          run.stdout)
        if not found or not guest:
            sys.stdout.write(run.stdout)
            raise SystemExit('run produced no profile')
        profile = Path(found[1])
        insns, cycles, cpi = int(guest[1]), int(guest[2]), float(guest[3])

    lo, hi = fastcode_base((HERE / 'riscv.map').read_text())
    mapping = fill_gaps(listing_map(HERE / 'build/rv32_hot.lst'), hi - lo)

    rows = []
    for line in Path(profile).read_text().split('\n'):
        if line.strip():
            addr, n, clk, fetch, act = line.split()
            rows.append((int(addr, 16), int(n), int(clk), int(fetch), int(act)))

    # Per instruction of the source, gathering every copy of a macro.
    agg = collections.defaultdict(lambda: [0, 0, 0, 0])
    groups = collections.defaultdict(lambda: [0, 0, 0, 0])
    total = [0, 0, 0, 0]
    unmapped = [0, 0]
    for addr, n, clk, fetch, act in rows:
        if not lo <= addr < hi:
            continue
        where = mapping.get(addr - lo)
        if where is None:
            unmapped[0] += n
            unmapped[1] += clk
            continue
        macro, index, text, label = where
        key = (macro, index, text) if macro else (label, index, text)
        group = macro if macro else 'bodies'
        for i, v in enumerate((n, clk, fetch, act)):
            agg[key][i] += v
            groups[group][i] += v
            total[i] += v

    print(f'{insns} retired guest instructions'
          + (f', {cpi:.2f} cyc/insn overall' if cpi else ''))
    print(f'rv32_hot.o .fastcode is {lo:#x}..{hi:#x}, '
          f'{total[0] / insns:.1f} host insns and {total[1] / insns:.1f} cycles '
          f'per guest instruction inside it')
    if unmapped[0]:
        print(f'unmapped: {unmapped[0]} insns, {unmapped[1]} cycles')

    print(f'\n{"group":12}{"insns":>8}{"cycles":>9}{"fetch":>8}{"rows":>7}{"share":>8}')
    for key, (n, clk, fetch, act) in sorted(groups.items(), key=lambda kv: -kv[1][1]):
        print(f'{key:12}{n / insns:8.2f}{clk / insns:9.2f}{fetch / insns:8.2f}'
              f'{act / insns:7.2f}{100 * clk / total[1]:7.1f}%')

    print(f'\nper source instruction, all copies summed '
          f'(cycles and fetch-wait per retired guest instruction):')
    print(f'{"where":22}{"insns":>7}{"cycles":>8}{"cyc/ex":>8}{"fetch":>7}{"rows":>7}  text')
    for key, (n, clk, fetch, act) in sorted(agg.items(), key=lambda kv: -kv[1][1])[:args.top]:
        where = f'{key[0]}+{key[1]}' if key[0] else '?'
        print(f'{where:22}{n / insns:7.2f}{clk / insns:8.2f}'
              f'{clk / n if n else 0:8.2f}{fetch / insns:7.2f}{act / insns:7.2f}  {key[2][:40]}')


if __name__ == '__main__':
    main()
