#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare NX packed framebuffer operations with a pixel oracle on the host."""

import argparse
from pathlib import Path
import subprocess
import tempfile

HEADER = r"""
#include <stdint.h>
#include <string.h>
#define FAR
struct nxgl_point_s { int16_t x, y; };
struct nxgl_rect_s { struct nxgl_point_s pt1, pt2; };
struct fb_planeinfo_s { void *fbmem; unsigned int stride; };
"""

TEST = r"""
#include <stdio.h>
#include <stdlib.h>
#include "types.h"
#define W 64
#define H 12
#define STRIDE (W * BPP / 8 + 3)
#define GUARD 32
#define LEN (STRIDE * H + 2 * GUARD)
void nxgl_fillrectangle_test(struct fb_planeinfo_s *,
                            const struct nxgl_rect_s *, uint8_t);
void nxgl_copyrectangle_test(struct fb_planeinfo_s *,
                            const struct nxgl_rect_s *, const void *,
                            const struct nxgl_point_s *, unsigned int);
void nxgl_moverectangle_test(struct fb_planeinfo_s *,
                            const struct nxgl_rect_s *, struct nxgl_point_s *);
static uint32_t seed = 12345;
static unsigned rnd(void)
{
  seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
  return seed;
}
static unsigned pixel(const uint8_t *data, int stride, int x, int y)
{
  unsigned value = 0;
  for (int bit = 0; bit < BPP; bit++)
    {
      int index = x * BPP + bit;
      int shift = MSFIRST ? 7 - index % 8 : index % 8;
      int weight = MSFIRST ? BPP - 1 - bit : bit;
      value |= ((data[y * stride + index / 8] >> shift) & 1) << weight;
    }
  return value;
}
static void setpixel(uint8_t *data, int x, int y, unsigned value)
{
  for (int bit = 0; bit < BPP; bit++)
    {
      int index = x * BPP + bit;
      int shift = MSFIRST ? 7 - index % 8 : index % 8;
      int weight = MSFIRST ? BPP - 1 - bit : bit;
      uint8_t *byte = data + y * STRIDE + index / 8;
      *byte = (*byte & ~(1 << shift)) | (((value >> weight) & 1) << shift);
    }
}
int main(void)
{
  uint8_t actual[LEN], expected[LEN], source[LEN];
  struct fb_planeinfo_s plane = {actual + GUARD, STRIDE};

  /* Tight glyph allocations catch reads beyond the final source byte,
   * including differently aligned source/destination and partial bytes.
   */

  for (int sx = 0; sx < 8 / BPP; sx++)
    for (int dx = 0; dx < 8 / BPP; dx++)
      for (int width = 1; width <= 16 / BPP; width++)
        {
          unsigned bytes = ((sx + width) * BPP + 7) / 8;
          uint8_t *glyph = malloc(bytes);
          struct nxgl_rect_s rect = {{dx, 0}, {dx + width - 1, 0}};
          struct nxgl_point_s origin = {dx - sx, 0};
          for (unsigned i = 0; i < bytes; i++) glyph[i] = rnd();
          for (int i = 0; i < LEN; i++) actual[i] = rnd();
          memcpy(expected, actual, LEN);
          for (int i = 0; i < width; i++)
            setpixel(expected + GUARD, dx + i, 0,
                     pixel(glyph, bytes, sx + i, 0));
          nxgl_copyrectangle_test(&plane, &rect, glyph, &origin, bytes);
          free(glyph);
          if (memcmp(actual, expected, LEN))
            {
              fprintf(stderr, "FAIL tight glyph bpp=%d sx=%d dx=%d width=%d\n",
                      BPP, sx, dx, width);
              return 1;
            }
        }

  /* The terminal moves 240 visible pixels within a 256-pixel stride.
   * Check both scroll directions, retaining padding and keyboard rows.
   */

  enum { SCREEN_STRIDE = 256 * BPP / 8, SCREEN_LEN = SCREEN_STRIDE * 208 };
  uint8_t screen[SCREEN_LEN + 2 * GUARD], snapshot[sizeof(screen)];
  struct fb_planeinfo_s display = {screen + GUARD, SCREEN_STRIDE};
  for (int down = 0; down < 2; down++)
    {
      struct nxgl_rect_s rect = {{0, down ? 0 : 9},
                                {239, down ? 110 : 119}};
      struct nxgl_point_s dest = {0, down ? 9 : 0};
      for (unsigned i = 0; i < sizeof(screen); i++) screen[i] = rnd();
      memcpy(snapshot, screen, sizeof(screen));
      nxgl_moverectangle_test(&display, &rect, &dest);
      for (unsigned i = 0; i < sizeof(screen); i++)
        {
          unsigned index = i;
          int y = ((int)i - GUARD) / SCREEN_STRIDE;
          int x = ((int)i - GUARD) % SCREEN_STRIDE;
          if (i >= GUARD && i < GUARD + SCREEN_LEN &&
              y >= dest.y && y < dest.y + 111 && x < 240 * BPP / 8)
            index = i + (down ? -9 : 9) * SCREEN_STRIDE;
          if (screen[i] != snapshot[index])
            {
              fprintf(stderr, "FAIL full-width scroll bpp=%d down=%d at %u\n",
                      BPP, down, i);
              return 1;
            }
        }
    }

  for (int trial = 0; trial < 30000; trial++)
    {
      int width = 1 + rnd() % W;
      int height = 1 + rnd() % H;
      int x = rnd() % (W - width + 1);
      int y = rnd() % (H - height + 1);
      int dx = rnd() % (W - width + 1);
      int dy = rnd() % (H - height + 1);
      struct nxgl_rect_s rect = {{x, y}, {x + width - 1, y + height - 1}};
      struct nxgl_point_s origin = {x - dx, y - dy};
      struct nxgl_point_s dest = {dx, dy};
      for (int i = 0; i < LEN; i++) actual[i] = rnd();
      memcpy(expected, actual, LEN);
      memcpy(source, actual, LEN);
      unsigned color = rnd() % (1 << BPP);
      int op = trial % 3;
      for (int j = 0; j < height; j++)
        for (int i = 0; i < width; i++)
          {
            if (op == 0) setpixel(expected + GUARD, x+i, y+j, color);
            if (op == 1) setpixel(expected + GUARD, x+i, y+j,
              pixel(source + GUARD, STRIDE - 3, dx+i, dy+j));
            if (op == 2) setpixel(expected + GUARD, dx+i, dy+j,
              pixel(source + GUARD, STRIDE, x+i, y+j));
          }
      if (op == 0) nxgl_fillrectangle_test(&plane, &rect, color);
      if (op == 1) nxgl_copyrectangle_test(&plane, &rect, source + GUARD,
                                          &origin, STRIDE - 3);
      if (op == 2) nxgl_moverectangle_test(&plane, &rect, &dest);
      if (memcmp(actual, expected, LEN))
        {
          fprintf(stderr, "FAIL bpp=%d msfirst=%d trial=%d op=%d "
                  "rect=%d,%d %dx%d dest=%d,%d\n",
                  BPP, MSFIRST, trial, op, x, y, width, height, dx, dy);
          return 1;
        }
    }
  printf("PASS: %d bpp, %s first, 30000 guarded fill/copy/move cases\n",
         BPP, MSFIRST ? "MSB" : "LSB");
  return 0;
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path,
                        default=Path(__file__).resolve().parents[5])
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    source = args.source_root / "graphics/nxglib"
    with tempfile.TemporaryDirectory(prefix="wr-packed-") as tmp:
        out = Path(tmp)
        for name in ("nuttx/config.h", "nuttx/video/fb.h", "nuttx/nx/nxglib.h"):
            header = out / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "types.h"\n')
        (out / "types.h").write_text("#pragma once\n" + HEADER)
        (out / "test.c").write_text(TEST)
        for bpp in (1, 2, 4):
            for msfirst in (0, 1):
                cmd = [args.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                       "-fsanitize=address,undefined", "-g", "-O1",
                       f"-I{out}", f"-I{source}", f"-DBPP={bpp}",
                       f"-DNXGLIB_BITSPERPIXEL={bpp}", f"-DMSFIRST={msfirst}",
                       "-DNXGLIB_SUFFIX=_test"]
                if msfirst:
                    cmd += ["-DCONFIG_NX_PACKEDMSFIRST=1"]
                cmd += [str(source / "fb" / f"nxglib_{op}rectangle.c")
                        for op in ("fill", "copy", "move")]
                cmd += [str(out / "test.c"), "-o", str(out / "test")]
                subprocess.run(cmd, check=True)
                subprocess.run([str(out / "test")], check=True)


if __name__ == "__main__":
    main()
