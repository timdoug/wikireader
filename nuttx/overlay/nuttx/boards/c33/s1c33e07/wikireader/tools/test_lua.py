#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run Lua's standard library on the device, from a script on the card.

The script comes off a FAT card rather than being echoed into tmpfs a line at
a time, which the other tests here have to do: it keeps the fixture readable
Lua instead of something shaped by NSH's quoting, and it means the run
exercises the card driver on the way past.

What is worth testing is less "does Lua work" -- it is a mature interpreter --
than "is the library there at all".  The NuttX build compiles every opener
under one option and registered only the base one, so math and string were nil
in an interpreter that otherwise ran, printed its banner and evaluated
expressions.  check.lua touches every library that gets registered.

The number model is part of the contract and is checked: 32-bit integers and
single-precision floats, which on a chip with no floating-point unit runs a
little over twice as fast as the 64-bit model for the same code.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

import wr_boot

SCRIPT = "check.lua"


def main():
    root = Path(__file__).resolve().parents[5]
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wikireader", type=Path,
                        default=Path.home() / "wikireader")
    parser.add_argument("--image", type=Path, default=root / "nuttx")
    parser.add_argument("--out", type=Path,
                        default=root / "build/wikireader/lua")
    parser.add_argument("--limit", type=int,
                        default=1_500_000_000 + wr_boot.BOOT_CYCLES)
    args = parser.parse_args()

    emulator = args.wikireader.resolve() / "emulator/wremu"
    if not emulator.exists():
        print(f"SKIP: {emulator} is missing; build the emulator first")
        return 0

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    source = (here / SCRIPT).read_bytes()
    card = out / "card.img"
    wr_boot.make_card(card, args.image.resolve(), args.wikireader,
                      {SCRIPT: source})
    flash = wr_boot.make_flash(out, args.wikireader)

    commands = f"lua -v\nlua /sd/{SCRIPT}\necho LUA_STATUS=$?\n"
    (out / "input.txt").write_text(commands)

    command = [str(emulator), "-R", *wr_boot.boot_args(card, flash),
               "-n", str(args.limit),
               "--uart-input", str(out / "input.txt"),
               "--uart-start", wr_boot.UART_START, "--uart-gap", "300000"]
    with (out / "lua.log").open("w") as log:
        subprocess.run(command, cwd=out, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=1800,
                       env={**os.environ, "WREMU_HOLD_MS": "33"})

    text = (out / "lua.log").read_bytes().decode(errors="replace")
    text = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", text.replace("\r", ""))
    (out / "console.txt").write_text(text)

    problems = []
    if not re.search(r"^Lua 5\.4", text, re.M):
        problems.append("no interpreter banner")
    checks = re.search(r"LUA_CHECKS=(\d+)", text)
    if not checks:
        problems.append("check.lua did not reach its last line")
    if not re.search(r"LUA_STATUS=0\b", text):
        problems.append("the interpreter exited non-zero")
    for line in text.splitlines():
        if line.startswith("lua:"):
            problems.append(line.strip())

    if problems:
        for problem in problems:
            print(f"   {problem}")
        raise SystemExit(f"see {out / 'console.txt'}")

    print(f"PASS: Lua 5.4, {checks.group(1)} library checks from the card; "
          f"{out / 'console.txt'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
