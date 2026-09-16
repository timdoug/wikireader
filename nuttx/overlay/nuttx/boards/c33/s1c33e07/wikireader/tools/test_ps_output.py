#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Measure ps output cost and detect a return to character-at-a-time rendering."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

import wr_boot
from test_terminal import read_pgm, run, write_png

COMMAND = "ps; echo PS_DONE\n"
PROBES = ("cmd_ps", "nxterm_write", "nx_bitmap", "pty_write")


def results(folder):
    text = (folder / "emulator.log").read_bytes().decode(
        errors="replace").replace("\r", "")
    if "\nPS_DONE\nnsh> " not in text:
        raise SystemExit(f"ps did not finish: {folder}")
    probes = {}
    pattern = (r"--- probe (\w+)\s+(\d+) hits\s+first\s+(\d+)/\s*"
               r"([\d.]+)ms\s+last\s+(\d+)/\s*([\d.]+)ms")
    for name, hits, first, first_ms, last, last_ms in re.findall(pattern, text):
        probes[name] = {"hits": int(hits), "first": int(first),
                        "first_ms": float(first_ms), "last": int(last),
                        "last_ms": float(last_ms)}
    if any(name not in probes for name in PROBES) or probes["cmd_ps"]["hits"] != 1:
        raise SystemExit(f"Missing ps output probes: {folder}")
    work = re.search(r"--- work: (\d+) instructions executed", text)
    if work is None:
        raise SystemExit(f"Missing instruction count: {folder}")
    console = text.split("WIKIREADER_TERMINAL_READY\n", 1)[1].split("--- timer:", 1)[0]
    for expected in ("SIGMASK", "COMMAND", "Idle_Task", "wr_touch", "wr_nx", "nsh"):
        if expected not in console:
            raise SystemExit(f"Missing ps field/task: {expected}")
    metrics = {
        "elf_sha256": hashlib.sha256((folder / "kernel.elf").read_bytes()).hexdigest(),
        "executed_instructions": int(work[1]),
        "terminal_writes": probes["nxterm_write"]["hits"],
        "bitmap_updates": probes["nx_bitmap"]["hits"],
        "output_instructions": probes["nx_bitmap"]["last"] -
                               probes["cmd_ps"]["first"],
        "output_guest_ms": round(probes["nx_bitmap"]["last_ms"] -
                                 probes["cmd_ps"]["first_ms"], 1),
        "probes": probes,
    }
    return metrics, console


def main():
    root = Path(__file__).resolve().parents[5]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wikireader", type=Path, default=Path.home() / "wikireader")
    parser.add_argument("--image", type=Path, default=root / "nuttx")
    parser.add_argument("--out", type=Path,
                        default=root / "build/wikireader/ps-output")
    parser.add_argument("--baseline", type=Path, help="Earlier benchmark output directory")
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
               "-n", str(100000000 + wr_boot.BOOT_CYCLES), "--uart-input",
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
    write_png(out / "screen.png", read_pgm(out / "screen.pgm"))
    if args.baseline:
        baseline = args.baseline.resolve()
        if (baseline / "input.txt").read_text() != COMMAND:
            raise SystemExit("Baseline used a different workload")
        old, old_console = results(baseline)
        # ps prints pthread entry addresses, which move when the ELF changes.
        normalized = lambda s: re.sub(r"0x[0-9a-f]{8}\b", "<address>", s)
        if normalized(console) != normalized(old_console):
            raise SystemExit("ps output changed beyond linked entry addresses")
        metrics["baseline"] = old
        metrics["output_speedup"] = round(old["output_guest_ms"] /
                                          metrics["output_guest_ms"], 2)
    (out / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    # The budget is the span from the command to the last pixel, not the
    # whole run: booting the way the device boots puts the loader, grifo and
    # three megabytes off the card in front of the prompt, and that is not
    # what this test is measuring.
    if (metrics["output_instructions"] > 8_000_000 or
            metrics["terminal_writes"] > 120 or metrics["bitmap_updates"] > 160):
        raise SystemExit(f"ps output exceeded performance budget: {metrics}")
    print(f"PASS: ps completed with {metrics['terminal_writes']} terminal writes, "
          f"{metrics['bitmap_updates']} bitmap updates, "
          f"{metrics['output_instructions']:,} output instructions, "
          f"{metrics['output_guest_ms']} ms guest output span. "
          f"Metrics: {out / 'metrics.json'}")


if __name__ == "__main__":
    main()
