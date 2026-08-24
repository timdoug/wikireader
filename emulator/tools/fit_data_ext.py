#!/usr/bin/env python3
"""
Fit how ext prefixes compose into *data* immediates (as opposed to the
PC-relative branch displacements handled by fit_ext.py).

objdump prints both the raw field and the resolved value for extended data
operations:

    6fcf  ld.w %r15,0x3c   xld.w %r15,0x11fffffc <__MAIN_STACK>

so every ext-prefixed site in a real image is a labelled example. The branch
rule turned out to be shift 18 / 29 bits rather than the obvious width+13, so
this must be measured rather than assumed.
"""

import re
import sys
from collections import defaultdict
from pathlib import Path

LINE = re.compile(r"^\s*[0-9a-f]+:\s+([0-9a-f]{4})\s+(\S+)\s+(\S+)"
                  r"(?:\s+x(\S+)\s+(\S+))?")
HEX = re.compile(r"^0x[0-9a-fA-F]+$")


def last_imm(ops):
    """Trailing immediate of an operand list, or None."""
    tok = ops.split(",")[-1]
    tok = tok.split("[")[-1].rstrip("]")
    return int(tok, 0) if HEX.match(tok) else None


def main():
    src = Path(sys.argv[1] if len(sys.argv) > 1 else "grifo-full.txt")
    samples = defaultdict(list)   # (mnemonic, n_ext) -> [(exts, base, want)]
    pend = []

    for line in src.read_text().splitlines():
        m = LINE.match(line)
        if not m:
            continue
        _enc, mnem, ops, xmnem, xops = m.groups()

        if mnem == "ext":
            pend.append(int(ops, 0))
            continue
        if xmnem and xops and pend:
            base = last_imm(ops)
            want = last_imm(xops)
            if base is not None and want is not None:
                samples[(mnem, len(pend))].append((list(pend), base, want))
        pend = []

    print(f"{'mnemonic':<10} {'exts':>4} {'n':>5}  rule")
    print("-" * 62)
    for (mnem, ne), rows in sorted(samples.items()):
        found = None
        for T in range(6, 34):
            for s1 in range(0, 30):
                for s2 in range(0, 30):
                    ok = True
                    for exts, base, want in rows:
                        v = base
                        if ne >= 1:
                            v |= exts[0] << s1
                        if ne >= 2:
                            v |= exts[1] << s2
                        v &= (1 << T) - 1
                        if v >> (T - 1):
                            v -= 1 << T
                        if (v & 0xFFFFFFFF) != (want & 0xFFFFFFFF):
                            ok = False
                            break
                    if ok:
                        found = (T, s1, s2)
                        break
                if found:
                    break
            if found:
                break
        if found:
            T, s1, s2 = found
            desc = (f"width {T}, ext1<<{s1}" +
                    (f", ext2<<{s2}" if ne >= 2 else ""))
        else:
            desc = "NO FIT"
        print(f"{mnem:<10} {ne:>4} {len(rows):>5}  {desc}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
