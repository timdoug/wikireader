#!/usr/bin/env python3
"""Application header generator, an alternative to the awk script.

The Makefile invokes it as

    $(AWK) -f GenerateApplicationHeader.awk foo.c

so the -f and its argument are accepted and ignored.

Header layout, matching menu.c:

    struct { uint32_t magic;   /* "SAMO" */
             uint32_t count;
             char names[count][32]; }

Both generators emit 32-byte names, matching the C structure that consumes
the header.
"""

import re
import sys

NAME_LENGTH = 32
TITLE_RE = re.compile(
    r'^\s*#\s*define\s+APPLICATION_TITLE[0-9]*\s+"([^"]*)"')


def main(argv):
    args = [a for a in argv[1:]]
    if args and args[0] == "-f":
        args = args[2:]                      # drop -f <script>
    names = []
    for path in args:
        with open(path, "r", errors="replace") as fh:
            for line in fh:
                m = TITLE_RE.match(line)
                if m and m.group(1):
                    names.append(m.group(1))
    if not names:
        names = ["No Name"]

    out = sys.stdout.buffer
    out.write(b"SAMO")
    out.write(bytes([len(names), 0, 0, 0]))
    for n in names:
        out.write(n.encode("ascii", "replace")[:NAME_LENGTH]
                  .ljust(NAME_LENGTH, b"\0"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
