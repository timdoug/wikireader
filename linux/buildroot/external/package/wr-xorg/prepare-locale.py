#!/usr/bin/env python3
"""Restrict X font-set searches to the server's built-in font encodings.

Retain upstream's complete UTF-8 conversion definition. This changes only
which font encodings XCreateFontSet searches, not the accepted input text.
"""
from pathlib import Path
import sys

FONTSET = """# Built-in fonts: fixed is Latin-1; 5x8 is ISO10646-1.
XLC_FONTSET
on_demand_loading True
object_name generic
fs0 {
 charset {
  name ISO8859-1:GL
 }
 font {
  primary ISO8859-1:GL
  vertical_rotate all
 }
}
fs1 {
 charset {
  name ISO8859-1:GR
 }
 font {
  primary ISO8859-1:GR
 }
}
fs2 {
 charset {
  name ISO10646-1
 }
 font {
  primary ISO10646-1
 }
}
END XLC_FONTSET
"""


def main():
    path = Path(sys.argv[1])
    original = path.read_text()
    _, conversion = original.split("END XLC_FONTSET", 1)
    if "XLC_XLOCALE" not in conversion:
        raise SystemExit("Missing upstream UTF-8 conversion definition")
    path.write_text(FONTSET.rstrip("\n") + conversion)


if __name__ == "__main__":
    main()
