#!/usr/bin/env python3
"""Where the cycles go inside the code cache, instruction by instruction.

The translator's output has no symbols and no source lines, so the ordinary
profile tools cannot say anything about it.  This runs the benchmark twice:
once to learn where the code cache landed, and once more with the profile
windowed to the run and the cache dumped from memory when the run ends.  The
dump goes through the toolchain's disassembler and each instruction is shown
with the cycles the profile charged it, grouped into the runs the profile hit,
hottest first.

    python3 jitprof.py                 the whole cache, hottest runs first
    python3 jitprof.py --top 6         only that many runs
    python3 jitprof.py --app X.app     a particular build
    python3 jitprof.py --boot          the Linux boot instead of rvbench
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
BIN = ROOT / 'host-tools/toolchain-c33/work/install/bin'


def symbol(mapfile, name):
    match = re.search(r'^\s+(0x[0-9a-f]+)\s+' + re.escape(name) + r'$',
                      mapfile, re.M)
    if not match:
        raise SystemExit(f'no {name} in riscv.map')
    return match[1]


def run(app, extra, boot):
    if boot:
        cmd = [sys.executable, str(HERE / 'boot.py'), '--app', str(app), *extra]
    else:
        cmd = [sys.executable, str(HERE / 'run.py'), '--app', str(app),
               '--limit', '4000000000', '--timeout', '300', *extra]
    out = subprocess.run(cmd, cwd=HERE, capture_output=True, text=True)
    where = re.search(r'^full log: (.*)$', out.stdout, re.M)
    if not where:
        sys.stdout.write(out.stdout)
        sys.stderr.write(out.stderr)
        raise SystemExit('the run did not finish')
    return out.stdout, Path(where[1]).parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, default=HERE / 'riscv.app')
    parser.add_argument('--top', type=int, default=12)
    parser.add_argument('--gap', type=int, default=8,
                        help='bytes of silence that end a run')
    parser.add_argument('--boot', action='store_true',
                        help='profile the Linux boot, to the shell')
    parser.add_argument('--from', dest='again', type=Path,
                        help='re-read the profile and dump of an earlier run')
    args = parser.parse_args()

    mapfile = (HERE / 'riscv.map').read_text()
    window = f'{symbol(mapfile, "rv32_run")},{symbol(mapfile, "power_off")}'

    if args.again:
        out = args.again
        text = (out / 'result.txt').read_text()
    else:
        text, out = run(args.app, [], args.boot)
    log = (out / 'run.log').read_text(errors='replace')
    cache = re.search(r'rv32: jit code at ([0-9a-f]+), (\d+) bytes', log)
    # A boot prints its statistics every million instructions; the last line
    # is the one with the whole cache in it.
    used = re.findall(r'rv32: jit \d+ blocks, (\d+) bytes', log)
    if not cache:
        raise SystemExit('the run did not say where its code cache is')
    base = int(cache[1], 16)
    length = (out / 'dump.bin').stat().st_size if args.again else \
             int(used[-1]) + 64 if used else int(cache[2])

    if not args.again:
        text, out = run(args.app, ['--profile', '--window', window,
                                   '--dump', f'0x{base:x},{length}'], args.boot)
        (out / 'result.txt').write_text(text)
    if args.boot:
        total = re.search(r': (\d+) guest insns, (\d+) cycles', text)
    else:
        total = re.search(r'rv32: total\s+(\d+) insns, (\d+) cycles', text)
    guest_insns, guest_cycles = int(total[1]), int(total[2])
    print(f'run: {out}')

    cycles, hits, outside = {}, {}, []
    for line in (out / 'profile.txt').read_text().split('\n'):
        if line.strip():
            addr, n, clk, fetch, act = line.split()
            addr = int(addr, 16)
            if base <= addr < base + length:
                cycles[addr] = int(clk)
                hits[addr] = int(n)
            else:
                outside.append((addr, int(clk)))
    in_cache = sum(cycles.values())

    # Everything else, by the nearest symbol below it in the map file and in
    # grifo's.  Translation, interpretation and the C helpers all live here.
    symbols = []
    for m in re.finditer(r'^\s+(0x[0-9a-f]+)\s+([A-Za-z_.][\w.]*)$', mapfile, re.M):
        symbols.append((int(m[1], 16), m[2]))
    nm = subprocess.run([str(BIN / 'c33-epson-elf-nm'), '-n',
                         str(ROOT / 'samo-lib/grifo/grifo.elf')],
                        capture_output=True, text=True).stdout
    for line in nm.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in 'tTwW':
            symbols.append((int(parts[0], 16), 'grifo:' + parts[2]))
    # The map names only globals.  The translator and the C interpreter are
    # static, and without them their cycles land on whatever global precedes
    # them: each object's code sections from the map, plus the object's own
    # symbol table, which says which section each symbol is in.
    bases = {}
    for m in re.finditer(r'^ \.(text|fastcode|ivram_code)\s+(0x[0-9a-f]+)\s+0x[0-9a-f]+\s+\S*\((\w+\.o)\)$',
                         mapfile, re.M):
        bases[(m[3], '.' + m[1])] = int(m[2], 16)
    for obj in {o for o, _ in bases}:
        listing = subprocess.run([str(BIN / 'c33-epson-elf-objdump'), '-t',
                                  str(HERE / 'build' / obj)],
                                 capture_output=True, text=True).stdout
        for line in listing.splitlines():
            m = re.match(r'^([0-9a-f]{8})\s+\S+\s+F\s+(\S+)\s+[0-9a-f]+\s+(\S+)$', line)
            if m and (obj, m[2]) in bases:
                symbols.append((bases[(obj, m[2])] + int(m[1], 16),
                                f'{obj[:-2]}:{m[3]}'))
    symbols.sort()
    import bisect
    keys = [a for a, _ in symbols]
    by_symbol, span = {}, {}
    for addr, clk in outside:
        i = bisect.bisect_right(keys, addr) - 1
        name = symbols[i][1] if i >= 0 else '?'
        by_symbol[name] = by_symbol.get(name, 0) + clk
        lo, hi = span.get(name, (addr, addr))
        span[name] = (min(lo, addr), max(hi, addr))

    dump = subprocess.run([str(BIN / 'c33-epson-elf-objdump'), '-D', '-b', 'binary',
                           '-m', 'c33', f'--adjust-vma=0x{base:x}',
                           str(out / 'dump.bin')],
                          capture_output=True, text=True).stdout
    insns = []
    for line in dump.splitlines():
        m = re.match(r'^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)$', line)
        if m:
            insns.append((int(m[1], 16), m[3].strip()))

    # Runs: maximal stretches of instructions the profile hit, split where
    # more than --gap bytes went unexecuted.
    runs, current, last = [], [], None
    for addr, asm in insns:
        if addr in cycles:
            if last is not None and addr - last > args.gap and current:
                runs.append(current)
                current = []
            current.append((addr, asm))
            last = addr
    if current:
        runs.append(current)
    runs.sort(key=lambda r: -sum(cycles[a] for a, _ in r))

    print(f'guest: {guest_insns} insns, {guest_cycles} cycles, '
          f'{guest_cycles / guest_insns:.2f} cyc/insn; '
          f'{in_cache / guest_cycles * 100:.1f}% of cycles inside the code cache '
          f'({in_cache / guest_insns:.2f} cyc/insn)')
    print('outside it, by symbol:')
    for name, clk in sorted(by_symbol.items(), key=lambda kv: -kv[1])[:args.top]:
        lo, hi = span[name]
        print(f'  {clk:>12}  {clk / guest_insns:6.2f} cyc/insn  {name} '
              f'[{lo:08x}..{hi:08x}]')
    for r in runs[:args.top]:
        clk = sum(cycles[a] for a, _ in r)
        print(f'\n--- {r[0][0]:08x}..{r[-1][0]:08x}: {clk} cycles, '
              f'{clk / guest_cycles * 100:.1f}% of the run')
        for addr, asm in r:
            n = hits[addr]
            print(f'{addr:08x}  {cycles[addr]:>10}  {cycles[addr] / n:6.2f}  '
                  f'{n:>9}  {asm}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
