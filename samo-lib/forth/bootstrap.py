#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Metacompile the Forth with the Forth, so the build needs no other one.

The interpreter is written in Forth and compiled by a program written in
Forth, which is the usual chicken and egg: something has to read Forth
before there is a Forth to read it.  The Makefile answers that with gforth
on the build machine.  This answers it the way Smalltalk and Lisp always
have -- with an image.

seed/wrforth-seed.app is a WikiReader that knows how to be a Forth, and
emulator/wremu is a WikiReader written in C.  Between them they will read
meta.fs and ansi-forth.fs off a card and write out the assembly for the
next Forth, which is the same assembly gforth writes: byte for byte, once
the line endings agree.  The interpreter's "cr" is two characters because
it was written for a serial terminal.

So the tools this needs are a C compiler and an awk.  The seed is a binary
in the tree, which is worth being uneasy about -- except that this is the
program that proves it: what comes out is checked against the source it
came from, and against gforth whenever gforth is around.

Regenerating the seed, when the Forth has changed enough to be worth it:

    make -C nuttx CONFIG=forthseed
    cp nuttx/nuttx.app samo-lib/forth/seed/wrforth-seed.app

**The seed in the tree needs that doing before this will run.**  This
script used to hand the seed to the emulator as a bare ELF, which no longer
boots -- a direct boot leaves the SDRAM controller, the clocks and the
serial line in a state the hardware is never in, and the serial line is how
this drives the metacompiler.  It now puts the seed on the card as init.app
under a kernel.elf and boots the whole chain, which is right and is what
every other harness here does.  The seed itself does not survive that
handoff: grifo chains through `jpr.d %r0` at 0x10000200 with r0 holding
0x00300281, a peripheral register address rather than a code offset, and
execution lands in zeros before the seed prints anything.  It was built and
only ever exercised under the direct boot, so it has never had to.  The
same NuttX configuration built today boots the chain perfectly well as
nuttx.app, so regenerating is expected to be the whole fix.
"""

import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile

# The card the emulated device reads.  The order is the order gforth is
# given them in, and it matters: each file is its own input source, and the
# metacompiler stops at the end of whichever one it was started from.
SOURCES = {
    "meta.fs": "meta.fs",
    "symbols.fi": "forth-symbols.fi",
    "ansi.fs": "ansi-forth.fs",
    "vector.fi": "forth-vector.fi",
}

DRIVER = b"""0 warnings !
include meta.fs
include symbols.fi
include ansi.fs
include vector.fi
bye
"""

# Read the sources into tmpfs first.  They are only read once each, but the
# output is written a line at a time, and going to the card for that costs
# more emulated time than the compiling does.
COMMANDS = "".join(f"cp /sd/{name} /tmp/{name}\n"
                   for name in ["forth.ini", *SOURCES]) + """cd /tmp
wrforth > /tmp/out.s
cp /tmp/out.s /sd/out.s
umount /sd
echo BOOTSTRAP-DONE
"""

MARKER = ";;; Meta Compiler starting"


def load_fat_helper(root):
    helper = root / "emulator/tools/mem_dma_bench/run.py"
    spec = importlib.util.spec_from_file_location("wr_fat_fixture", helper)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def metacompile(root, here, seed, work, limit, awk, grifo):
    fat = load_fat_helper(root)

    symbols = work / "forth-symbols.fi"
    with symbols.open("w") as out:
        subprocess.run([awk, "-f", str(here / "sym.awk"),
                        str(here / "ansi-forth.fs"),
                        str(here / "forth-vector.fi")],
                       stdout=out, check=True)

    # The seed goes on the card as init.app, under a kernel.elf, and the run
    # boots the whole chain.  It used to be handed to the emulator as a bare
    # ELF, which no longer boots at all: a direct boot leaves the SDRAM
    # controller, the clocks and the serial line in a state the hardware is
    # never in, and the serial line is how this drives the metacompiler.
    files = {"forth.ini": DRIVER,
             "kernel.elf": grifo.read_bytes(),
             "init.app": seed.read_bytes()}
    for name, source in SOURCES.items():
        files[name] = (symbols if source == "forth-symbols.fi"
                       else here / source).read_bytes()

    card = work / "card.img"
    fat.make_image(card, files)
    flash = work / "flash.rom"
    subprocess.run([sys.executable, str(root / "samo-lib/mbr/make-flash.py"),
                    str(flash)], check=True, stdout=subprocess.DEVNULL)

    script = work / "commands.txt"
    script.write_text(COMMANDS)

    command = [str(root / "emulator/wremu"), "-n", str(limit),
               "-c", str(card), "-e", str(flash),
               "--uart-input", str(script),
               # Late enough that the prompt is there to type at: the loader
               # and grifo run first, and grifo loads the seed off the card.
               "--uart-start", "250000000", "--uart-gap", "400000"]
    with (work / "console.log").open("wb") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=3600,
                       env={**os.environ, "WREMU_HOLD_MS": "33"})

    console = (work / "console.log").read_bytes()
    if b"BOOTSTRAP-DONE" not in console:
        raise SystemExit(f"the emulated run did not finish; see "
                         f"{work / 'console.log'}")

    out = fat.read_file(card, "out.s")
    if not out:
        raise SystemExit("nothing was written to the card")

    # Everything before the interpreter's own banner belongs to the console,
    # not to the output.
    body = out[out.index(MARKER.encode()):].replace(b"\r\n", b"\n")
    return body


def main():
    here = Path(__file__).resolve().parent
    root = here.parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=Path,
                        default=here / "seed/wrforth-seed.app")
    parser.add_argument("--output", type=Path, default=here / "forth.s")
    parser.add_argument("--body", type=Path,
                        help="write only the metacompiler's output here")
    parser.add_argument("--awk", default=os.environ.get("AWK", "awk"))
    parser.add_argument("--grifo", type=Path,
                        default=root / "samo-lib/grifo/grifo.elf",
                        help="the kernel the card boots before the seed")
    parser.add_argument("--limit", type=int, default=40_000_000_000)
    parser.add_argument("--keep", type=Path,
                        help="working directory to keep for inspection")
    args = parser.parse_args()

    for needed in (root / "emulator/wremu", args.seed, args.grifo):
        if not needed.exists():
            raise SystemExit(f"{needed} is missing")

    context = (tempfile.TemporaryDirectory() if args.keep is None
               else None)
    work = Path(context.name) if context else args.keep
    work.mkdir(parents=True, exist_ok=True)
    try:
        body = metacompile(root, here, args.seed, work, args.limit, args.awk,
                           args.grifo)
    finally:
        if context:
            context.cleanup()

    if args.body:
        args.body.write_bytes(body)

    # The same three pieces the Makefile assembles, in the same order.
    hoisted = subprocess.run([args.awk, "-f", str(here / "hoist-flags.awk"),
                              "/dev/stdin"], input=body,
                             stdout=subprocess.PIPE, check=True).stdout
    args.output.write_bytes((here / "header.s").read_bytes() + hoisted +
                            (here / "trailer.s").read_bytes())
    print(f"{args.output}: {args.output.stat().st_size} bytes, "
          f"metacompiled by the Forth itself")
    return 0


if __name__ == "__main__":
    sys.exit(main())
