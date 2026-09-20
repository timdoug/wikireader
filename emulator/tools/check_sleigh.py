#!/usr/bin/env python3
"""Compare every base C33 PE word with GMMan's compiled SLEIGH decoder.

This is deliberately a decode oracle, not a p-code oracle.  The upstream
README still lists p-code validation as unfinished, and known semantic
differences are documented in emulator/README.md.

    python3 -m pip install pypcode
    sleigh /path/to/s1c33_sleigh/data/languages/s1c33.slaspec
    python3 tools/check_sleigh.py /path/to/s1c33_sleigh
"""

import re
import sys
from collections import Counter
from pathlib import Path


def load_wremu(root):
    forms = (root / "c33_forms.h").read_text()
    names = dict(re.findall(r'\[(OP_\w+)\] = "([^"]+)"', forms))

    start = forms.index("static const struct c33_form c33_forms[")
    end = forms.index("};", start)
    form_ops = re.findall(r"\{ (OP_\w+),", forms[start:end])

    key = "c33_form_of[65536] = {"
    start = forms.index(key) + len(key)
    end = forms.index("};", start)
    form_of = [int(value) for value in re.findall(r"\d+", forms[start:end])]
    if len(form_of) != 65536:
        raise ValueError(f"parsed {len(form_of)} decoder entries, expected 65536")

    valid_source = (root / "c33_pe_valid.h").read_text()
    key = "c33_pe_valid[8192] = {"
    start = valid_source.index(key) + len(key)
    end = valid_source.index("};", start)
    bitmap = [int(value, 16) for value in
              re.findall(r"0x([0-9a-fA-F]{2})", valid_source[start:end])]
    if len(bitmap) != 8192:
        raise ValueError(f"parsed {len(bitmap)} validity bytes, expected 8192")

    mnemonics = [names.get(form_ops[form_of[word]], ".short")
                 for word in range(65536)]
    valid = [bool(bitmap[word >> 3] & (1 << (word & 7)))
             for word in range(65536)]
    return mnemonics, valid


def documented_nop(word, mnemonic):
    """True where the manual defines a reserved operand as an instruction nop.

    SLEIGH uses unattached-register patterns to reject these words.  That is
    useful for decompilation, but execution on the PE core is explicitly a
    no-op, so wremu intentionally accepts them.
    """
    reg = word & 0xf
    if mnemonic == "pushs":
        return word & 0xfff0 == 0x0090 and reg not in (2, 3)
    if mnemonic == "pops":
        return word & 0xfff0 == 0x00d0 and reg not in (2, 3)
    if mnemonic == "psrset":
        return 0xbf40 <= word <= 0xbf5f and (word & 0x1f) > 4
    if mnemonic == "psrclr":
        return 0xbf80 <= word <= 0xbf9f and (word & 0x1f) > 4
    if mnemonic == "ld.w" and word & 0xff00 == 0xa000:
        return reg not in (0, 1, 2, 3, 8, 15)
    if mnemonic == "ld.w" and word & 0xff00 == 0xa400:
        source_reg = (word >> 4) & 0xf
        return source_reg not in (0, 1, 2, 3, 8, 10, 11, 15)
    return False


def main(argv):
    if len(argv) != 2:
        print(f"usage: {argv[0]} /path/to/s1c33_sleigh", file=sys.stderr)
        return 2

    try:
        import pypcode
    except ImportError:
        print("check_sleigh: install the optional pypcode package", file=sys.stderr)
        print("  python3 -m pip install pypcode", file=sys.stderr)
        return 2

    root = Path(__file__).resolve().parent.parent
    sleigh = Path(argv[1]).resolve()
    languages = sleigh / "data" / "languages"
    ldefs = languages / "s1c33.ldefs"
    sla = languages / "s1c33.sla"
    if not ldefs.exists():
        print(f"check_sleigh: no language definition at {ldefs}", file=sys.stderr)
        return 2
    if not sla.exists():
        print(f"check_sleigh: compile {languages / 's1c33.slaspec'} first", file=sys.stderr)
        print("  /path/to/ghidra/support/sleigh s1c33.slaspec", file=sys.stderr)
        return 2

    wremu_mnemonic, wremu_valid = load_wremu(root)
    arch = pypcode.Arch("S1C33", str(ldefs))
    context = pypcode.Context(arch.languages[0])

    counts = Counter()
    problems = []
    gaps = Counter()
    for word in range(65536):
        data = word.to_bytes(2, "little") + b"\0" * 6
        try:
            insn = context.disassemble(data, 0, max_instructions=1).instructions[0]
            sleigh_mnemonic = insn.mnem
        except (pypcode.BadDataError, pypcode.DecoderError, IndexError):
            sleigh_mnemonic = ".short"

        wremu_mnem = wremu_mnemonic[word] if wremu_valid[word] else ".short"
        if wremu_mnem == sleigh_mnemonic:
            counts["agree-valid" if wremu_mnem != ".short"
                   else "agree-invalid"] += 1
        elif (sleigh_mnemonic == ".short"
              and documented_nop(word, wremu_mnem)):
            counts["documented-no-op gap"] += 1
            gaps[wremu_mnem] += 1
        else:
            counts["unexpected"] += 1
            if len(problems) < 40:
                problems.append((word, wremu_mnem, sleigh_mnemonic))

    print(f"agree, valid          {counts['agree-valid']:5d}")
    print(f"agree, invalid        {counts['agree-invalid']:5d}")
    print(f"documented no-op gaps {counts['documented-no-op gap']:5d}")
    for mnemonic, count in sorted(gaps.items()):
        print(f"  {mnemonic:<8} {count:5d}")
    print(f"unexpected            {counts['unexpected']:5d}")
    for word, wremu_mnem, sleigh_mnemonic in problems:
        print(f"  {word:04x}: wremu={wremu_mnem} sleigh={sleigh_mnemonic}")
    return bool(problems)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
