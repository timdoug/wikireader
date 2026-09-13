#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run NuttX's benchmarks on the emulated device, and read the same numbers
back out of a real one.

This is a measurement, not a test: nothing here passes or fails, it prints a
table.  The point of it is the comparison.  The emulator counts cycles and
models SDRAM row activations and the SPI card, and the guest's clock is driven
by those counts -- so every number below is in emulated seconds, and the only
way to know what that is worth is to run the same image on the hardware and
put the two columns side by side.

Both sides run the same thing: the device command `bench`, which runs every
benchmark with its iteration counts built in and writes what they printed to
/sd/bench.txt.  This types that command at the emulator and reads the file
back out of the card image afterwards.  On real hardware, type `bench`, wait
for it to say the card can come out, and then

    run_benchmarks.py --compare /Volumes/.../bench.txt

parses that file with the same parsers and prints emulator, device and ratio
side by side.  There is one list of commands and it lives in the device
application, because a benchmark run compared against a differently-argued
benchmark run is worse than no comparison at all.

Two benchmarks in the tree are not here.  cachespeed wants ARCH_ICACHE and
ARCH_DCACHE, and the C33 has neither -- which is itself the answer it would
have given.  cyclictest wants a /dev/timer character driver, which this board
does not implement; osperf wants the high-priority work queue, which would put
another thread in the shipping image to benchmark it.
"""

import argparse
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys

# What the device application writes, and where.  Both are its Kconfig
# defaults; it prints the command it ran above each benchmark's output, so a
# file is self-describing whatever it was called.
RESULTS = "bench.txt"


def parse_coremark(text):
    """CoreMark's own summary line, which it only prints if the CRCs match."""
    match = re.search(r"^CoreMark 1\.0 : ([\d.]+) /", text, re.M)
    if not match:
        return []
    score = float(match.group(1))

    # The system clock is the PLL's 60 MHz, not the 48 MHz crystal that
    # feeds it -- see CONFIG_S1C33E07_MCLK.
    return [("iterations/sec", score, ""), ("CoreMark/MHz", score / 60.0, "")]


def parse_dhrystone(text):
    match = re.search(r"Dhrystones per Second:\s+([\d.]+)", text)
    if not match:
        return []
    rate = float(match.group(1))

    # 1757 Dhrystones/sec is what a VAX 11/780 managed, and dividing by it is
    # what the "MIPS" in Dhrystone MIPS has always meant.
    return [("Dhrystones/sec", rate, ""), ("VAX MIPS", rate / 1757.0, "")]


def parse_whetstone(text):
    match = re.search(r"Whetstones: ([\d.]+) (KIPS|MIPS)", text)
    if not match:
        return []
    value = float(match.group(1))
    if match.group(2) == "MIPS":
        value *= 1000.0
    return [("KIPS (double precision)", value, "")]


def parse_ramspeed(text):
    """The largest block size each variant reported, which is the one the
    10 ms clock measured properly and the one least helped by a lucky row.
    """
    results = {}
    size = None
    for line in text.splitlines():
        heading = re.match(r"_+Perform (\d+) (K?)Bytes access\s*_+", line.strip())
        if heading:
            size = int(heading.group(1)) * (1024 if heading.group(2) else 1)
            continue
        rate = re.search(r"(system|internal) (memcpy|memset)\(\):\s+"
                         r"Rate = ([\d.]+) KB/s", line)
        if rate and size:
            key = f"{rate.group(2)} {rate.group(1)}"
            results[key] = (size, float(rate.group(3)))

    return [(f"{name} @ {size // 1024} KB", rate, "KB/s")
            for name, (size, rate) in sorted(results.items())]


def parse_sdbench(text):
    out = []
    for pattern, label in ((r"Avg\s+:\s+([\d.]+) KB/s, [\d.]+ MB written",
                            "sequential write"),
                           (r"Avg\s+:\s+([\d.]+) KB/s, [\d.]+ MB and verified",
                            "sequential read + verify")):
        match = re.search(pattern, text)
        if match:
            out.append((label, float(match.group(1)), "KB/s"))
    return out


PARSERS = [
    ("coremark", parse_coremark),
    ("dhrystone", parse_dhrystone),
    ("whetstone", parse_whetstone),
    ("ramspeed", parse_ramspeed),
    ("sdbench", parse_sdbench),
]


def clean(raw):
    """A console log, with the escape sequences and carriage returns gone."""
    return re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", raw.replace("\r", ""))


def measure(text):
    """Every benchmark's numbers, keyed by (benchmark, metric).

    Each parser is given the whole log rather than a slice of it: the
    benchmarks announce themselves distinctly enough, and slicing a hardware
    capture on prompts assumes a prompt this parser has never seen.
    """
    results = {}
    for name, parser in PARSERS:
        for metric, value, unit in parser(text):
            results[(name, metric)] = (value, unit)
    return results


