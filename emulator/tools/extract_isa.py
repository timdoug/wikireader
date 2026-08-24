#!/usr/bin/env python3
"""
Extract the Epson C33 instruction table from binutils' opcodes/c33-opc.c.

The table in binutils is the authoritative encoding description -- it is what
the assembler and disassembler both drive off -- so deriving our decoder from
it removes any guesswork about bit layouts.

Note: c33-opc.c contains Shift-JIS comments from the original Epson authors,
so it must be read as latin-1 rather than utf-8.
"""

import json
import re
import sys
from pathlib import Path

# --- encoding classes, transcribed from c33-opc.c lines 25-50 -----------------

def _c0_1(x): return (x & 0x3ff) << 6
def _c0_2(x): return (x & 0xff) << 8
def _c1(x):   return (x & 0xff) << 8
def _c2(x):   return (x & 0x3f) << 10
def _c3(x):   return (x & 0x3f) << 10
def _c4_1(x): return (x & 0x3f) << 10
def _c4_2(x): return _c1(x)
def _c5(x):   return _c1(x)
def _c6(x):   return (x & 0x7) << 13
def _c7(x):   return (x & 0x3f) << 10

CLASSES = {
    "OP_CLASS0_1": _c0_1, "OP_CLASS0_2": _c0_2,
    "OP_CLASS1": _c1, "OP_CLASS2": _c2, "OP_CLASS3": _c3,
    "OP_CLASS4_1": _c4_1, "OP_CLASS4_2": _c4_2,
    "OP_CLASS5": _c5, "OP_CLASS6": _c6, "OP_CLASS7": _c7,
}

# the _MASK forms are just the class applied to an all-ones field
MASKS = {
    "OP_CLASS0_1_MASK": _c0_1(0x3ff), "OP_CLASS0_2_MASK": _c0_2(0xff),
    "OP_CLASS1_MASK": _c1(0xff), "OP_CLASS2_MASK": _c2(0x3f),
    "OP_CLASS3_MASK": _c3(0x3f), "OP_CLASS4_1_MASK": _c4_1(0xff),
    "OP_CLASS4_2_MASK": _c4_2(0xff), "OP_CLASS5_MASK": _c5(0xff),
    "OP_CLASS6_MASK": _c6(0x7), "OP_CLASS7_MASK": _c7(0x3f),
}

# --- operand fields, parsed straight out of the c33_operands[] table ---------
# Each is "#define NAME idx" followed by "{ bits, shift, NULL, NULL, flags, size }".
# Parsing rather than transcribing avoids silent omissions -- an operand form we
# do not know about causes its whole instruction to be dropped from the table.
OPERAND_RE = re.compile(
    r'^[ \t]*#define[ \t]+(\w+)[ \t]+(\d+)[ \t]*$'
    r'(?:[^\n]*\n)*?'                       # skip commented-out variants
    r'^[ \t]*\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,'
    r'\s*NULL\s*,\s*NULL\s*,\s*([^,]+?)\s*,\s*(\d+)\s*\}',
    re.MULTILINE,
)


def parse_operands(text):
    start = text.find("c33_operand c33_operands[]")
    end = text.find("\n};", start)
    body = text[start:end if end > 0 else len(text)]
    out = {}
    for m in OPERAND_RE.finditer(body):
        name, _idx, bits, shift, flags, _size = m.groups()
        out[name] = (int(bits), int(shift), "C33_OPERAND_SIGNED" in flags)
    return out


OPERANDS = {}

EXPR = re.compile(r"(OP_CLASS[0-9_]*[0-9])\s*\(\s*(0x[0-9a-fA-F]+|\d+)\s*\)")
ENTRY = re.compile(
    r'\{\s*"([^"]+)"\s*,'      # mnemonic
    r'\s*([^,]+?)\s*,'         # opcode expression
    r'\s*([A-Z0-9_]+)\s*,'     # mask symbol
    r'\s*\{([^}]*)\}\s*,'      # operand list
    r'\s*(\d+)\s*,\s*(\d+)\s*\}'
)


def evaluate(expr):
    """Resolve an OP_CLASSn(x) expression to its numeric opcode."""
    expr = expr.strip()
    m = EXPR.fullmatch(expr)
    if not m:
        return None
    cls, val = m.group(1), int(m.group(2), 0)
    fn = CLASSES.get(cls)
    return fn(val) if fn else None


def parse_table(text, table_name):
    """Pull one `const struct c33_opcode <name>[] = { ... };` table."""
    start = text.find(f"c33_opcode {table_name}[]")
    if start < 0:
        return []
    end = text.find("\n};", start)
    body = text[start:end if end > 0 else len(text)]

    out = []
    for m in ENTRY.finditer(body):
        name, opexpr, maskname, operands, memop, special = m.groups()
        opcode = evaluate(opexpr)
        mask = MASKS.get(maskname)
        if opcode is None or mask is None:
            continue
        ops = [o.strip() for o in operands.split(",") if o.strip()]
        # skip entries using operand forms we have not transcribed
        if any(o not in OPERANDS for o in ops):
            continue
        out.append({
            "name": name,
            "opcode": opcode,
            "mask": mask,
            "operands": ops,
            "memop": int(memop),
            "special": int(special),
        })
    return out


def main():
    src = Path(sys.argv[1] if len(sys.argv) > 1 else "isa-ref/c33-opc.c")
    text = src.read_text(encoding="latin-1")

    OPERANDS.update(parse_operands(text))
    print(f"operand forms parsed     : {len(OPERANDS)}", file=sys.stderr)

    tables = {}
    for t in ("c33_opcodes", "c33_opcodes32", "c33_advance_opcodes",
              "c33_advance_opcodes32", "c33_pe_opcodes", "c33_pe_opcodes32",
              "c33_ext_opcodes"):
        entries = parse_table(text, t)
        if entries:
            tables[t] = entries
            print(f"{t:24s} {len(entries):5d} entries", file=sys.stderr)

    out = Path("c33_isa.json")
    out.write_text(json.dumps({
        "operands": {k: {"bits": v[0], "shift": v[1], "signed": v[2]}
                     for k, v in OPERANDS.items()},
        "tables": tables,
    }, indent=1))
    print(f"\nwrote {out} with {len(tables)} tables", file=sys.stderr)


if __name__ == "__main__":
    main()
