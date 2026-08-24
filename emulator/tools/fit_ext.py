#!/usr/bin/env python3
"""
Determine how `ext` prefixes compose into a branch displacement.

objdump prints both the raw field and the resolved target for extended
branches:

    1000001e: 1c11  call 0x11   xcall 0x22 (0x10000040) <process>

so for every ext-prefixed branch in a real image we know the answer and can
check a candidate rule against all of them at once.
"""

import re
import sys
from pathlib import Path

INSN = re.compile(r"^\s*([0-9a-f]+):\s+([0-9a-f]{4})\s+(\S+)\s+(\S+)"
                  r"(?:\s+x\S+\s+(\S+)\s+\((0x[0-9A-Fa-f]+)\))?")

BRANCH = {"call", "call.d", "jp", "jp.d"} | {
    f"jr{c}{d}" for c in ("eq", "ne", "gt", "ge", "lt", "le",
                          "ugt", "uge", "ult", "ule") for d in ("", ".d")}


def parse(path):
    """-> list of (addr, exts, base, mnemonic, target)"""
    out, pending = [], []
    for line in Path(path).read_text().splitlines():
        m = INSN.match(line)
        if not m:
            continue
        addr = int(m.group(1), 16)
        mnem = m.group(3)
        raw = m.group(4)
        target = m.group(6)

        if mnem == "ext":
            pending.append(int(raw, 0))
            continue
        if mnem in BRANCH and target is not None:
            try:
                base = int(raw, 0)
            except ValueError:
                pending = []
                continue
            out.append((addr, list(pending), base, mnem, int(target, 16)))
        pending = []
    return out


def sext(v, bits):
    if bits < 32 and (v >> (bits - 1)) & 1:
        v -= 1 << bits
    return v


def candidate(name, fn, samples):
    good = bad = 0
    first = None
    for addr, exts, base, mnem, target in samples:
        got = fn(addr, exts, base)
        if got == target:
            good += 1
        else:
            bad += 1
            if first is None:
                first = (addr, exts, base, mnem, got, target)
    tag = "OK " if bad == 0 else "   "
    print(f"{tag}{name:<42} {good:5d} ok  {bad:5d} bad")
    if first and bad:
        a, e, b, mn, got, want = first
        print(f"      e.g. {a:08x} {mn} exts={[hex(x) for x in e]} "
              f"base={b:#x} -> {got:#010x} want {want:#010x}")
    return bad == 0


def main():
    samples = parse(sys.argv[1] if len(sys.argv) > 1 else "grifo-full.txt")
    withext = [s for s in samples if s[1]]
    print(f"branches with resolved targets: {len(samples)} "
          f"({len(withext)} ext-prefixed)\n")

    W = 8

    def rule_first_high(addr, exts, base):
        if not exts:
            v = sext(base, 8)
        elif len(exts) == 1:
            v = sext((exts[0] << W) | base, W + 13)
        else:
            v = sext((exts[0] << (W + 13)) | (exts[1] << W) | base, 32)
        return (addr + (v << 1)) & 0xFFFFFFFF

    def rule_first_low(addr, exts, base):
        if not exts:
            v = sext(base, 8)
        elif len(exts) == 1:
            v = sext((exts[0] << W) | base, W + 13)
        else:
            v = sext((exts[1] << (W + 13)) | (exts[0] << W) | base, 32)
        return (addr + (v << 1)) & 0xFFFFFFFF

    def rule_bytes(addr, exts, base):
        """displacement already in bytes, no <<1"""
        if not exts:
            v = sext(base, 8)
        elif len(exts) == 1:
            v = sext((exts[0] << W) | base, W + 13)
        else:
            v = sext((exts[0] << (W + 13)) | (exts[1] << W) | base, 32)
        return (addr + v) & 0xFFFFFFFF

    candidate("first ext high, <<1", rule_first_high, samples)
    candidate("first ext low,  <<1", rule_first_low, samples)
    candidate("first ext high, no shift", rule_bytes, samples)
    return 0


if __name__ == "__main__":
    sys.exit(main())
