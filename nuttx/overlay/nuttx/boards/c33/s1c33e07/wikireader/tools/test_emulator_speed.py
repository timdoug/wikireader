#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare emulator wall time while requiring identical guest work and pixels."""

import argparse
import json
from pathlib import Path
import re
import selectors
import statistics
import subprocess
import time


def measure(emulator, image, folder):
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "input.txt").write_text("ps; echo PS_DONE\n")
    command = [str(emulator), "-n", "100000000", "--uart-input",
               str(folder / "input.txt"), "--uart-start", "20000000", str(image)]
    (folder / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    data = b""
    start = end = None
    with subprocess.Popen(command, cwd=folder, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT) as process:
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                deadline = time.monotonic() + 60
                while True:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0 or not selector.select(remaining):
                        raise TimeoutError(f"Emulator timed out: {folder}")
                    chunk = process.stdout.read1(4096)
                    if not chunk:
                        break
                    now = time.monotonic()
                    data += chunk
                    clean = data.replace(b"\r", b"")
                    if start is None and b"ps; echo PS_DONE\n" in clean:
                        start = now
                    if end is None and b"\nPS_DONE\nnsh> \x1b[K" in clean:
                        end = now
            if process.wait(timeout=5) != 0 or start is None or end is None:
                raise RuntimeError(f"ps did not finish: {folder}")
        finally:
            if process.poll() is None:
                process.kill()
            (folder / "emulator.log").write_bytes(data)
    if re.search(rb"PANIC|Assertion|stop reason:|fault:", data):
        raise RuntimeError(f"Guest failure: {folder}")
    work = re.search(rb"--- work: .* ---", data)
    if work is None:
        raise RuntimeError(f"Missing guest execution count: {folder}")
    return end - start, work[0], data.split(b"--- timer:", 1)[0]


def main():
    root = Path(__file__).resolve().parents[5]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path,
                        default=Path.home() / "wikireader/emulator/wremu")
    parser.add_argument("--image", type=Path,
                        default=root / "build/wikireader/lcd/kernel.elf")
    parser.add_argument("--out", type=Path,
                        default=root / "build/wikireader/emulator-speed-bench")
    args = parser.parse_args()
    out = args.out.resolve()
    times = {"before": [], "after": []}
    reference = None
    for trial in range(3):
        for name in ("before", "after"):
            folder = out / f"{name}-{trial}"
            elapsed, work, console = measure(getattr(args, name).resolve(),
                                             args.image.resolve(), folder)
            result = (work, console, (folder / "screen.pgm").read_bytes())
            if reference is None:
                reference = result
            elif result != reference:
                raise SystemExit("Emulator changed guest work, console output or pixels")
            times[name].append(round(elapsed, 4))
    medians = {name: round(statistics.median(values), 4)
               for name, values in times.items()}
    metrics = {"command_to_prompt_host_seconds": times, "median_seconds": medians,
               "speedup": round(medians["before"] / medians["after"], 2),
               "identical_guest_work": reference[0].decode()}
    (out / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    print(f"PASS: identical guest work/output/pixels; median ps wall time "
          f"{medians['before']} -> {medians['after']} s "
          f"({metrics['speedup']}x). Metrics: {out / 'metrics.json'}")


if __name__ == "__main__":
    main()
