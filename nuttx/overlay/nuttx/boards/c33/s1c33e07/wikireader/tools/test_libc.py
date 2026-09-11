#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run NuttX's own C library test suites on the board.

Three bugs in NuttX's libc turned up by accident while porting Toybox --
getdelim() believing an uninitialized size, an unimplemented
sysconf(_SC_ARG_MAX), and a strchr() family that never matched a byte over
0x7f -- which is an argument for running the tests that exist.

These are the suites under apps/testing/libc that build here. Each prints its
own result in its own format, so this checks for the specific success line
each one ends with, and fails on any "FAIL"/"Error" the guest prints along the
way.

The same commands work typed at the device's own prompt, which is the point:
a run here and a run on hardware are comparing the same thing.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

# command -> (regex that must match, regex that must NOT match)
SUITES = [
    ("arch_libctest", r"arch_libc_test Passed", None),
    ("atomic", r"atomic test complete!", None),
    ("fmemopen_test", r"fmemopen tests: SUCCESSFUL: \d+; FAILED: 0", None),
    ("fopencookie_test", r"fopencokie tests were successful", None),
    ("open_memstream_test",
     r"open_memstream tests: SUCCESSFUL: \d+; FAILED: 0", None),
    ("popen_test", r"=== Results: 0 failed ===", None),
    ("scanftest", r"Scanf tests done\.\.\. OK: \d+, FAILED: 0", None),
    ("wcstombs", r"Test Scenario:", None),
]

# Anything matching this in the transcript is a failure whatever the summary
# lines say.
TROUBLE = re.compile(r"\bFAIL\b|FAILED: [1-9]|Error opening|Assertion|PANIC|"
                     r"unmapped read|misaligned", re.I)


def run(emulator, image, out, limit):
    source = out / "input.txt"
    source.write_text("".join(cmd + "\n" for cmd, _, _ in SUITES))

    command = [str(emulator), "-R", "-n", str(limit),
               "--uart-input", str(source), "--uart-start", "20000000",
               "--uart-gap", "400000", str(image)]
    with (out / "libc.log").open("w") as log:
        subprocess.run(command, cwd=out, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=3600,
                       env={**os.environ, "WREMU_HOLD_MS": "33"})

    text = (out / "libc.log").read_bytes().decode(errors="replace")
    text = text.replace("\r", "")
    return re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", text)


def main():
    root = Path(__file__).resolve().parents[5]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wikireader", type=Path,
                        default=Path.home() / "wikireader")
    parser.add_argument("--image", type=Path, default=root / "nuttx")
    parser.add_argument("--out", type=Path,
                        default=root / "build/wikireader/libc")
    parser.add_argument("--limit", type=int, default=8_000_000_000)
    args = parser.parse_args()

    emulator = args.wikireader.resolve() / "emulator/wremu"
    if not emulator.exists():
        print(f"SKIP: {emulator} is missing; build the emulator first")
        return 0

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    text = run(emulator, args.image.resolve(), out, args.limit)
    (out / "console.txt").write_text(text)

    bad = [line for line in text.splitlines() if TROUBLE.search(line)]
    if bad:
        print("Guest reported failures:")
        for line in bad[:10]:
            print("   ", line.strip())
        raise SystemExit(f"see {out / 'console.txt'}")

    missing = [cmd for cmd, want, _ in SUITES
               if not re.search(want, text)]
    if missing:
        raise SystemExit(f"No success line from: {', '.join(missing)}; "
                         f"see {out / 'console.txt'}")

    checks = len(re.findall(r": PASSED", text)) + \
        sum(int(m) for m in re.findall(r"OK: (\d+)", text)) + \
        len(re.findall(r"\[PASS\]", text)) + \
        sum(int(m) for m in re.findall(r"SUCCESSFUL: (\d+)", text))
    print(f"PASS: {len(SUITES)} libc suites, {checks} checks; "
          f"{out / 'console.txt'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
