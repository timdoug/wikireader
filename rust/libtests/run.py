#!/usr/bin/env python3
"""Run core's and alloc's test suites (built by build.sh, in the parts
split.py makes) under C33 Linux in wremu, booted the device's way through
linux/ask-app.py, one part a boot, and report each part's result line and
its failures.  Each test runs on a thread of its own, and std's default
2 MB stack is that much memory without an MMU; after a few thousand tests
the next one could not be found contiguous, so RUST_MIN_STACK makes it
256 KB.

    run.py [PART...]      e.g. coretests_1; all six by default
"""

from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
OUT = HERE / "target/c33-unknown-linux-musl/release/build"


def binary(part):
    package, k = part.replace("tests", "").split("_")
    suite = part.split("_")[0]
    found = [p for p in OUT.glob(f"{package}-{k}/*/out/{suite}-*") if "." not in p.name]
    if not found:
        raise SystemExit(f"no {part}: run build.sh")
    return max(found, key=lambda p: p.stat().st_mtime)


def run(part):
    name = part.replace("tests_", "")[:8] + ".bin"      # an 8.3 name
    out = HERE / "ask" / part
    result = subprocess.run(
        [sys.executable, str(ROOT / "linux/ask-app.py"),
         "--cycles", "200000000000", "--file", f"{name}={binary(part)}",
         "--output", str(out),
         # Straight to the serial console, so serial.log shows progress
         # (a character a test) while it runs.
         f"RUST_MIN_STACK=262144 /mnt/sd/{name} --test-threads=1 -q; echo rc=$?"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    log = (out / "serial.log").read_text(errors="replace").replace("\r", "")
    summary = re.search(r"test result: .*", log)
    rc = re.search(r"^rc=(\d+)", log, re.M)
    print(f"{part}: rc={rc.group(1) if rc else '?'} "
          f"{summary.group(0) if summary else 'no result line'}", flush=True)
    failures = re.findall(r"^    (\S+)$", log, re.M)
    for f in failures:
        print(f"    FAILED {f}", flush=True)
    for m in re.findall(r"nommu: .*|Segmentation fault|Illegal instruction|panicked at .*", log):
        print(f"    {m}", flush=True)


def main():
    parts = sys.argv[1:] or [f"{s}_{k}" for s in ("coretests", "alloctests") for k in (1, 2, 3)]
    for part in parts:
        run(part)


if __name__ == "__main__":
    main()
