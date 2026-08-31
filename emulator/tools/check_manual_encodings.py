#!/usr/bin/env python3
"""Cross-check the decode table against the C33 PE Core manual.

The tables in c33_forms.h were derived empirically, by disassembling all
65536 encodings with binutils and solving each operand field. That makes
binutils a single point of failure: if its disassembler is wrong, this
emulator is wrong in exactly the same way, and "matches binutils on 65,605
instructions" would never notice.

The manual documents each instruction's encoding as a bit diagram followed
by a hex pattern with '_' for don't-care nibbles -- "add %rd, %rs" is
0x22__. This reads those patterns out of the PDF and checks that every
encoding matching one decodes to the mnemonic the manual names, giving the
decoder a second and genuinely independent authority.

What it checks: the opcode structure, for every form the manual gives a
pattern for. What it does not: operand field extents, which come from the
field solver and are validated against binutils' operand printing instead.

    python3 tools/check_manual_encodings.py ../s1c33.pdf
"""

import json
import re
import subprocess
import sys
from pathlib import Path


def manual_text(pdf):
    out = Path("/tmp/c33_pe_manual.txt")
    if not out.exists() or out.stat().st_mtime < Path(pdf).stat().st_mtime:
        subprocess.run(["pdftotext", "-layout", str(pdf), str(out)], check=True)
    return out.read_text(encoding="utf-8", errors="replace").split("\n")


def documented_encodings(lines):
    """{'add %rd, %rs': '22__'} from the manual's per-instruction pages."""
    toc = set()
    for l in lines:
        m = re.match(r'^\s{6,}([a-z][a-z0-9._]*(?: [^.]*?)?)\s*\.{5,}\s*\d+\s*$', l)
        if m:
            toc.add(m.group(1).strip())

    recs = {}
    for i, l in enumerate(lines):
        t = l.strip()
        if t not in toc or t in recs:
            continue
        code_at = next((j for j in range(i + 1, min(i + 8, len(lines)))
                        if lines[j].lstrip().startswith("Code")), None)
        if code_at is None:
            continue
        for j in range(code_at, min(code_at + 40, len(lines))):
            m = re.search(r'0x([0-9A-Fa-f][0-9A-Fa-f_]{3})\s*$', lines[j].rstrip())
            if m:
                recs[t] = m.group(1)
                break
    return recs


def decode_table(header):
    s = Path(header).read_text()
    names = dict(re.findall(r'\[(OP_\w+)\] = "([^"]+)"', s))
    st = s.index("static const struct c33_form c33_forms[")
    form_ops = re.findall(r'\{ (OP_\w+),', s[st:s.index("};", st)])
    key = "c33_form_of[65536] = {"
    st2 = s.index(key) + len(key)
    nums = [int(x) for x in re.findall(r'\d+', s[st2:s.index("};", st2)])]
    assert len(nums) == 65536, f"parsed {len(nums)} table entries"
    return lambda w: names.get(form_ops[nums[w]], "?")


def main(argv):
    pdf = argv[1] if len(argv) > 1 else "../s1c33.pdf"
    if not Path(pdf).exists():
        print(f"need the C33 PE Core manual at {pdf}")
        return 2
    recs = documented_encodings(manual_text(pdf))
    mnem = decode_table(Path(__file__).resolve().parent.parent / "c33_forms.h")

    agree, problems, skipped = 0, [], 0
    for form, pat in sorted(recs.items()):
        want = form.split()[0]
        fixed = [(i, c) for i, c in enumerate(pat) if c != "_"]
        if len(fixed) < 2:
            skipped += 1
            continue
        got = {mnem(w) for w in range(65536)
               if all(f"{w:04X}"[i] == c.upper() for i, c in fixed)}
        # The underscores are operand fields, not unspecified opcode bits:
        # every encoding selected by the documented pattern must therefore
        # decode to this mnemonic.  Merely finding WANT in GOT would hide an
        # overlap or precedence error in part of the operand space.
        if got == {want}:
            agree += 1
        else:
            problems.append((form, pat, want, sorted(got)))

    print(f"documented forms checked : {agree + len(problems)}")
    print(f"  agree                  : {agree}")
    print(f"  disagree               : {len(problems)}")
    if skipped:
        print(f"  skipped (under-specified pattern): {skipped}")
    for form, pat, want, got in problems:
        print(f"    {form:<26} 0x{pat}  manual={want:<9} table={got}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
