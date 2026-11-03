#!/usr/bin/env python3
"""Run the musl regression programs (musl/tests.sh builds them) on Linux in
wremu, through Grifo, with ask-app.py: they and musl's ld.so ride on the
card's FAT partition, beside the root image, whose C library they do not
use.  The shared runtimes, whose names are not 8.3, are copied to /tmp.

    run-tests.py [--only NAME,...]
"""
import argparse
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
art = root / "linux/artifacts/musl"

# Program, its arguments, and what it prints when it passes.
TESTS = [
    ("smokes.bin", "", "C33 uClibc smoke: pid="),
    ("smoke.bin", "", "C33 uClibc smoke: pid="),
    ("reloc.bin", "", "RELOCATION PASS"),
    ("orign.bin", "", "ORIGIN PASS"),
    ("execstk.bin", "", "EXEC STACK PASS"),
    ("devmem.bin", "", "DEVMEM PASS"),
    ("glob.bin", "", "GLOB PASS"),
    ("time64.bin", "", "TIME64 PASS"),
    ("malloc.bin", "", "MALLOC PASS"),
    ("sigtest.bin", "", "SIGNAL PASS:"),
    ("pthtest.bin", "", "PTHREAD PASS"),
    ("pthstat.bin", "", "PTHREAD PASS"),
    ("nptl.bin", "", "NPTL PASS"),
    ("atomtest.bin", "", "ATOMIC PASS"),
    ("ipctest.bin", "", "IPC PASS"),
    ("cxxtest.bin", "", "CXX PASS"),
    ("bench.bin", "--single", "BENCH PASS"),
]
RUNTIMES = {"gcc_s.so": "libgcc_s.so.1", "stdcxx.so": "libstdc++.so.6",
            "atomic.so": "libatomic.so.1"}

parser = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("--only", help="comma-separated programs to run")
parser.add_argument("--cycles", default="8000000000")
args = parser.parse_args()
tests = TESTS
if args.only:
    wanted = set(args.only.split(","))
    tests = [t for t in TESTS if t[0] in wanted or t[0].split(".")[0] in wanted]

files = sorted(p.name for p in art.iterdir())
setup = ["mkdir -p /tmp/musl"] + [f"cp /mnt/sd/{card} /tmp/musl/{name}"
                                  for card, name in RUNTIMES.items()]
# One line: hush discards type-ahead each time it prompts.
line = " && ".join(setup) + " && export LD_LIBRARY_PATH=/tmp/musl:/mnt/sd && cd /mnt/sd; " + \
    "; ".join(f"./{name} {argv}; echo EXIT {name} $?" for name, argv, _ in tests)
command = [sys.executable, str(root / "linux/ask-app.py"), "--cycles", args.cycles,
           "--output", str(root / "linux/artifacts/musl-run")]
for name in files:
    command += ["--file", f"{name}={art / name}"]
command.append(line)
text = subprocess.run(command, capture_output=True, text=True, errors="replace").stdout
log = (root / "linux/artifacts/musl-run/serial.log").read_text(errors="replace").replace("\r", "")

failed = []
for name, _, marker in tests:
    status = f"EXIT {name} 0"
    ok = status in log and marker in log
    print(f"{'PASS' if ok else 'FAIL'} {name}")
    if not ok:
        failed.append(name)
if failed:
    print("musl regressions failed: " + ", ".join(failed) +
          "; see linux/artifacts/musl-run/serial.log", file=sys.stderr)
    sys.exit(1)
print("musl regressions passed")
