#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run a BASIC program on the device, from a card.

Michael Haardt's Bas, which nuttx-apps carries in full -- no download -- and
which wanted nothing this board did not already have.  Its Kconfig mentions
fork, but only for the option that shells out; the interpreter itself does
not, which is worth knowing on an architecture that has no fork at all.

check.bas is written so that every line prints something, because the useful
failure here is not a crash but a wrong number: this is the first thing on
the target to lean on the software floating point from a language where
every bare number is a float.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

import wr_boot

SCRIPT = "check.bas"

# What the program must print, in order, with the spacing left out.
EXPECTED = [
    "SUMSQ=55",
    "STR=WIKIrea10",
    "MATH=127-1",
    "ARRAY=210",
    "GOSUB=ok",
    "FLOAT=0.25",
    "BASIC-CHECKS-DONE",
]




def main():
    root = Path(__file__).resolve().parents[5]
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wikireader", type=Path,
                        default=Path.home() / "wikireader")
    parser.add_argument("--image", type=Path, default=root / "nuttx")
    parser.add_argument("--out", type=Path, default=root / "build/wikireader/bas")
    parser.add_argument("--limit", type=int,
                        default=2_000_000_000 + wr_boot.BOOT_CYCLES)
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
    (out / "input.txt").write_text(f"bas /sd/{SCRIPT}\necho BAS_STATUS=$?\n")

    command = [str(emulator), "-R", *wr_boot.boot_args(card, flash),
               "-n", str(args.limit),
               "--uart-input", str(out / "input.txt"),
               "--uart-start", wr_boot.UART_START, "--uart-gap", "500000"]
    with (out / "bas.log").open("w") as log:
        subprocess.run(command, cwd=out, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=1800,
                       env={**os.environ, "WREMU_HOLD_MS": "33"})

    text = (out / "bas.log").read_bytes().decode(errors="replace")
    text = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", text.replace("\r", ""))
    (out / "console.txt").write_text(text)

    squeezed = [re.sub(r"\s+", "", line) for line in text.splitlines()]
    problems = [f"{want!r} is not in the output" for want in EXPECTED
                if want not in squeezed]
    problems += [line.strip() for line in text.splitlines()
                 if line.startswith("Error:")]
    if not re.search(r"BAS_STATUS=0\b", text):
        problems.append("the interpreter exited non-zero")

    if problems:
        for problem in problems:
            print(f"   {problem}")
        raise SystemExit(f"see {out / 'console.txt'}")

    print(f"PASS: Bas 2.4, {len(EXPECTED)} results from the card; "
          f"{out / 'console.txt'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
