#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check NXTerm scroll cache contents/order with readable and write-only displays."""

import argparse
from pathlib import Path
import subprocess
import tempfile

STUB = r"""
#pragma once
#include <stdint.h>
#define __GRAPHICS_NXTERM_NXTERM_H
#define FAR
#define gerr(...) ((void)0)
typedef int16_t nxgl_coord_t;
typedef uint8_t nxgl_mxpixel_t;
struct nxgl_point_s { nxgl_coord_t x, y; };
struct nxgl_rect_s { struct nxgl_point_s pt1, pt2; };
struct nxterm_state_s;
struct nxterm_operations_s {
  int (*fill)(struct nxterm_state_s *, const struct nxgl_rect_s *, uint8_t *);
  int (*move)(struct nxterm_state_s *, const struct nxgl_rect_s *,
              const struct nxgl_point_s *);
};
struct nxterm_bitmap_s {
  uint8_t code, flags;
  struct nxgl_point_s pos;
};
struct nxterm_state_s {
  const struct nxterm_operations_s *ops;
  struct { struct { int w, h; } wsize; uint8_t wcolor[1]; } wndo;
  uint8_t fheight;
  uint16_t nchars;
  struct nxgl_point_s fpos;
  struct nxgl_point_s frontier;
  struct nxterm_bitmap_s bm[1024];
};
void nxterm_scroll(struct nxterm_state_s *, int);
int nxterm_fillchar(struct nxterm_state_s *, const struct nxgl_rect_s *,
                    const struct nxterm_bitmap_s *);
"""

TEST = r"""
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "stub.h"
static unsigned seed = 7654321;
static unsigned rnd(void)
{
  seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
  return seed;
}
static struct nxgl_rect_s cleared, moved;
static struct nxgl_point_s offset;
static int moves, fills;
static int fill(struct nxterm_state_s *p, const struct nxgl_rect_s *r,
                 uint8_t *color)
{
  assert(r->pt1.x == 0 && r->pt2.x == 239);
  cleared = *r; fills++;
  return 0;
}
static int move(struct nxterm_state_s *p, const struct nxgl_rect_s *r,
                 const struct nxgl_point_s *o)
{
  moved = *r; offset = *o; moves++;
  return 0;
}
int nxterm_fillchar(struct nxterm_state_s *p, const struct nxgl_rect_s *r,
                    const struct nxterm_bitmap_s *bm)
{
  assert(bm >= p->bm && bm < p->bm + p->nchars);
  return 0;
}
int main(void)
{
  const struct nxterm_operations_s ops = {fill, move};
  for (int trial = 0; trial < 2000; trial++)
    {
      struct nxterm_state_s p = {.ops = &ops, .fheight = 9,
                                 .wndo = {.wsize = {240, 120}},
                                 .fpos = {42, 117}};
      struct nxterm_bitmap_s expected[1024];
      int count = 0;
      int height = 1 + rnd() % 20;

      /* The frontier bounds every retained character; 200 is past all of
       * the positions generated below, and 0 exercises the clamp.
       */

      int frontier0 = trial % 5 == 0 ? 0 : 200;
      p.frontier.x = 7;
      p.frontier.y = frontier0;
      p.nchars = trial % 4 == 0 ? 0 : trial % 4 == 1 ? 1024 : rnd() % 1024;
      for (int i = 0; i < p.nchars; i++)
        {
          /* Mix retained and discarded entries, including unsorted Y
           * positions produced by cursor editing, and exact boundaries.
           */
          int keep = trial % 3 == 0 ? 0 : trial % 3 == 1 ? 1 : rnd() % 2;
          struct nxterm_bitmap_s bm = {rnd(), rnd(), {rnd() % 240, 0}};
          if (keep)
            {
              bm.pos.y = CONFIG_NXTERM_LINESEPARATION + rnd() % 80;
              expected[count++] = bm;
              bm.pos.y += height;
            }
          else
            bm.pos.y = height + CONFIG_NXTERM_LINESEPARATION - 1;
          p.bm[i] = bm;
        }
      moves = fills = 0;
      nxterm_scroll(&p, height);
      assert(p.nchars == count);
      assert(memcmp(p.bm, expected, count * sizeof(expected[0])) == 0);
      assert(p.fpos.x == 42 && p.fpos.y == 117 - height);
      assert(cleared.pt2.y == 119);

      /* The frontier moves with the characters it bounds, stops at the top
       * of the window, and still bounds every one of them afterwards.
       */

      if (frontier0 - height < 0)
        assert(p.frontier.x == 0 && p.frontier.y == 0);
      else
        {
          assert(p.frontier.x == 7 && p.frontier.y == frontier0 - height);
          for (int i = 0; i < p.nchars; i++)
            assert(p.bm[i].pos.y < p.frontier.y);
        }

#ifdef CONFIG_NX_WRITEONLY
      assert(moves == 0 && fills > 1);
      assert(cleared.pt1.y == p.fpos.y);
#else
      assert(moves == 1 && fills == 1);
      assert(moved.pt1.x == 0 && moved.pt2.x == 239);
      assert(moved.pt1.y == height + CONFIG_NXTERM_LINESEPARATION);
      assert(moved.pt2.y == 119 && offset.x == 0);
      assert(offset.y == -moved.pt1.y);
      assert(cleared.pt1.y == 120 + offset.y);
#endif
    }
  puts("PASS: 2000 empty/full/mixed scroll caches, order, positions and display bounds");
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path,
                        default=Path(__file__).resolve().parents[5])
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="wr-nxterm-scroll-") as tmp:
        out = Path(tmp)
        (out / "stub.h").write_text(STUB)
        (out / "test.c").write_text(TEST)
        for name in ("config.h", "debug.h", "nx/nx.h", "nx/nxfonts.h"):
            header = out / "nuttx" / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "stub.h"\n')
        for separation in (0, 3):
            for writeonly in (False, True):
                cmd = [args.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                       "-Wno-unused-parameter", "-fsanitize=address,undefined",
                       "-g", "-O1", f"-I{out}",
                       f"-DCONFIG_NXTERM_LINESEPARATION={separation}"]
                if writeonly:
                    cmd += ["-DCONFIG_NX_WRITEONLY=1"]
                cmd += [str(args.source_root / "graphics/nxterm/nxterm_scroll.c"),
                        str(out / "test.c"), "-o", str(out / "test")]
                subprocess.run(cmd, check=True)
                print(f"Line separation {separation}, write-only {writeonly}:",
                      flush=True)
                subprocess.run([str(out / "test")], check=True)


if __name__ == "__main__":
    main()
