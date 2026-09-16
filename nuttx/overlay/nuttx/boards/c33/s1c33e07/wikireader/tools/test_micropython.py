#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run MicroPython on the device: a script off the card, and then the REPL.

check.py is the same idea as check.lua -- touch every module the build
registers, and assert the things the port had to choose rather than the
things Python guarantees: arbitrary precision integers, single-precision
floats, a collector heap taken from NuttX at startup, and a filesystem that
is NuttX's rather than one of MicroPython's own.

The REPL is exercised separately because it broke separately.  A flat build
has one symbol namespace and NuttX brings its own readline(), so the
interpreter's line editor lost the link to the shell's -- a function with a
different signature entirely.  What came out was a banner, no prompt, and an
endless run of erase-line escapes, with every other part of the interpreter
working perfectly.  So: type an expression at it and insist on seeing the
prompt, the echo and the answer.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

import wr_boot

SCRIPT = "check.py"

# Typed at the REPL once the script has run.  The input ends here, and the
# end of input is a Control-D, which is how the interpreter is asked to
# leave -- so reaching the shell prompt again is part of what is checked.
SESSION = """6*7
sum(n * n for n in range(10))
import sys; print(sys.implementation.name, sys.platform)
"""




def main():
    root = Path(__file__).resolve().parents[5]
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wikireader", type=Path,
                        default=Path.home() / "wikireader")
    parser.add_argument("--image", type=Path, default=root / "nuttx")
    parser.add_argument("--out", type=Path,
                        default=root / "build/wikireader/micropython")
    parser.add_argument("--limit", type=int,
                        default=2_500_000_000 + wr_boot.BOOT_CYCLES)
    args = parser.parse_args()

    emulator = args.wikireader.resolve() / "emulator/wremu"
    if not emulator.exists():
        print(f"SKIP: {emulator} is missing; build the emulator first")
        return 0

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    card = out / "card.img"
    wr_boot.make_card(card, args.image.resolve(), args.wikireader,
                      {SCRIPT: (here / SCRIPT).read_bytes()})
    flash = wr_boot.make_flash(out, args.wikireader)

    commands = (f"micropython /sd/{SCRIPT}\n"
                f"echo PY_STATUS=$?\n"
                f"micropython\n" + SESSION)
    (out / "input.txt").write_text(commands)

    command = [str(emulator), "-R", *wr_boot.boot_args(card, flash),
               "-n", str(args.limit),
               "--uart-input", str(out / "input.txt"),
               "--uart-start", wr_boot.UART_START, "--uart-gap", "300000"]
    with (out / "micropython.log").open("w") as log:
        subprocess.run(command, cwd=out, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=1800,
                       env={**os.environ, "WREMU_HOLD_MS": "33"})

    raw = (out / "micropython.log").read_bytes().decode(errors="replace")
    text = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", raw.replace("\r", ""))
    (out / "console.txt").write_text(text)

    problems = []
    checks = re.search(r"PY_CHECKS=(\d+)", text)
    if not checks:
        problems.append(f"{SCRIPT} did not reach its last line")
    if not re.search(r"PY_STATUS=0\b", text):
        problems.append("the interpreter exited non-zero")

    # The banner says which version was built and nothing about when, which
    # is what keeps two builds of this image identical.
    if not re.search(r"^MicroPython \d+\.\d+\.\d+; WikiReader", text, re.M):
        problems.append("no interpreter banner")
    if ">>> 6*7" not in text:
        problems.append("the REPL printed no prompt, or did not echo")
    if not re.search(r"^42$", text, re.M):
        problems.append("the REPL did not evaluate an expression")
    if not re.search(r"^285$", text, re.M):
        problems.append("the REPL did not evaluate a generator expression")
    if "micropython nuttx" not in text:
        problems.append("the REPL could not import a module")

    # An escape run is what the broken line editor produced, and it is not
    # something a working one has any reason to send.
    if re.search(r"(?:\x1b\[K){4,}", raw):
        problems.append("the line editor is redrawing in a loop")

    for line in text.splitlines():
        if line.startswith("Traceback"):
            problems.append("an exception reached the console")

    if problems:
        for problem in problems:
            print(f"   {problem}")
        raise SystemExit(f"see {out / 'console.txt'}")

    print(f"PASS: MicroPython, {checks.group(1)} checks from the card and a "
          f"REPL session; {out / 'console.txt'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
