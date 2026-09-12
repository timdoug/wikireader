#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that the Forth compiles itself into exactly what gforth compiles it into.

There are two ways to turn ansi-forth.fs into assembly: hand it to gforth, or
hand it to the Forth that came out of it last time, running inside the
emulator.  The second is what lets this build work on a machine with no Forth
on it, and it is only worth having if the two agree completely -- a
metacompiler that is nearly right produces an image that starts, prints its
banner, and is wrong somewhere in the middle.

So this runs both and compares the bytes.  It needs gforth, because gforth is
the thing being agreed with; without it there is nothing to check against and
the test skips.
"""

import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def main():
    here = Path(__file__).resolve().parent
    root = here.parents[1]

    if shutil.which("gforth") is None:
        print("SKIP: gforth is what the bootstrap is checked against")
        return 0
    for needed in (root / "emulator/wremu", here / "seed/wrforth-seed.app"):
        if not needed.exists():
            print(f"SKIP: {needed} is missing")
            return 0

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        symbols = work / "forth-symbols.fi"
        with symbols.open("w") as out:
            subprocess.run(["awk", "-f", str(here / "sym.awk"),
                            str(here / "ansi-forth.fs"),
                            str(here / "forth-vector.fi")],
                           stdout=out, check=True)

        reference = work / "gforth.s"
        with reference.open("w") as out:
            subprocess.run(["gforth", str(here / "meta.fs"), str(symbols),
                            str(here / "ansi-forth.fs"),
                            str(here / "forth-vector.fi"), "-e", "bye"],
                           stdout=out, check=True)

        mine = work / "seed.s"
        subprocess.run([sys.executable, str(here / "bootstrap.py"),
                        "--output", str(work / "full.s"),
                        "--body", str(mine)], check=True,
                       stdout=subprocess.DEVNULL)

        want = reference.read_bytes()
        want = want[want.index(b";;; Meta Compiler starting"):]
        got = mine.read_bytes()

        if want != got:
            (work / "differs").mkdir()
            for name, data in (("gforth.s", want), ("seed.s", got)):
                (Path.cwd() / f"bootstrap-{name}").write_bytes(data)
            index = next((i for i in range(min(len(want), len(got)))
                          if want[i] != got[i]), min(len(want), len(got)))
            raise SystemExit(
                f"the two disagree at byte {index} of {len(want)}; "
                f"wrote bootstrap-gforth.s and bootstrap-seed.s here")

    print(f"PASS: the Forth metacompiled itself into gforth's {len(want)} "
          f"bytes exactly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
