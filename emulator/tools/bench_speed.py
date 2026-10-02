#!/usr/bin/env python3
"""Compare host speed through the full boot, requiring identical guest results."""

import argparse
import json
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path,
                        default=Path(__file__).resolve().parents[1] / "wremu")
    parser.add_argument("--flash", type=Path, required=True)
    parser.add_argument("--card", type=Path, required=True)
    parser.add_argument("--writable-card", action="store_true",
                        help="give each run a fresh writable copy of the card")
    parser.add_argument("--instructions", type=int, default=400_000_000)
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--require-text", action="append", default=[],
                        help="require a guest completion marker; repeatable")
    parser.add_argument("--uart-input", type=Path)
    parser.add_argument("--uart-start", type=int, default=250_000_000)
    parser.add_argument("--uart-gap", type=int, default=50_000)
    args = parser.parse_args()
    if args.trials < 1 or args.instructions < 1 or args.timeout <= 0:
        parser.error("trials, instructions and timeout must be positive")

    # Each run gets its own output folder. Writable workloads need a fresh
    # fixture each time, so neither binary can inherit the other's SD writes.
    card = args.card.resolve()
    guest_args = ["-n", str(args.instructions), "-e", str(args.flash.resolve()),
                  "-c", "card.img" if args.writable_card else str(card)]
    if not args.writable_card:
        guest_args += ["-R"]
    if args.uart_input:
        guest_args += ["--uart-input", str(args.uart_input.resolve()),
                       "--uart-start", str(args.uart_start),
                       "--uart-gap", str(args.uart_gap)]
    out = args.out.resolve()
    times = {"before": [], "after": []}
    reference = None
    for trial in range(args.trials):
        # Alternate order to reduce bias from host load and thermal drift.
        order = ("before", "after") if trial % 2 == 0 else ("after", "before")
        for name in order:
            folder = out / f"{name}-{trial}"
            folder.mkdir(parents=True, exist_ok=True)
            (folder / "screen.pgm").unlink(missing_ok=True)
            if args.writable_card:
                target = folder / "card.img"
                if target.resolve() == card:
                    raise SystemExit("The output folder would overwrite the source card")
                target.unlink(missing_ok=True)
                # Preserve sparse multi-GB fixtures; prefer a filesystem clone.
                flags = ["-c"] if sys.platform == "darwin" else ["--reflink=auto", "--sparse=always"]
                copied = subprocess.run(["cp", *flags, str(card), str(target)],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                if copied.returncode:
                    shutil.copyfile(card, target)
            command = [str(getattr(args, name).resolve()), *guest_args]
            (folder / "command.json").write_text(json.dumps(command, indent=2) + "\n")
            start = time.monotonic()
            with (folder / "emulator.log").open("wb") as log:
                process = subprocess.run(command, cwd=folder, stdout=log,
                                         stderr=subprocess.STDOUT, timeout=args.timeout)
            elapsed = time.monotonic() - start
            console = (folder / "emulator.log").read_bytes()
            work = re.search(rb"--- work: (\d+) instructions executed, .* ---", console)
            if (process.returncode or not work or int(work[1]) == 0 or
                    re.search(rb"^stop reason:|^fault:|PANIC|Kernel panic|Assertion|^--- resets:",
                              console, re.MULTILINE)):
                raise SystemExit(f"Guest failure; see {folder / 'emulator.log'}")
            for marker in args.require_text:
                if marker.encode() not in console:
                    raise SystemExit(f"Missing guest marker {marker!r}: {folder}")
            result = (console, (folder / "screen.pgm").read_bytes())
            if reference is None:
                reference = result
            elif result != reference:
                raise SystemExit(f"Guest output, work or pixels changed: {folder}")
            times[name].append(elapsed)
            print(f"{name} {trial + 1}: {elapsed:.3f} s", flush=True)

    medians = {name: statistics.median(values) for name, values in times.items()}
    metrics = {"host_seconds": times, "median_seconds": medians,
               "speedup": medians["before"] / medians["after"],
               "identical_guest_work_output_pixels": True,
               "guest_work": work[0].decode()}
    (out / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    print(f"PASS: identical guest work/output/pixels; full-run median "
          f"{medians['before']:.3f} -> {medians['after']:.3f} s "
          f"({metrics['speedup']:.2f}x). Metrics: {out / 'metrics.json'}")


if __name__ == "__main__":
    main()
