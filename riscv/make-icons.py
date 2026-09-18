#!/usr/bin/env python3
"""Generate the launcher icons.

Two 64x64 monochrome XPMs, which the build turns into Grifo .ico files:

  riscv.xpm    "RV32", drawn with the terminal's own 6x9 font scaled up, so
               changing the wording is a one-line edit rather than pixel art
  rvjit.xpm    "JIT", the translator probe -- a developer measurement, on the
               panel only while it is being taken
  rvlinux.xpm  Tux, from the 64x64 bitmap beside this script

Tux is Larry Ewing's (lewing@isc.tamu.edu), made with The GIMP; his terms
ask that this be acknowledged, and it is here and in the README.  The
source image is drivers/video/logo/logo_linux_clut224.ppm from the kernel
tree, with only the black field around him removed.

The output is checked in: the build needs the XPMs, nothing else.  Run
this again only to change an icon.
"""
import argparse
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
SIZE = 64


def font():
    """The 6x9 glyphs the terminal already carries."""
    glyphs, code = {}, 32
    for line in (HERE / 'font6x9.h').read_text().splitlines():
        m = re.match(r'\s*\{ ((?:0x[0-9a-f]{2}, ){8}0x[0-9a-f]{2}) \},', line)
        if m:
            glyphs[code] = [int(v, 16) for v in m.group(1).split(', ')]
            code += 1
    return glyphs


GLYPH_W, GLYPH_H, GAP = 6, 9, 3


def text_icon(top, bottom, scale=None):
    """Two lines of the terminal's font, as large as will fit inside the
    border and centred between them.

    Both the scale and the two baselines are derived rather than written
    down.  They were fixed numbers to begin with, chosen for one label at
    one size, and draw() clips at the edge and overlaps in the middle
    without complaining about either -- so "RV"/"32" at scale 4 drew its
    two lines through each other for as long as it existed, and a
    three-character label lost its last glyph.
    """
    glyphs = font()
    widest = max(len(top), len(bottom))
    if scale is None:
        scale = max(1, min((SIZE - 8) // (GLYPH_W * widest),
                           (SIZE - 8 - GAP) // (2 * GLYPH_H)))
    px = [[0] * SIZE for _ in range(SIZE)]

    def draw(text, at):
        left = (SIZE - len(text) * 6 * scale) // 2
        for n, ch in enumerate(text):
            for r, bits in enumerate(glyphs[ord(ch)]):
                for c in range(6):
                    if bits & (0x80 >> c):
                        for dy in range(scale):
                            for dx in range(scale):
                                y, x = at + r * scale + dy, left + (n * 6 + c) * scale + dx
                                if 0 <= y < SIZE and 0 <= x < SIZE:
                                    px[y][x] = 1

    for i in range(SIZE):
        for edge in (0, 1, SIZE - 2, SIZE - 1):
            px[edge][i] = px[i][edge] = 1
    block = 2 * GLYPH_H * scale + GAP
    draw(top, (SIZE - block) // 2)
    draw(bottom, (SIZE - block) // 2 + GLYPH_H * scale + GAP)
    return px


def tux(pbm):
    """Read the 64x64 Tux bitmap.

    tux_64x64.pbm is derived from Tux_Mono.svg on Wikimedia Commons,
    rendered to 960x1160 and reduced here.  That source matters: it is flat
    vector art, 0.78% mid-tone, where the kernel's own 80x80 logo is
    dithered in its shaded areas -- and a dithered source cannot be reduced
    at all.  Below a 50% threshold its outline comes out heavy, above one
    the dither opens holes through Tux's body, and there is nothing in
    between.

    The reduction fits the art to 62 rows keeping its aspect, takes the
    silhouette at exactly 50% area coverage so the outline stays a single
    pixel, and then rescues thin lines the silhouette drops: a cell whose
    coverage stands above its neighbours' average is a line rather than an
    edge, and the feet and arms are drawn with lines too thin to reach 50%
    on their own.  That is 74 pixels of the 720.

    Already the icon's exact size, so there is nothing to resample and
    nothing to threshold -- which is the whole reason it looks right.  Work
    that went before this tried to reduce the kernel's 80x80 logo: 80 into
    64 puts every feature at the resampler's limit, and no ordering of
    flood, threshold and outline recovered a Tux worth looking at.

    P4 is one bit a pixel, rows padded to whole bytes, most significant bit
    leftmost, 1 black -- the same convention as the Grifo icon it becomes.
    """
    data = pbm.read_bytes()
    if not data.startswith(b'P4'):
        raise SystemExit(f'{pbm}: expected a binary PBM (P4)')
    fields, at = [], 2
    while len(fields) < 2:
        while at < len(data) and data[at:at + 1].isspace():
            at += 1
        if data[at:at + 1] == b'#':
            while data[at:at + 1] not in (b'\n', b''):
                at += 1
            continue
        start = at
        while at < len(data) and not data[at:at + 1].isspace():
            at += 1
        fields.append(int(data[start:at]))
    at += 1
    width, height = fields
    if (width, height) != (SIZE, SIZE):
        raise SystemExit(f'{pbm}: {width}x{height}, expected {SIZE}x{SIZE}')
    stride = (width + 7) // 8
    bits = data[at:]
    return [[1 if bits[y * stride + (x >> 3)] & (0x80 >> (x & 7)) else 0
             for x in range(width)] for y in range(height)]


def write_xpm(path, name, px, note):
    body = [f'"{"".join("#" if v else "." for v in row)}"' for row in px]
    text = (f'/* XPM: {note}\n'
            f'   Generated by make-icons.py; edit that, not this. */\n'
            f'static char *{name}[] = {{\n'
            f'"64 64 2 1",\n". c #FFFFFF",\n"# c #000000",\n'
            + ',\n'.join(body) + '\n};\n')
    path.write_text(text)
    print(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--logo', type=Path, default=HERE / 'tux_64x64.pbm')
    args = parser.parse_args()

    write_xpm(HERE / 'riscv.xpm', 'riscv_icon', text_icon('RV', '32'),
              'launcher icon for the interpreter benchmark.')
    write_xpm(HERE / 'rvjit.xpm', 'rvjit_icon', text_icon('JIT', 'PROB'),
              'launcher icon for the translator probe.')
    write_xpm(HERE / 'rvlinux.xpm', 'rvlinux_icon', tux(args.logo),
              "launcher icon for Linux: Tux, by Larry Ewing\n"
              "   (lewing@isc.tamu.edu) with The GIMP, from tux_64x64.pbm.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
