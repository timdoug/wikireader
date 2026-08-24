#!/usr/bin/env python3
"""
Validate our extracted C33 decode table against binutils' own disassembler.

Input is a file of "<hex16> <mnemonic>" pairs produced by
c33-epson-elf-objdump. For every distinct encoding we decode with our table
and compare the mnemonic. Any mismatch means our table or its precedence
rules are wrong.
"""

import json
import sys
from collections import Counter
from pathlib import Path


def load(path, names):
    """Concatenate the named tables in order; earlier tables win ties."""
    data = json.loads(Path(path).read_text())
    tables = data["tables"]
    out = []
    for n in names:
        out.extend(tables.get(n, []))
    return out


def decode(insn, table):
    """First entry whose masked bits match. Table order is significant --
    binutils documents it as sorted so the disassembler considers the most
    specific encodings first."""
    for e in table:
        if (insn & e["mask"]) == e["opcode"]:
            return e
    return None


def main():
    ref = Path(sys.argv[2] if len(sys.argv) > 2 else "ref-grifo.txt")
    names = sys.argv[3].split(",") if len(sys.argv) > 3 else ["c33_opcodes"]
    table = load(sys.argv[1] if len(sys.argv) > 1 else "c33_isa.json", names)
    print(f"tables             : {','.join(names)} ({len(table)} entries)")

    pairs = []
    for line in ref.read_text().splitlines():
        parts = line.split()
        if len(parts) >= 2:
            pairs.append((int(parts[0], 16), parts[1]))

    distinct = sorted(set(pairs))
    ok = 0
    miss = Counter()
    wrong = Counter()
    examples = {}

    for insn, expect in distinct:
        got = decode(insn, table)
        if got is None:
            miss[expect] += 1
            examples.setdefault(("MISS", expect), f"{insn:04x}")
        elif got["name"] != expect:
            wrong[(expect, got["name"])] += 1
            examples.setdefault(("WRONG", expect), f"{insn:04x} -> {got['name']}")
        else:
            ok += 1

    total = len(distinct)
    print(f"distinct encodings : {total}")
    print(f"correct            : {ok}  ({100.0*ok/total:.1f}%)")
    print(f"undecoded          : {sum(miss.values())}")
    print(f"mismatched         : {sum(wrong.values())}")

    if miss:
        print("\ntop undecoded mnemonics:")
        for name, n in miss.most_common(12):
            print(f"  {name:12s} {n:4d}   e.g. {examples[('MISS', name)]}")
    if wrong:
        print("\ntop mismatches (expected -> got):")
        for (exp, got), n in wrong.most_common(12):
            print(f"  {exp:12s} -> {got:12s} {n:4d}   e.g. {examples[('WRONG', exp)]}")

    return 0 if not miss and not wrong else 1


if __name__ == "__main__":
    sys.exit(main())
