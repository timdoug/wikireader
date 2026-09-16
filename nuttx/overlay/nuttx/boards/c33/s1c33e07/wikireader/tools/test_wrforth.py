#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the WikiReader's own Forth as a task, against its own programs.

The interpreter is the one the device shipped with, metacompiled out of
samo-lib/forth and given POSIX instead of bare-metal drivers.  The thing
worth proving is not that it starts -- it prints a banner whatever state it
is in -- but that the two halves of it work:

  * compilation.  A colon definition needs the immediate words to run while
    it is being read, which is where the dictionary's flags matter, and a
    build with those flags lost still starts, prints, and evaluates
    arithmetic before going quietly wrong at the first ":".
  * the outside world.  cli.4th is carried in this repository and is what
    the card has always held: it opens directories and files, reads lines,
    writes, renames and deletes, so including it exercises most of the C
    calls the interpreter has at once.

Everything here is typed the same way at the device's own prompt.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

import wr_boot

# Each command, and what the transcript must hold afterwards.
SESSION = [
    ("1 2 + . cr", "3"),
    (": sq dup * ;", None),
    ("7 sq . cr", "49"),
    ("hex ff . decimal cr", "FF"),
    (": five 5 0 do i . loop cr ; five", "0 1 2 3 4"),
    ("variable v 99 v ! v @ . cr", "99"),
    ("create arr 10 cells allot 1234 arr ! arr @ . cr", "1234"),
    ("include /sd/cli.4th", "cli.4th - some simple commands"),
    ("display /sd/hello.txt", "second line"),
    ("mkfile /tmp/made.txt", None),
    ("display /tmp/made.txt", "abcdefghijklmnopqrstuvwxyz"),
    ("rename /tmp/made.txt /tmp/moved.txt", None),
    ("delete /tmp/moved.txt", None),
    ("bye", None),
]

PAYLOAD = b"first line\nsecond line\nthird line\n"




def main():
    root = Path(__file__).resolve().parents[5]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wikireader", type=Path,
                        default=Path.home() / "wikireader")
    parser.add_argument("--image", type=Path, default=root / "nuttx")
    parser.add_argument("--out", type=Path,
                        default=root / "build/wikireader/wrforth")
    parser.add_argument("--limit", type=int,
                        default=3_000_000_000 + wr_boot.BOOT_CYCLES)
    args = parser.parse_args()

    wikireader = args.wikireader.resolve()
    emulator = wikireader / "emulator/wremu"
    if not emulator.exists():
        print(f"SKIP: {emulator} is missing; build the emulator first")
        return 0

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    card = out / "card.img"
    wr_boot.make_card(card, args.image.resolve(), wikireader, {
        "cli.4th": (wikireader /
                    "samo-lib/forth/programs/cli.4th").read_bytes(),
        "hello.txt": PAYLOAD,
    })
    flash = wr_boot.make_flash(out, wikireader)

    commands = ["wrforth"] + [line for line, _ in SESSION]
    commands += ["echo BACK_AT_THE_SHELL"]
    (out / "input.txt").write_text("\n".join(commands) + "\n")

    command = [str(emulator), "-R", *wr_boot.boot_args(card, flash),
               "-n", str(args.limit),
               "--uart-input", str(out / "input.txt"),
               "--uart-start", wr_boot.UART_START, "--uart-gap", "600000"]
    with (out / "wrforth.log").open("w") as log:
        subprocess.run(command, cwd=out, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=1800,
                       env={**os.environ, "WREMU_HOLD_MS": "33"})

    text = (out / "wrforth.log").read_bytes().decode(errors="replace")
    text = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", text.replace("\r", ""))
    (out / "console.txt").write_text(text)

    problems = []
    if "moko forth interpreter for S1C33" not in text:
        problems.append("the interpreter did not start")

    # An error prints "error -13" (undefined word) or similar and re-shows
    # the line with a caret; neither should appear.
    for line in text.splitlines():
        if re.match(r"^error -\d+", line.strip()):
            problems.append(f"the interpreter reported {line.strip()}")

    # Each answer has to appear after the command that produces it, so a
    # stale match earlier in the transcript cannot stand in for it.
    position = 0
    for line, expected in SESSION:
        index = text.find(line, position)
        if index < 0:
            problems.append(f"{line!r} was never echoed")
            continue

        position = index + len(line)
        if expected is not None and text.find(expected, position) < 0:
            problems.append(f"{line!r} did not answer {expected!r}")

    if "BACK_AT_THE_SHELL" not in text:
        problems.append("BYE did not return to the shell")

    if problems:
        for problem in problems:
            print(f"   {problem}")
        raise SystemExit(f"see {out / 'console.txt'}")

    print(f"PASS: {len(SESSION)} Forth lines, cli.4th loaded off the card; "
          f"{out / 'console.txt'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
