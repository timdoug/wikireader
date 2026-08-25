#!/usr/bin/env python3
"""Application header generator, a drop-in for the gawk script.

The Makefile invokes it as

    $(AWK) -f GenerateApplicationHeader.awk foo.c

so the -f and its argument are accepted and ignored. Exists because the awk
version uses gensub(), a gawk extension, and gawk is not everywhere.

Header layout, matching menu.c:

    struct { uint32_t magic;   /* "SAMO" */
             uint32_t count;
             char names[count][32]; }

Note on a discrepancy: the awk original writes each name with
substr(name, 0, 32). gawk clamps a start index below one to one but keeps
the end position, so that yields 31 characters, not 32, while menu.c reads
NameType[32]. Names after the first would be read shifted. This emits 32,
matching the C structure that consumes it.
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
