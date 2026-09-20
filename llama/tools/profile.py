#!/usr/bin/env python3
"""Attribute a wremu -F profile to functions using the application's map.

The app is linked stripped, so addr2line has nothing to work with; the link
map has every symbol's address anyway.  Buckets are two bytes wide and
carry instructions, MCLK cycles, fetch-wait cycles and row activations.

The profiler's buckets are cumulative over the whole run, so a window is
usually wanted: --from picks the symbol generation starts at.
"""

import argparse
import re
from pathlib import Path

SYMBOL = re.compile(r"^\s+0x([0-9a-f]{8})\s+([A-Za-z_][A-Za-z0-9_]*)\s*$")


def read_map(path):
    syms = []
    for line in Path(path).read_text().splitlines():
        m = SYMBOL.match(line)
        if m:
            syms.append((int(m.group(1), 16), m.group(2)))
    syms.sort()
    # Collapse duplicate addresses, keeping the first name seen.
    out = []
    for addr, name in syms:
        if not out or out[-1][0] != addr:
            out.append((addr, name))
    return out


def owner(syms, addr):
    lo, hi = 0, len(syms) - 1
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= addr:
            best = syms[mid][1]
            lo = mid + 1
        else:
            hi = mid - 1
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("profile")
    ap.add_argument("mapfile")
    ap.add_argument("--top", type=int, default=20)
    args = ap.parse_args()

    syms = read_map(args.mapfile)
    if not syms:
        raise SystemExit(f"{args.mapfile}: no symbols found")
    lowest = syms[0][0]

    totals = {}
    grand = [0, 0, 0, 0]
    for line in Path(args.profile).read_text().splitlines():
        parts = line.split()
        if len(parts) != 5:
            continue
        addr = int(parts[0], 16)
        insns, cycles, fetch, acts = (int(p) for p in parts[1:])
        grand[0] += insns
        grand[1] += cycles
        grand[2] += fetch
        grand[3] += acts
        if addr < lowest:
            continue
        name = owner(syms, addr)
        t = totals.setdefault(name, [0, 0, 0, 0])
        t[0] += insns
        t[1] += cycles
        t[2] += fetch
        t[3] += acts

    ranked = sorted(totals.items(), key=lambda kv: -kv[1][1])
    app_cycles = sum(v[1] for v in totals.values())
    print(f"{'function':28s} {'cycles':>12s} {'%app':>6s} "
          f"{'insns':>11s} {'cyc/in':>7s} {'fetch%':>7s} {'rowact':>9s}")
    for name, (insns, cycles, fetch, acts) in ranked[: args.top]:
        print(f"{name:28s} {cycles:12d} "
              f"{100.0 * cycles / app_cycles if app_cycles else 0:6.1f} "
              f"{insns:11d} "
              f"{cycles / insns if insns else 0:7.2f} "
              f"{100.0 * fetch / cycles if cycles else 0:7.1f} "
              f"{acts:9d}")
    print(f"\napplication {app_cycles} cycles of {grand[1]} total "
          f"({100.0 * app_cycles / grand[1] if grand[1] else 0:.1f}%)")


if __name__ == "__main__":
    main()
