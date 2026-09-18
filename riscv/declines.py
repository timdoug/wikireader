#!/usr/bin/env python3
"""What a declined guest instruction costs, and which function spends it.

The assembly hot path gives up on device addresses, CSRs, atomics and
misaligned accesses; `make mix` says a Linux boot does that once every 78
instructions, and hotspots_asm.py says the round trip is about 885 cycles and
fourteen percent of the boot.  This says where those cycles go, which decides
whether the fix is to keep the common cases out of C or to make the trip
itself cheaper.

Everything outside the interpreter's internal-RAM sections is attributed to
the nearest preceding symbol in riscv.map -- the application's own, not just
grifo's, because rv32_interpret and the MMIO helpers are the suspects.
"""
import argparse
import bisect
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from hotspots_asm import listing_map, fill_gaps, fastcode_base   # noqa: E402


OBJDUMP = HERE.parent / 'host-tools/toolchain-c33/work/install/bin/c33-epson-elf-objdump'


def symbols(mapfile):
    """Every function, as a sorted list of (address, name).

    The map only names globals, and the ones that matter here are not:
    rv32_interpret and the MMIO helpers are static, so a nearest-preceding
    lookup against the map alone attributes the whole C interpreter to
    whatever global happens to sit below it -- which first reported 46% of
    the cost inside grifo_main.  So the sections come from the map and the
    symbols inside them from the objects the map says they came from.
    """
    import subprocess
    found = {}
    # The kernel is at the base of SDRAM and the application above it, so
    # everything below 0x10040000 belongs to grifo and the application's map
    # says nothing about it.  Without these, a nearest-preceding lookup put
    # grifo's serial busy-wait inside libgcc's divide.
    nm = str(OBJDUMP).replace('objdump', 'nm')
    listing = subprocess.run([nm, '-n', str(HERE.parent / 'samo-lib/grifo/grifo.elf')],
                             capture_output=True, text=True).stdout
    for line in listing.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in 'tTwW':
            found.setdefault(int(parts[0], 16), 'grifo:' + parts[2])
    for address, name in re.findall(r'^\s+(0x[0-9a-f]{8})\s+(\w+)$', mapfile, re.M):
        found.setdefault(int(address, 16), name)
    for section, base, _, obj in re.findall(
            r'^ (\.\w+)\s+(0x[0-9a-f]{8})\s+(0x[0-9a-f]+) (\S+)$', mapfile, re.M):
        path = HERE / re.sub(r'.*\((\w+\.o)\)', r'build/\1', obj)
        if not path.exists():
            continue
        listing = subprocess.run([str(OBJDUMP), '-t', str(path)],
                                 capture_output=True, text=True).stdout
        for line in listing.splitlines():
            hit = re.match(r'^([0-9a-f]{8})\s+\S+\s+F\s+(\S+)\s+[0-9a-f]+\s+(\S+)$',
                           line)
            if hit and hit[2] == section:
                found[int(base, 16) + int(hit[1], 16)] = hit[3]
    return sorted(found.items())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('profile', type=Path)
    parser.add_argument('--top', type=int, default=16)
    parser.add_argument('--declines', type=float, default=1 / 78,
                        help='declines per guest instruction, from `make mix`')
    args = parser.parse_args()

    mapfile = (HERE / 'riscv.map').read_text()
    table = symbols(mapfile)
    addresses = [a for a, _ in table]

    rows = []
    for line in args.profile.read_text().split('\n'):
        if line.strip():
            addr, n, clk, fetch, act = line.split()
            rows.append((int(addr, 16), int(n), int(clk), int(act)))

    # The guest instruction count comes from the hot path itself: the first
    # instruction of DISPATCH runs exactly once per retired guest instruction.
    lo, hi = fastcode_base(mapfile)
    hot = fill_gaps(listing_map(HERE / 'build/rv32_hot.lst'), hi - lo)
    by_address = {a: (n, clk) for a, n, clk, _ in rows}
    guest = sum(by_address.get(lo + off, (0, 0))[0]
                for off, (macro, i, _, _) in hot.items()
                if macro == 'DISPATCH' and i == 0)

    spent = {}
    total = [0, 0]
    for addr, n, clk, act in rows:
        if lo <= addr < hi or addr < 0x1000:
            continue            # the hot path itself, and grifo's vectors
        index = bisect.bisect_right(addresses, addr) - 1
        name = table[index][1] if index >= 0 else f'{addr:08x}'
        at = spent.setdefault(name, [0, 0, 0])
        at[0] += n
        at[1] += clk
        at[2] += act
        total[0] += n
        total[1] += clk

    per_decline = args.declines * guest
    print(f'{guest} guest instructions, about {per_decline:.0f} declines at '
          f'one in {1 / args.declines:.0f}')
    print(f'outside the hot path: {total[0] / guest:.2f} host instructions and '
          f'{total[1] / guest:.2f} cycles per guest instruction, '
          f'{total[1] / per_decline:.0f} cycles per decline\n')
    print(f'{"function":28}{"insns/dec":>10}{"cyc/dec":>9}{"cyc/guest":>11}{"share":>8}')
    for name, (n, clk, act) in sorted(spent.items(), key=lambda kv: -kv[1][1])[:args.top]:
        print(f'{name:28}{n / per_decline:10.1f}{clk / per_decline:9.1f}'
              f'{clk / guest:11.2f}{100 * clk / total[1]:7.1f}%')


if __name__ == '__main__':
    main()
