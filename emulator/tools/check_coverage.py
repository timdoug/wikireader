#!/usr/bin/env python3
"""
Verify the generated decode table against real firmware.

Every instruction objdump found in our cross-compiled binaries must decode to
the same mnemonic via the flat table, and none may land on OP_INVALID.
"""

import sys
from collections import Counter
from pathlib import Path


def main():
    tab = {}
    for line in Path("allinsn.txt").read_text().splitlines():
        p = line.split()
        if len(p) >= 2:
            tab[int(p[0], 16)] = p[1]

    total = ok = 0
    bad = Counter()
    invalid = Counter()

    for ref in sys.argv[1:]:
        for line in Path(ref).read_text().splitlines():
            p = line.split()
            if len(p) < 2:
                continue
            insn, expect = int(p[0], 16), p[1]
            got = tab.get(insn)
            total += 1
            if got == ".short":
                invalid[f"{insn:04x}"] += 1
            elif got != expect:
                bad[(expect, got)] += 1
            else:
                ok += 1

    print(f"instructions decoded : {total}")
    print(f"exact mnemonic match : {ok}  ({100.0*ok/total:.2f}%)")
    print(f"decoded as .short    : {sum(invalid.values())}")
    print(f"mismatched           : {sum(bad.values())}")
    for (e, g), n in bad.most_common(10):
        print(f"   {e} -> {g}  x{n}")
    for enc, n in invalid.most_common(10):
        print(f"   invalid encoding {enc} x{n}")
    return 0 if ok == total else 1


if __name__ == "__main__":
    sys.exit(main())
