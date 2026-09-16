#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Benchmark scrolling through three NSH help listings in the emulator."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

import wr_boot
from test_terminal import read_pgm, run, write_png

COMMAND = "help; help; help; echo SCROLL_DONE\n"
PROBES = ("nxterm_scroll", "nxgl_moverectangle_1bpp", "cmd_help", "nxterm_write")


def results(folder):
    text = (folder / "emulator.log").read_text(
        errors="replace").replace("\r", "")
    if "\nSCROLL_DONE\nnsh> " not in text or text.count("help usage:") != 3:
        raise SystemExit(f"Incomplete scrolling workload: {folder}")
    probes = {}
    pattern = (r"--- probe (\w+)\s+(\d+) hits\s+first\s+(\d+)/\s*"
               r"([\d.]+)ms\s+last\s+(\d+)/\s*([\d.]+)ms")
    for name, hits, first, first_ms, last, last_ms in re.findall(pattern, text):
        probes[name] = {"hits": int(hits), "first": int(first),
                        "first_ms": float(first_ms), "last": int(last),
                        "last_ms": float(last_ms)}
    if any(name not in probes for name in PROBES):
        raise SystemExit(f"Missing scrolling probes: {folder}")
    scrolls = probes["nxterm_scroll"]["hits"]
    if (scrolls < 50 or scrolls != probes["nxgl_moverectangle_1bpp"]["hits"] or
            probes["cmd_help"]["hits"] != 3):
        raise SystemExit(f"Unexpected scrolling workload: {probes}")
    work = re.search(r"--- work: (\d+) instructions executed", text)
    if work is None:
        raise SystemExit(f"Missing instruction count: {folder}")
    metrics = {
        "elf_sha256": hashlib.sha256((folder / "kernel.elf").read_bytes()).hexdigest(),
        "executed_instructions": int(work[1]),
        "scrolls": scrolls,
        # Function entry probes bracket the output, from the first help
        # command to the final NXTerm write (the returned shell prompt).
        "output_instructions": probes["nxterm_write"]["last"] -
                               probes["cmd_help"]["first"],
        "output_guest_ms": round(probes["nxterm_write"]["last_ms"] -
                                 probes["cmd_help"]["first_ms"], 1),
        "probes": probes,
    }
    console = text.split("WIKIREADER_TERMINAL_READY\n", 1)[1].split("--- timer:", 1)[0]
    return metrics, console


def main():
    root = Path(__file__).resolve().parents[5]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wikireader", type=Path, default=Path.home() / "wikireader")
    parser.add_argument("--image", type=Path, default=root / "nuttx")
    parser.add_argument("--out", type=Path,
                        default=root / "build/wikireader/scrolling")
    parser.add_argument("--baseline", type=Path, help="Earlier benchmark output directory")
    parser.add_argument("--max-instructions", type=int, default=25_000_000)
    args = parser.parse_args()
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    image = out / "kernel.elf"
    if args.image.resolve() != image:
        shutil.copy2(args.image, image)
    (out / "input.txt").write_text(COMMAND)
    wr = args.wikireader.resolve()
    nm = wr / "host-tools/toolchain-c33/work/install/bin/c33-epson-elf-nm"
    symbols = subprocess.check_output([str(nm), str(image)], text=True)
    (out / "symbols.txt").write_text(symbols)
    card = out / "card.img"
    wr_boot.make_card(card, image, wr)
    flash = wr_boot.make_flash(out, wr)
    command = [str(wr / "emulator/wremu"),
               "-n", str(400000000 + wr_boot.BOOT_CYCLES), "--uart-input",
               str(out / "input.txt"), "--uart-start", wr_boot.UART_START,
               "-F", str(out / "profile.txt")]
    for name in PROBES:
        match = re.search(r"^([0-9a-f]+) [tT] " + name + r"$", symbols, re.M)
        if match is None:
            raise SystemExit(f"Missing ELF symbol: {name}")
        command += ["-X", f"0x{match[1]},{name}"]
    command += wr_boot.boot_args(card, flash)
    (out / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    run(command, out, "emulator.log")
    metrics, console = results(out)
    (out / "console.txt").write_text(console)
    pixels = read_pgm(out / "screen.pgm")
    write_png(out / "screen.png", pixels)
    if args.baseline:
        baseline = args.baseline.resolve()
        if (baseline / "input.txt").read_text() != COMMAND:
            raise SystemExit("Baseline used a different workload")
        old, old_console = results(baseline)
        if console != old_console or pixels != read_pgm(baseline / "screen.pgm"):
            raise SystemExit("Scrolling changed the console output or screen pixels")
        metrics["baseline"] = old
        metrics["output_speedup"] = round(old["output_guest_ms"] /
                                          metrics["output_guest_ms"], 2)
    (out / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    # The scrolling itself, not the boot in front of it: the run now starts
    # at the mask ROM and reaches the prompt through grifo.
    if metrics["output_instructions"] > args.max_instructions:
        raise SystemExit(f"Scrolling exceeded instruction budget: {metrics}")
    print(f"PASS: {metrics['scrolls']} scrolls, "
          f"{metrics['output_instructions']:,} instructions, "
          f"{metrics['output_guest_ms']} ms guest output span. "
          f"Metrics: {out / 'metrics.json'}")


if __name__ == "__main__":
    main()
