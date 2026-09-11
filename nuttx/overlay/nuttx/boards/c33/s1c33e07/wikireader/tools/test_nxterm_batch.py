#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check batched terminal pixels, clipping, resizing and failed submissions."""

import argparse
from pathlib import Path
import subprocess
import tempfile

STUB = r"""
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define __GRAPHICS_NXTERM_NXTERM_H
#define __GRAPHICS_NXGLIB_NXBLIC_H
#define FAR
#define OK 0
#define ngl_min(a,b) ((a) < (b) ? (a) : (b))
#define ngl_max(a,b) ((a) > (b) ? (a) : (b))
#define CONFIG_NX_NPLANES 1
#define CONFIG_NXTERM_BATCH 1
typedef int16_t nxgl_coord_t;
typedef uint8_t nxgl_mxpixel_t;
struct nxgl_point_s { nxgl_coord_t x, y; };
struct nxgl_size_s { nxgl_coord_t w, h; };
struct nxgl_rect_s { struct nxgl_point_s pt1, pt2; };
struct fb_planeinfo_s { void *fbmem; size_t fblen; unsigned stride; uint8_t bpp; };
struct nxterm_state_s;
struct nxterm_batch_s;
struct nxterm_operations_s {
  int (*fill)(struct nxterm_state_s *, const struct nxgl_rect_s *, uint8_t *);
#ifndef CONFIG_NX_WRITEONLY
  int (*move)(struct nxterm_state_s *, const struct nxgl_rect_s *,
              const struct nxgl_point_s *);
#endif
  int (*bitmap)(struct nxterm_state_s *, const struct nxgl_rect_s *,
                const void **, const struct nxgl_point_s *, unsigned);
};
struct nxterm_state_s {
  const struct nxterm_operations_s *ops;
  struct nxterm_batch_s *batch;
  struct { struct nxgl_size_s wsize; uint8_t wcolor[1]; } wndo;
};
void *kmm_zalloc(size_t);
void kmm_free(void *);
void nxgl_rectintersect(struct nxgl_rect_s *, const struct nxgl_rect_s *,
                        const struct nxgl_rect_s *);
void nxgl_rectunion(struct nxgl_rect_s *, const struct nxgl_rect_s *,
                    const struct nxgl_rect_s *);
void nxgl_rectoffset(struct nxgl_rect_s *, const struct nxgl_rect_s *,
                     nxgl_coord_t, nxgl_coord_t);
bool nxgl_nullrect(const struct nxgl_rect_s *);
void nxgl_fillrectangle_1bpp(struct fb_planeinfo_s *, const struct nxgl_rect_s *, uint8_t);
void nxgl_copyrectangle_1bpp(struct fb_planeinfo_s *, const struct nxgl_rect_s *,
                            const void *, const struct nxgl_point_s *, unsigned);
void nxgl_moverectangle_1bpp(struct fb_planeinfo_s *, const struct nxgl_rect_s *,
                            struct nxgl_point_s *);
int nxterm_batch_resize(struct nxterm_state_s *, const struct nxgl_size_s *);
int nxterm_batch_flush(struct nxterm_state_s *);
void nxterm_batch_free(struct nxterm_state_s *);
"""

