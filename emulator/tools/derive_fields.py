#!/usr/bin/env python3
"""
Derive C33 operand field layouts empirically from objdump.

For every 16-bit encoding we know the mnemonic and the operand text binutils
prints. Grouping encodings by (mnemonic, operand shape) gives, for each
instruction form, a set whose constant bits are the opcode and whose varying
bits carry operands. For each numeric slot we search for the
(shift, width, signed, bias) reproducing the printed value for *every* member.

Deliberately not driven by c33-opc.c's operand table: that is the assembler's
view and is missing forms the disassembler emits -- e.g. srl/sll/sra, where
the real encoding is a single 5-bit immediate at shift 4 rather than the two
separate IMM4 families the assembler table lists.

Note the derived opcode/mask pairs OVERLAP between forms and must not be used
for decoding. Decoding is done by flat lookup (see gen_optab.py); these
layouts only extract operands once the form is known.
"""

import re
import sys
from collections import defaultdict
from pathlib import Path

LINE = re.compile(r"^\s*[0-9a-f]+:\s+([0-9a-f]{4})\s+(\S+)(?:\s+(\S+))?")
NUM = re.compile(r"0x[0-9a-fA-F]+|\d+")
REG = re.compile(r"%r(\d+)")


def looks_like_operands(tok):
    """objdump repeats the mnemonic in a later column; tell them apart."""
    if not tok:
        return False
    return tok[0] in "%[-" or tok.startswith("0x") or tok[0].isdigit()


def shape(text):
    """'%r7,0x12' -> '%r#,#' so encodings of one form group together."""
    return NUM.sub("#", REG.sub("%r#", text))


def values(text):
    """Ordered numeric operands: register numbers and immediates, in order."""
    out, i = [], 0
    while i < len(text):
        m = REG.match(text, i)
        if m:
            out.append(int(m.group(1)))
            i = m.end()
            continue
        m = NUM.match(text, i)
        if m:
            out.append(int(m.group(0), 0))
            i = m.end()
            continue
        i += 1
    return out


def solve(pairs):
    """Find (shift, width, signed, bias) reproducing the value for all pairs."""
    for width in range(1, 17):
        for shift in range(0, 17 - width):
            mask = (1 << width) - 1
            biases = {v - ((insn >> shift) & mask) for insn, v in pairs}
            if len(biases) == 1:
                return (shift, width, False, biases.pop())
            sb = set()
            for insn, v in pairs:
                raw = (insn >> shift) & mask
                if raw >> (width - 1):
                    raw -= 1 << width
                sb.add(v - raw)
            if len(sb) == 1:
                return (shift, width, True, sb.pop())
    return None


def _const_bits(members):
    ones, zeros = 0xFFFF, 0xFFFF
    for i, _ in members:
        ones &= i
        zeros &= ~i & 0xFFFF
    return ones, (ones | zeros) & 0xFFFF


def _analyse(members):
    ones, mask = _const_bits(members)
    nslots = len(members[0][1])
    fields = []
    for s in range(nslots):
        pairs = [(i, v[s]) for i, v in members if len(v) == nslots]
        fields.append(solve(pairs) if pairs else None)
    if any(f is None for f in fields):
        return None
    return (ones, mask, fields)


def _split(members, depth=0):
    """Distinct opcode families can share mnemonic+shape; bisect on the
    highest varying bit until each subgroup solves."""
    got = _analyse(members)
    if got is not None:
        return [(members, got)]
    if depth > 6:
        return [(members, None)]
    ones, mask = _const_bits(members)
    varying = (~mask) & 0xFFFF
    if not varying:
        return [(members, None)]
    bit = varying.bit_length() - 1
    lo = [m for m in members if not (m[0] >> bit) & 1]
    hi = [m for m in members if (m[0] >> bit) & 1]
    if not lo or not hi:
        return [(members, None)]
    return _split(lo, depth + 1) + _split(hi, depth + 1)


def derive(path):
    """-> list of forms, each a dict with mnemonic/shape/fields/encodings."""
    groups = defaultdict(list)
    for line in Path(path).read_text().splitlines():
        m = LINE.match(line)
        if not m:
            continue
        insn, mnem, tok = int(m.group(1), 16), m.group(2), m.group(3)
        if mnem == ".short":
            continue
        ops = tok if looks_like_operands(tok) else ""
        groups[(mnem, shape(ops))].append((insn, values(ops)))

    forms = []
    for (mnem, shp), members in sorted(groups.items()):
        for sub, got in _split(members):
            opcode, mask, fields = got if got else (0, 0, None)
            forms.append({
                "mnemonic": mnem,
                "shape": shp,
                "opcode": opcode,
                "mask": mask,
                "fields": fields,
                "encodings": sorted(i for i, _ in sub),
            })
    return forms


def main():
    forms = derive(sys.argv[1] if len(sys.argv) > 1 else "allinsn_full.txt")
    print(f"{'mnemonic':<10} {'shape':<22} {'n':>5}  opcode/mask      fields")
    print("-" * 92)
    bad = 0
    for f in forms:
        if f["fields"] is None:
            print(f"{f['mnemonic']:<10} {f['shape']:<22} "
                  f"{len(f['encodings']):>5}  ????/????  [UNSOLVED]")
            bad += 1
            continue
        desc = " ".join(
            f"[sh{a},w{b}{',s' if c else ''}{',+%d' % d if d else ''}]"
            for a, b, c, d in f["fields"]) or "-"
        print(f"{f['mnemonic']:<10} {f['shape']:<22} {len(f['encodings']):>5}  "
              f"{f['opcode']:04x}/{f['mask']:04x}  {desc}")
    print("-" * 92)
    print(f"forms: {len(forms)}   unsolved: {bad}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
