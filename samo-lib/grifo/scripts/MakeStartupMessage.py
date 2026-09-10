#!/usr/bin/env python3
"""Embed the boot message using the same text.bmf metrics as wiki/glyph.c.

The kernel draws this before mounting the card. Generate only this fixed
line, so boot needs neither a font file read nor the app's font renderer.
"""
from pathlib import Path
import struct
import sys

font = Path(sys.argv[1]).read_bytes()
line_height, _, descent, _, _ = struct.unpack_from('<4bi', font)
message = 'Starting WikiReader...'
width, stride = 240, 32


def glyph(char):
    # See font_bmf_header and charmetric_bmf in wiki/bmf.h.
    offset = 8 + ord(char) * 56
    return struct.unpack_from('<8b48s', font, offset)


advance = sum(glyph(char)[7] for char in message)
if not 0 < advance <= width or not 0 < line_height <= 208 - 94:
    raise ValueError('Boot message does not fit the display')
pixels = bytearray(line_height * stride)
x = (width - advance) // 2
for char in message:
    w, h, row_bytes, row_bits, ascent, _, bearing, step, bitmap = glyph(char)
    if w < 0 or h < 0 or row_bits != row_bytes * 8 or w > row_bits or h * row_bytes > len(bitmap):
        raise ValueError('Invalid BMF glyph')
    top = line_height - descent - ascent
    for gy in range(h):
        for gx in range(w):
            if bitmap[gy * row_bytes + gx // 8] & (0x80 >> (gx % 8)):
                px, py = x + bearing + gx, top + gy
                if not (0 <= px < width and 0 <= py < line_height):
                    raise ValueError('BMF glyph exceeds the boot message bounds')
                pixels[py * stride + px // 8] |= 0x80 >> (px % 8)
    x += step

rows = ['\t' + ', '.join(f'0x{b:02x}' for b in pixels[i:i + 16]) + ','
        for i in range(0, len(pixels), 16)]
Path(sys.argv[2]).write_text(
    '/* Generated from ROOT_IMAGE/text.bmf by MakeStartupMessage.py. */\n'
    'static const unsigned char startup_message[] = {\n' +
    '\n'.join(rows) + '\n};\n')