TEST = r"""
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "stub.h"
#define W 240
#define H 120
static uint8_t expected[H][W], display[H][W];
#ifndef CONFIG_NX_WRITEONLY
static uint8_t snapshot[H][W];
#endif
static unsigned calls, allocations;
static bool fail_alloc, fail_submit;
static uint32_t seed = 0x33e07;
static unsigned rnd(void)
{
  seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
  return seed;
}
void *kmm_zalloc(size_t size)
{
  if (fail_alloc) return NULL;
  void *p = calloc(1, size);
  assert(p); allocations++;
  return p;
}
void kmm_free(void *p)
{
  if (p) { assert(allocations); allocations--; free(p); }
}
static unsigned pixel(const uint8_t *p, unsigned stride, int x, int y)
{
  unsigned shift = MSFIRST ? 7 - (x & 7) : x & 7;
  return (p[y * stride + x / 8] >> shift) & 1;
}
static int submit(struct nxterm_state_s *p, const struct nxgl_rect_s *r,
                   const void **src, const struct nxgl_point_s *o, unsigned stride)
{
  calls++;
  if (fail_submit) return -EIO;
  assert(r->pt1.x >= 0 && r->pt2.x < p->wndo.wsize.w);
  assert(r->pt1.y >= 0 && r->pt2.y < p->wndo.wsize.h);
  for (int y = r->pt1.y; y <= r->pt2.y; y++)
    for (int x = r->pt1.x; x <= r->pt2.x; x++)
      display[y][x] = pixel(src[0], stride, x - o->x, y - o->y);
  return 0;
}
static void check(struct nxterm_state_s *p)
{
  assert(nxterm_batch_flush(p) == 0);
  for (int y = 0; y < p->wndo.wsize.h; y++)
    for (int x = 0; x < p->wndo.wsize.w; x++)
      if (display[y][x] != expected[y][x]) {
        fprintf(stderr, "pixel mismatch at %d,%d seed=%u\n", x, y, seed);
        abort();
      }
  unsigned count = calls;
  assert(nxterm_batch_flush(p) == 0 && calls == count);
}
static void resize(struct nxterm_state_s *p, int w, int h)
{
  struct nxgl_size_s size = {w, h};
  int oldw = p->wndo.wsize.w, oldh = p->wndo.wsize.h;
  assert(nxterm_batch_resize(p, &size) == 0);
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      if (x >= oldw || y >= oldh) expected[y][x] = p->wndo.wcolor[0] & 1;
  p->wndo.wsize = size;
  check(p);
}
int main(void)
{
  const struct nxterm_operations_s original = {.bitmap = submit};
  struct nxterm_state_s p = {.ops = &original};
  struct nxgl_size_s size = {W, H};
  fail_alloc = true;
  assert(nxterm_batch_resize(&p, &size) == -ENOMEM);
  assert(p.ops == &original && p.batch == NULL && allocations == 0);
  fail_alloc = false;

  for (unsigned background = 0; background < 2; background++) {
    p.wndo.wcolor[0] = background;
    p.wndo.wsize.w = p.wndo.wsize.h = 0;
    resize(&p, W, H);
    for (int trial = 0; trial < 4000; trial++) {
      int w = p.wndo.wsize.w, h = p.wndo.wsize.h;
      unsigned start_calls = calls;
      for (int op = 0; op < 12; op++) {
        int x = (int)(rnd() % (w + 20)) - 10;
        int y = (int)(rnd() % (h + 20)) - 10;
        int rw = 1 + rnd() % w, rh = 1 + rnd() % h;
        struct nxgl_rect_s r = {{x, y}, {x + rw - 1, y + rh - 1}};
        unsigned kind = rnd() % 3;
#ifdef CONFIG_NX_WRITEONLY
        if (kind == 2) kind = 0;
#endif
        if (kind == 0) {
          uint8_t color = rnd() & 1;
          assert(p.ops->fill(&p, &r, &color) == 0);
          for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < w; xx++)
              if (xx >= x && xx < x + rw && yy >= y && yy < y + rh)
                expected[yy][xx] = color;
        } else if (kind == 1) {
          unsigned stride = (rw + 7) / 8;
          uint8_t *glyph = malloc(stride * rh);
          for (unsigned i = 0; i < stride * rh; i++) glyph[i] = rnd();
          const void *src[] = {glyph};
          assert(p.ops->bitmap(&p, &r, src, &r.pt1, stride) == 0);
          for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < w; xx++)
              if (xx >= x && xx < x + rw && yy >= y && yy < y + rh)
                expected[yy][xx] = pixel(glyph, stride, xx - x, yy - y);
          /* Flush occurs after freeing every source glyph. */
          free(glyph);
        }
#ifndef CONFIG_NX_WRITEONLY
        else {
          struct nxgl_point_s delta = {(int)(rnd() % (2*w)) - w,
                                       (int)(rnd() % (2*h)) - h};
          memcpy(snapshot, expected, sizeof snapshot);
          assert(p.ops->move(&p, &r, &delta) == 0);
          for (int yy = 0; yy < h; yy++)
            for (int xx = 0; xx < w; xx++) {
              int sx = xx - delta.x, sy = yy - delta.y;
              if (sx >= 0 && sx < w && sy >= 0 && sy < h &&
                  sx >= x && sx < x + rw && sy >= y && sy < y + rh)
                expected[yy][xx] = snapshot[sy][sx];
            }
        }
#endif
      }
      assert(calls == start_calls); /* No per-glyph server calls. */
      if (trial % 31 == 0) {
        fail_submit = true;
        int ret = nxterm_batch_flush(&p);
        assert(ret == -EIO || ret == 0); /* Fully clipped batches can be empty. */
        fail_submit = false;
        start_calls = calls;
      }
      check(&p);
      assert(calls <= start_calls + 1);
      if (trial % 47 == 0) {
        struct nxterm_batch_s *old = p.batch;
        struct nxgl_size_s changed = {w == W ? 7 : W, h == H ? 9 : H};
        fail_alloc = true;
        assert(nxterm_batch_resize(&p, &changed) == -ENOMEM);
        assert(p.batch == old);
        fail_alloc = false;
        check(&p);
        resize(&p, changed.w, changed.h);
      }
    }
    struct nxgl_size_s invalid = {0, H};
    assert(nxterm_batch_resize(&p, &invalid) == -EINVAL);
    check(&p);
    nxterm_batch_free(&p);
    assert(p.batch == NULL && p.ops == &original && allocations == 0);
    nxterm_batch_free(&p);
  }
  puts("PASS: 96000 buffered draws; pixels, clipping, overlap, resize, failures and source lifetime");
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[5]
    with tempfile.TemporaryDirectory(prefix="wr-nxterm-batch-") as tmp:
        out = Path(tmp)
        (out / "stub.h").write_text(STUB)
        (out / "test.c").write_text(TEST)
        for name in ("config.h", "kmalloc.h", "video/fb.h", "nx/nxglib.h"):
            header = out / "nuttx" / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "stub.h"\n')
        for msfirst in (0, 1):
            for writeonly in (False, True):
                cmd = [args.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                       "-Wno-unused-parameter", "-fsanitize=address,undefined",
                       "-g", "-O1", f"-I{out}",
                       f"-I{root / 'graphics/nxglib'}", "-DNXGLIB_BITSPERPIXEL=1",
                       "-DNXGLIB_SUFFIX=_1bpp", f"-DMSFIRST={msfirst}"]
                if msfirst:
                    cmd += ["-DCONFIG_NX_PACKEDMSFIRST=1"]
                if writeonly:
                    cmd += ["-DCONFIG_NX_WRITEONLY=1"]
                cmd += [str(root / "graphics/nxterm/nxterm_batch.c")]
                cmd += [str(root / f"graphics/nxglib/fb/nxglib_{op}rectangle.c")
                        for op in ("fill", "copy", "move")]
                cmd += [str(root / f"libs/libnx/nxglib/nxglib_{op}.c")
                        for op in ("rectintersect", "rectunion", "rectoffset", "nullrect")]
                cmd += [str(out / "test.c"), "-o", str(out / "test")]
                subprocess.run(cmd, check=True)
                print(f"MSB first {msfirst}, write-only {writeonly}:", flush=True)
                subprocess.run([str(out / "test")], check=True)


if __name__ == "__main__":
    main()
