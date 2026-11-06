#!/usr/bin/env python3
"""Run the card's `r` under C33 Linux in wremu, booted the device's way (MBR
flash -> Grifo -> init.app -> linux.app, through linux/ask-app.py), and
check what Rust's standard library did: arguments, the environment, a file
round trip, four threads over a channel, and a clock.  The FAT fixture
cannot make folders, so the shell puts r in /mnt/sd/bin first."""

from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def main():
    program = ROOT / "rust/stdhello/stdhello.bin"
    if not program.exists():
        raise SystemExit(f"missing: {program} (rust/build-linux.sh)")
    out = ROOT / "rust/stdhello/ask"
    result = subprocess.run(
        [sys.executable, str(ROOT / "linux/ask-app.py"),
         "--file", f"stdhello.bin={program}",
         "--file", f"r.sh={ROOT / 'rust/card/bin/r'}", "--output", str(out),
         "mkdir -p /mnt/sd/bin && cp /mnt/sd/r.sh /mnt/sd/bin/r && r && cat /mnt/sd/rust.txt"],
        capture_output=True, timeout=600)
    log = (result.stdout + result.stderr).decode(errors="replace").replace("\r", "")
    sums = ", ".join(f"{i}: {500500 * (i + 1)}" for i in range(4))
    expected = [
        'stdhello: 3 args, first Some("/mnt/sd/stdhello.bin")',
        'stdhello: HOME=Some("/root")',
        "stdhello: file round trip: written by Rust on the C33",
        f"stdhello: thread sums {{{sums}}}",
    ]
    missing = [line for line in expected if line not in log]
    if (missing or "panicked" in log or "PASS (rust.txt)" not in log
            or not re.search(r"stdhello: done in [\d.]+m?s", log)):
        print(log, file=sys.stderr)
        raise SystemExit("stdhello failed; missing: " + ", ".join(missing or ["done"]))
    print("stdhello passed under C33 Linux: args, env, files, threads, channel, time")


if __name__ == "__main__":
    main()
