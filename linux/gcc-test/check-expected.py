#!/usr/bin/env python3
"""Compare a GCC testsuite summary with its expected results.

    check-expected.py SUM [LIST]

LIST (default: expected/TOOL.txt beside this script, TOOL from SUM's name)
holds every result other than a pass that a run gives -- FAIL, XPASS,
UNRESOLVED and ERROR lines, grouped under comments naming their cause.
Prints the lines SUM has that LIST does not, and the reverse, and exits 1
if SUM has a line LIST does not.  UNSUPPORTED results are not compared.
"""
from pathlib import Path
import re
import sys

RESULT = re.compile(r"^(FAIL|XPASS|UNRESOLVED|ERROR): ")


def results(path):
    return {line.rstrip() for line in open(path, errors="replace") if RESULT.match(line)}


def main():
    if not 2 <= len(sys.argv) <= 3:
        sys.exit(__doc__.strip())
    summary = Path(sys.argv[1])
    listed = Path(sys.argv[2]) if len(sys.argv) == 3 else \
        Path(__file__).with_name("expected") / (summary.stem + ".txt")
    got = results(summary)
    expected = {line.rstrip() for line in open(listed)
                if line.strip() and not line.startswith("#")}
    new = sorted(got - expected)
    gone = sorted(expected - got)
    for line in new:
        print("new:", line)
    for line in gone:
        print("no longer seen:", line)
    print(f"{summary.name}: {len(got)} results other than a pass, "
          f"{len(new)} not in {listed.name}, {len(gone)} listed and not seen")
    sys.exit(1 if new else 0)


if __name__ == "__main__":
    main()
