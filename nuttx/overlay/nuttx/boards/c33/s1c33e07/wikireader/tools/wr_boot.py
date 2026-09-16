#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Boot the emulated device the way the hardware boots it.

The harnesses here used to hand the emulator a kernel ELF.  That skips the
mask ROM, the MBR, the boot loader and grifo, so the SDRAM controller and the
clocks are whatever the emulator invents for them and the serial line has
never been through init_rs232_ch0().  The machine is close to the real one
and not equal to it, and the differences have cost a guest clock a quarter
slow, a memory model that was never switched on, benchmarks five times out,
and a console that could not receive a byte.  The emulator refuses a bare ELF
now; `--bare-elf` exists for the toolchain suites, which run compiler output
and have no loader to go through.

So a test builds what the device reads: a card holding `kernel.elf` (grifo)
and `init.app` (the NuttX image under test), and a FLASH image with the boot
program in it.  Grifo chains to init.app when it has nothing else to do, so
NuttX starts without a menu to tap.

The cost is time.  Grifo brings the card up and loads the image off it before
NuttX's first instruction runs, which is why `UART_START` is a quarter of a
billion cycles rather than the twenty million a direct boot needed: type
before the prompt is there and the characters go nowhere.  Add `BOOT_CYCLES`
to whatever budget a test needed under the direct boot.
"""

import importlib.util
from pathlib import Path
import subprocess
import sys

# Cycles to the NSH prompt: grifo brings up the card, reads the image off it
# and chains.  Measured at about 210 million for a three megabyte image, so
# this leaves room for a bigger one without waiting on a whole extra boot.
UART_START = "250000000"

# What the boot costs a test that used to start at the application: add it to
# the instruction budget, not to the timeout, which is wall clock.
BOOT_CYCLES = 300_000_000

# One sector a cluster means fs_fat32.c never issues CMD18 and every read
# costs what the slowest formatting costs; 64 is what the device reports and
# what the benchmarks compare against.
SECTORS_PER_CLUSTER = 64


def load_fat_helper(wikireader):
    """The FAT fixture builder, which lives with the emulator's own tools."""

    helper = Path(wikireader) / "emulator/tools/mem_dma_bench/run.py"
    spec = importlib.util.spec_from_file_location("wr_fat_fixture", helper)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def make_card(path, image, wikireader, files=None, grifo=None,
              sectors_per_cluster=SECTORS_PER_CLUSTER):
    """Write the card the boot reads: grifo, the image, and the test's files.

    `image` is the NuttX ELF; grifo's loader walks its section headers, so a
    stripped or unstripped one both work.  Anything in `files` lands beside
    them and shows up on the device as /sd/<name>.
    """

    wikireader = Path(wikireader).resolve()
    grifo = Path(grifo) if grifo else wikireader / "samo-lib/grifo/grifo.elf"
    for needed in (grifo, Path(image)):
        if not needed.exists():
            raise SystemExit(f"nothing to boot: {needed} is missing")

    contents = {"kernel.elf": grifo.read_bytes(),
                "init.app": Path(image).read_bytes()}
    contents.update(files or {})

    fat = load_fat_helper(wikireader)
    fat.make_image(path, contents, sectors_per_cluster)
    return fat


def make_flash(out, wikireader):
    """Build the FLASH image the mask ROM starts from.

    make-flash.py will not overwrite, and these live in the build tree rather
    than a temporary directory, so last run's file is still there.
    """

    flash = Path(out) / "flash.rom"
    flash.unlink(missing_ok=True)
    subprocess.run([sys.executable,
                    str(Path(wikireader).resolve() /
                        "samo-lib/mbr/make-flash.py"), str(flash)],
                   check=True, stdout=subprocess.DEVNULL)
    return flash


def boot_args(card, flash):
    """The emulator arguments that boot that pair, in place of an ELF."""

    return ["-c", str(card), "-e", str(flash)]


def main():
    """Build the pair and print the command that boots it.

    For running the image by hand -- an interactive session in the SDL
    window, say -- where there is no harness to build the card.
    """

    import argparse

    root = Path(__file__).resolve().parents[5]
    parser = argparse.ArgumentParser(description=main.__doc__)
    parser.add_argument("--image", type=Path, default=root / "nuttx",
                        help="the NuttX image to boot as init.app")
    parser.add_argument("--wikireader", type=Path,
                        default=Path.home() / "wikireader")
    parser.add_argument("--out", type=Path, default=root / "build/wikireader/boot")
    args = parser.parse_args()

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    card = out / "card.img"
    make_card(card, args.image.resolve(), args.wikireader)
    flash = make_flash(out, args.wikireader)
    emulator = Path(args.wikireader).resolve() / "emulator/wremu"
    print(" ".join([f'"{emulator}"', "-g", *boot_args(card, flash)]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
