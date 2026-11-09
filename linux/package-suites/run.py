#!/usr/bin/env python3
"""Run the package suites package-suites/build.py made, each in Linux in
wremu through Grifo (ltp/run.py), and print each case's result.  Output in
linux/artifacts/package-suites/NAME-run.

    package-suites/run.py [NAME ...]
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
SUITES = ROOT / "linux/artifacts/package-suites"
# Guest seconds a case may take.  expat's one program holds all 5,000-odd
# of its checks.
TIMEOUTS = {"zlib": 600, "expat": 14400, "fontconfig": 1200, "pixman": 1200, "libdrm": 120}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("suites", nargs="*", default=list(TIMEOUTS))
    args = parser.parse_args()
    failed = False
    for name in args.suites:
        log = SUITES / f"{name}-run.log"
        cases = len(json.loads((SUITES / name / "manifest.json").read_text())["cases"])
        # The host deadline bounds the whole batch.
        wall = 2 * cases * TIMEOUTS[name] + 3600
        with log.open("w") as out:
            r = subprocess.run([sys.executable, str(ROOT / "linux/ltp/run.py"),
                                "--binaries", str(SUITES / name),
                                "--output", str(SUITES / f"{name}-run"),
                                "--timeout", str(TIMEOUTS[name]),
                                "--wall-timeout", str(wall)],
                               stdout=out, stderr=subprocess.STDOUT)
        failed |= r.returncode != 0
        for line in log.read_text(errors="replace").splitlines():
            if line.split(":", 1)[0] in ("PASS", "FAIL", "SKIPPED", "TIMEOUT", "ERROR", "MISSING"):
                print(line)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