def load_fat_helper(wikireader):
    helper = wikireader / "emulator/tools/mem_dma_bench/run.py"
    spec = importlib.util.spec_from_file_location("wr_fat_fixture", helper)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run_emulator(args):
    """Boot the way the device boots.

    Loading the NuttX ELF straight into the emulator leaves the machine in a
    state it is never in on the device: the boot loader and grifo between
    them program the SDRAM controller, the refresh counters and the PLL, and
    a direct load does none of it. Every one of those has cost a bug, and
    the PLL one was expensive -- the timer block ran at the crystal's 48 MHz
    while NuttX divided for the 60 MHz grifo would have set, so every
    interval the guest measured came out a quarter long and every fitted
    parameter in the model quietly absorbed it.

    So this runs grifo, off a card holding the application as init.app,
    which is the name grifo chains to when it has nothing else to do. The
    same binary, the same loader, the same machine state. ubench's alu loop
    then measures 15.15 cycles a pass against the device's 15.15, where the
    direct boot managed 18.15.
    """
    emulator = args.wikireader.resolve() / "emulator/wremu"
    if not emulator.exists():
        print(f"SKIP: {emulator} is missing; build the emulator first")
        return None

    grifo = args.grifo.resolve()
    app = args.app.resolve()
    for needed in (grifo, app):
        if not needed.exists():
            print(f"SKIP: {needed} is missing")
            return None

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    # sdbench writes a couple of megabytes and reads them back, so the card
    # image has to be writable -- wremu's -R, which the tests here pass
    # because they only read, would fail every write with EIO.
    fat = load_fat_helper(args.wikireader.resolve())
    card = out / "card.img"
    # The card's shape decides what sdbench measures: fs_fat32.c clips a
    # multi-sector read to the sectors left in the cluster, so one sector a
    # cluster means the driver never issues CMD18 and the same read takes
    # 9.78 s where 32 KiB clusters take 2.63. The default is what every
    # earlier run used; `cardb` on the device prints its own geometry, and
    # matching it here is the difference between comparing the card and
    # comparing two formattings.
    fat.make_image(card, {"init.app": app.read_bytes()},
                   args.sectors_per_cluster)

    (out / "input.txt").write_text("bench\n")

    # Late enough that the prompt is there to type at: grifo has to bring the
    # card up and load three megabytes off it before NuttX starts.
    command = [str(emulator), "-c", str(card), "-n", str(args.limit),
               "--uart-input", str(out / "input.txt"),
               "--uart-start", "250000000", "--uart-gap", "200000",
               str(grifo)]
    with (out / "benchmarks.log").open("w") as log:
        # The device these numbers are compared against is one of the 32 MB
        # boards -- its own SDRAM controller reports ADDRC 3 -- and the
        # emulator defaults to the 16 MB kind. Model the machine being
        # compared with, unless the caller has said otherwise.
        env = {**os.environ, "WREMU_HOLD_MS": "33"}
        env.setdefault("WREMU_BOARD_REV", "7")

        subprocess.run(command, cwd=out, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=7200, env=env)

    console = clean(
        (out / "benchmarks.log").read_bytes().decode(errors="replace"))
    (out / "console.txt").write_text(console)

    # The results are on the card, not on the console: the point of the
    # device application is that the console is a 39-column panel.
    written = fat.read_file(card, RESULTS)
    if not written:
        print(f"warning: nothing was written to the card; "
              f"reading the console instead, see {out / 'console.txt'}")
        return console

    text = clean(written.decode(errors="replace"))
    (out / RESULTS).write_text(text)
    return text


def cell(value, unit):
    return "-" if value is None else f"{value:,.2f} {unit}".rstrip()


def report(emulated, hardware):
    rows = list(emulated) + [k for k in hardware if k not in emulated]
    if not rows:
        raise SystemExit("nothing was measured; see the console log")

    names = max(len(name) for name, _ in rows)
    metrics = max(len(metric) for _, metric in rows)
    values = 16

    header = (f"{'benchmark':<{names}}  {'metric':<{metrics}}  "
              f"{'emulator':>{values}}")
    if hardware:
        header += f"  {'device':>{values}}  {'ratio':>6}"
    print(header)
    print("-" * len(header))

    last = None
    for key in rows:
        name, metric = key
        shown = name if name != last else ""
        last = name
        value, unit = emulated.get(key, (None, ""))
        other, _ = hardware.get(key, (None, ""))
        line = (f"{shown:<{names}}  {metric:<{metrics}}  "
                f"{cell(value, unit):>{values}}")
        if hardware:
            ratio = ("-" if None in (value, other) or not other
                     else f"{value / other:.2f}x")
            line += f"  {cell(other, unit):>{values}}  {ratio:>6}"
        print(line)


def main():
    root = Path(__file__).resolve().parents[5]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wikireader", type=Path,
                        default=Path.home() / "wikireader")
    parser.add_argument("--app", type=Path, default=root / "nuttx/nuttx.app",
                        help="the application, installed on the card as "
                             "init.app")
    parser.add_argument("--grifo", type=Path,
                        default=root / "samo-lib/grifo/grifo.elf",
                        help="the kernel the device boots; see run_emulator")
    parser.add_argument("--out", type=Path,
                        default=root / "build/wikireader/benchmarks")
    parser.add_argument("--limit", type=int, default=20_000_000_000)
    parser.add_argument("--sectors-per-cluster", type=int, default=1,
                        help="cluster size of the emulated card, in 512-byte "
                             "sectors; match the device's, which cardb "
                             "reports in its geometry line")
    parser.add_argument("--compare", type=Path,
                        help="a captured device session to compare against")
    parser.add_argument("--parse", type=Path,
                        help="read a console log instead of running anything")
    args = parser.parse_args()

    if args.parse:
        text = clean(args.parse.read_bytes().decode(errors="replace"))
    else:
        text = run_emulator(args)
        if text is None:
            return 0

    hardware = {}
    if args.compare:
        hardware = measure(
            clean(args.compare.read_bytes().decode(errors="replace")))

    report(measure(text), hardware)
    return 0


if __name__ == "__main__":
    sys.exit(main())
