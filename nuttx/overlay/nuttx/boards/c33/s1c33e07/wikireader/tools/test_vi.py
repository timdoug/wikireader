#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Drive the VI editor on the WikiReader's own LCD terminal.

The point of the test is that the screen is read back, not just that the
editor exits cleanly.  The framebuffer is decoded into characters using the
same font the terminal renders with, so a cursor-addressed full-screen
redraw either lands in the right cells or the comparison fails.  Escape
sequences that NxTerm does not understand used to be drawn as text, which
this would catch as garbage in the decoded screen.
"""

import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]
WREMU = Path.home() / "wikireader/emulator/wremu"
FONT = ROOT / "libs/libnx/nxfonts/nxfonts_x11-misc-fixed-6x9.h"
OUT = ROOT / "build/wikireader/vi"

FBADDR = 0x00080000
WIDTH, HEIGHT, STRIDE = 240, 208, 32
CELLW, CELLH = 6, 9          # font width/height, CONFIG_NXTERM_LINESEPARATION=0
XORIGIN, YORIGIN = 6, 0      # nxterm_home(): one space in from the left
COLS, ROWS = 39, 13          # the terminal window is the top 120 rows


def glyphs():
    """code -> tuple of CELLH rows, each a tuple of CELLW booleans."""
    text = FONT.read_text()
    table = {}
    for code, body in re.findall(r"#define NXFONT_BITMAP_(\d+)\s+\{([^}]*)\}",
                                 text):
        rows = [int(v, 0) for v in body.split(",")]
        if len(rows) != CELLH:
            continue
        table[chr(int(code))] = tuple(
            tuple(bool(r & (0x80 >> x)) for x in range(CELLW)) for r in rows)
    table[" "] = tuple((False,) * CELLW for _ in range(CELLH))
    return table


def screen(fb, table):
    """Decode the terminal region of a framebuffer dump.

    Returns the text rows and the set of (row, col) cells that are drawn in
    reverse video, which is how the cursor marks its position.
    """
    def pixel(x, y):
        return bool(fb[y * STRIDE + (x >> 3)] & (0x80 >> (x & 7)))

    lines = []
    reversed_cells = set()
    for row in range(ROWS):
        out = ""
        for col in range(COLS):
            x0 = XORIGIN + col * CELLW
            y0 = YORIGIN + row * CELLH
            cell = tuple(tuple(pixel(x0 + x, y0 + y) for x in range(CELLW))
                         for y in range(CELLH))
            if not any(any(r) for r in cell):
                # Several codes share a blank glyph; prefer a space.
                out += " "
                continue

            match = [c for c, g in table.items() if g == cell]
            if match:
                out += match[0]
                continue

            # No glyph matches, so try the cell inverted.  A character under
            # the cursor is shown with its foreground and background
            # exchanged, which at one bit per pixel is the complement.
            flipped = tuple(tuple(not p for p in r) for r in cell)
            match = [c for c, g in table.items() if g == flipped]
            if match or not any(any(r) for r in flipped):
                reversed_cells.add((row + 1, col + 1))
                out += match[0] if match else " "
            else:
                out += "�"
        lines.append(out.rstrip())
    return lines, reversed_cells


def run(name, keys, limit=12_000_000_000, dump=None):
    OUT.mkdir(parents=True, exist_ok=True)
    script = OUT / ("%s-input.bin" % name)
    script.write_bytes(keys)
    cmd = [str(WREMU), "-n", str(limit), "--uart-input", str(script),
           "--uart-start", "60000000", "--uart-gap", "30000000"]
    if dump:
        cmd += ["-D", hex(FBADDR), "-L", str(STRIDE * HEIGHT),
                "-O", str(OUT / dump)]
    cmd.append(str(ROOT / "nuttx"))
    log = subprocess.run(cmd, capture_output=True, text=True,
                         errors="replace", timeout=1800).stdout
    (OUT / ("%s.log" % name)).write_text(log)
    return log


def fail(msg, lines=None):
    print("FAIL: %s" % msg)
    if lines is not None:
        for i, line in enumerate(lines, 1):
            print("  %2d |%s|" % (i, line))
    print("Artifacts: %s" % OUT)
    sys.exit(1)


def main():
    table = glyphs()

    # The board bridges UART0 into the same PTY that drives the LCD, so
    # keystrokes sent here are typed into the shell shown on the display.
    # vi is left running so that the exit-time framebuffer dump captures a
    # live full-screen redraw.
    run("display",
        b"echo 'int x = 42;' > /tmp/v.c\n" + b"vi /tmp/v.c\n",
        limit=4_000_000_000, dump="fb.bin")
    lines, inverted = screen((OUT / "fb.bin").read_bytes(), table)

    if "�" in "".join(lines):
        fail("undecodable glyph on screen", lines)

    # The cursor shows the character it is on in reverse video rather than
    # covering it, so the file's text is all still readable.
    if lines[0] != "int x = 42;":
        fail("file text missing from row 1", lines)

    if inverted != {(1, 1)}:
        fail("expected exactly the home cell in reverse video, got %s"
             % sorted(inverted), lines)

    # vi marks every line past the end of the file with a tilde, and puts a
    # row,column ruler on the bottom row.
    tildes = [i for i, line in enumerate(lines[1:-1], 2) if line == "~"]
    if len(tildes) < 8:
        fail("expected tilde filler rows, found %d" % len(tildes), lines)

    if not re.search(r"\d+,\d+", lines[-1]):
        fail("no ruler on the bottom row", lines)

    # An escape sequence rendered as text would show up as stray bracket or
    # digit runs outside the first and last rows.
    for i, line in enumerate(lines[1:-1], 2):
        if line not in ("", "~"):
            fail("unexpected text on row %d" % i, lines)

    print("Screen as decoded from the framebuffer:")
    for i, line in enumerate(lines, 1):
        print("  %2d |%s|" % (i, line))

    # Now a real edit, through the same on-LCD session: append to the line,
    # leave insert mode and write the file back out.  No throwaway keystroke
    # precedes the 'A'.  That matters: when the terminal cannot report its
    # size, termcurses falls back to probing for a cursor position report and
    # swallows whatever the user typed during the reply window.  Answering
    # TIOCGWINSZ skips the probe, so the first keystroke has to survive.
    log = run("edit",
              b"echo 'int x = 42;' > /tmp/v.c\nvi /tmp/v.c\n"
              b"A /* edited */\x1b:wq\n"
              b"cat /tmp/v.c\necho VI_TEST_DONE\n")

    if "VI_TEST_DONE" not in log:
        fail("edit session did not finish")

    if "int x = 42; /* edited */" not in log:
        fail("edit did not reach the file")

    print("PASS: vi renders a cursor-addressed screen on the LCD terminal "
          "and edits a file through it.")
    print("Artifacts: %s" % OUT)


if __name__ == "__main__":
    main()
