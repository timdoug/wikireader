/****************************************************************************
 * apps/system/bench/ubench_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/* What the standard benchmarks cannot say.
 *
 * The device runs CoreMark about five times slower than the emulator models
 * it, and one number cannot say why. These loops each do one thing, so the
 * time they take divided by the work they did is the price of that thing:
 * an instruction fetch, a load, a store.
 *
 * The one that matters most is the first two together. There is no
 * instruction cache on this part, so code in SDRAM is fetched from SDRAM on
 * every pass round the loop, while code in internal RAM is not. The same
 * instructions run from both places, and the difference between the two
 * times is what fetching from SDRAM costs -- which is the number the
 * emulator's timing model is missing, since it charges about a cycle per
 * instruction wherever the program happens to be.
 */

#include <nuttx/config.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>

#include "bench_card.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Internal RAM above the framebuffer. The panel is 240x208 at one bit, on a
 * 32-byte stride, so the display owns 0x80000 through 0x819ff and the rest
 * of the 12 KB is free. Nothing else in this image goes near it.
 */

#define UB_IVRAM_BASE  0x00082000
#define UB_IVRAM_SIZE  0x00001000

/* Enough passes that the ten millisecond clock has something to measure --
 * a few seconds on the device, which is the slower of the two machines and
 * the one that matters.  The long loop does ten times the work per pass, so
 * it gets a tenth of the passes.
 */

#define UB_PASSES      4000000

/* The walking loops cover sixteen bytes a pass, so a megabyte of buffer is
 * 65536 of them -- far enough to cross a thousand pages and to leave no row
 * usefully open.
 */

#define UB_STREAM_BYTES  (1024 * 1024)
#define UB_STREAM_PASSES (UB_STREAM_BYTES / 16)
#define UB_LONG_PASSES 400000

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef void (*ub_fn_t)(unsigned long passes, volatile void *buffer);

struct ub_case_s
{
  FAR const char *name;
  ub_fn_t fn;
  FAR void *end;           /* so the loop can report its own size */
  unsigned long passes;
  unsigned instructions;   /* per pass, counted from ubench.S */
  bool internal;           /* run it from internal RAM */
  bool stream;             /* walks through the buffer rather than sitting */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

extern void ub_block_start(unsigned long passes, volatile void *buffer);
extern void ub_block_end(unsigned long passes, volatile void *buffer);
extern void ub_alu(unsigned long passes, volatile void *buffer);
extern void ub_alu_end(void);
extern void ub_alu_off(unsigned long passes, volatile void *buffer);
extern void ub_alu_off_end(void);
extern void ub_wide(unsigned long passes, volatile void *buffer);
extern void ub_wide_end(void);
extern void ub_load(unsigned long passes, volatile void *buffer);
extern void ub_load_end(void);
extern void ub_store(unsigned long passes, volatile void *buffer);
extern void ub_store_end(void);
extern void ub_s4(unsigned long passes, volatile void *buffer);
extern void ub_s4_end(void);
extern void ub_s16(unsigned long passes, volatile void *buffer);
extern void ub_s16_end(void);
extern void ub_s32(unsigned long passes, volatile void *buffer);
extern void ub_s32_end(void);
extern void ub_loadb(unsigned long passes, volatile void *buffer);
extern void ub_loadb_end(void);
extern void ub_storeb(unsigned long passes, volatile void *buffer);
extern void ub_storeb_end(void);
extern void ub_loadseq(unsigned long passes, volatile void *buffer);
extern void ub_loadseq_end(void);
extern void ub_storeseq(unsigned long passes, volatile void *buffer);
extern void ub_storeseq_end(void);
extern void ub_straight(unsigned long passes, volatile void *buffer);
extern void ub_straight_end(void);

static uint32_t g_scratch[8];
static FAR uint8_t *g_stream;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* The same routine, at the address it was linked for or at its copy in
 * internal RAM.
 */

static ub_fn_t ub_relocate(ub_fn_t fn, bool internal)
{
  uintptr_t offset;

  if (!internal)
    {
      return fn;
    }

  offset = (uintptr_t)fn - (uintptr_t)ub_block_start;
  return (ub_fn_t)(UB_IVRAM_BASE + offset);
}

static double ub_run(const struct ub_case_s *test)
{
  struct timespec start;
  struct timespec end;
  ub_fn_t fn = ub_relocate(test->fn, test->internal);

  clock_gettime(CLOCK_MONOTONIC, &start);
  fn(test->passes, test->stream ? (FAR void *)g_stream : g_scratch);
  clock_gettime(CLOCK_MONOTONIC, &end);

  return (end.tv_sec - start.tv_sec) +
         (end.tv_nsec - start.tv_nsec) / 1000000000.0;
}

/* Where the boundary is.
 *
 * These numbers moved by a factor of four when an unrelated source file
 * shifted the loops, and by nothing at all when the loop was deliberately
 * offset by sixteen bytes. So something larger than sixteen bytes is
 * expensive to cross and a 26-byte loop usually does not. The way to find
 * it is to put the same loop where it must cross a given boundary and see
 * which one costs: each offset below is sixteen bytes short of a power of
 * two, so the loop straddles it, and 0 is the control that straddles
 * nothing.
 */

static const unsigned g_offsets[] =
{
  0, 16, 48, 112, 240, 496, 1008, 2032
};

#define UB_ARENA_ALIGN 4096
#define UB_ARENA_SIZE  8192

static void ub_boundary(int fd, size_t span)
{
  FAR uint8_t *arena = memalign(UB_ARENA_ALIGN, UB_ARENA_SIZE);
  int i;

  if (arena == NULL)
    {
      dprintf(fd, "# no room for the boundary sweep\n");
      return;
    }

  for (i = 0; i < (int)(sizeof(g_offsets) / sizeof(g_offsets[0])); i++)
    {
      unsigned off = g_offsets[i];
      FAR uint8_t *at = arena + off;
      struct timespec start;
      struct timespec end;
      double seconds;

      memcpy(at, (FAR const void *)ub_alu, span);

      clock_gettime(CLOCK_MONOTONIC, &start);
      ((ub_fn_t)at)(UB_PASSES, g_scratch);
      clock_gettime(CLOCK_MONOTONIC, &end);

      seconds = (end.tv_sec - start.tv_sec) +
                (end.tv_nsec - start.tv_nsec) / 1000000000.0;

      dprintf(fd, "UBO offset %5u crosses %5u  %9.3f %10.2f\n", off,
              off ? off + 16 : 0, seconds,
              seconds * CONFIG_S1C33E07_MCLK / UB_PASSES);
    }

  free(arena);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  static const struct ub_case_s cases[] =
  {
    { "alu   sdram", ub_alu,      ub_alu_end,      UB_PASSES,      11, false , false },
    { "alu   ivram", ub_alu,      ub_alu_end,      UB_PASSES,      11, true  , false },
    { "alu+16 sdram", ub_alu_off, ub_alu_off_end,  UB_PASSES,      11, false , false },
    { "wide  sdram", ub_wide,     ub_wide_end,     UB_PASSES,      11, false , false },
    { "wide  ivram", ub_wide,     ub_wide_end,     UB_PASSES,      11, true  , false },
    { "load  sdram", ub_load,     ub_load_end,     UB_PASSES,      11, false , false },
    { "load  ivram", ub_load,     ub_load_end,     UB_PASSES,      11, true  , false },
    { "store sdram", ub_store,    ub_store_end,    UB_PASSES,      11, false , false },
    { "store ivram", ub_store,    ub_store_end,    UB_PASSES,      11, true  , false },
    { "size 4     ", ub_s4, ub_s4_end, UB_PASSES, 7, false , false },
    { "size 16    ", ub_s16, ub_s16_end, UB_PASSES, 19, false , false },
    { "size 32    ", ub_s32, ub_s32_end, UB_LONG_PASSES, 35, false , false },
    { "loadb sdram", ub_loadb,   ub_loadb_end,   UB_PASSES, 11, false, false },
    { "storeb sdrm", ub_storeb,  ub_storeb_end,  UB_PASSES, 11, false, false },
    { "loadseq    ", ub_loadseq,  ub_loadseq_end,  UB_STREAM_PASSES, 11, false, true },
    { "storeseq   ", ub_storeseq, ub_storeseq_end, UB_STREAM_PASSES, 11, false, true },
    { "long  sdram", ub_straight, ub_straight_end, UB_LONG_PASSES, 67, false , false },
    { "long  ivram", ub_straight, ub_straight_end, UB_LONG_PASSES, 67, true  , false },
  };

  size_t span = (uintptr_t)ub_block_end - (uintptr_t)ub_block_start;

  /* Standard output is where this goes when something else is collecting
   * it -- bench redirects it into the file with everything else. Run on
   * its own from the prompt it has a terminal instead, which nobody can
   * read a table off, so it keeps its own file and unmounts the card.
   */

  FAR const char *path = argc > 1 ? argv[1] :
                         CONFIG_SYSTEM_BENCH_MOUNT "/ubench.txt";
  bool standalone = isatty(STDOUT_FILENO);
  int fd = standalone ? bench_card_open(path) : STDOUT_FILENO;
  int i;

  if (span > UB_IVRAM_SIZE)
    {
      printf("ubench: %zu bytes will not fit in internal RAM\n", span);
      return EXIT_FAILURE;
    }

  memcpy((void *)UB_IVRAM_BASE, (const void *)ub_block_start, span);

  g_stream = malloc(UB_STREAM_BYTES);
  if (g_stream == NULL)
    {
      printf("ubench: no room for the streaming buffer\n");
      return EXIT_FAILURE;
    }

  dprintf(fd, "# microbenchmark, %u MHz, %u passes\n",
          (unsigned)(CONFIG_S1C33E07_MCLK / 1000000), UB_PASSES);
  dprintf(fd, "# sdram: tRP %u, tRAS %u, tRC %u, refresh 0x%x\n",
          (unsigned)(((*(FAR volatile uint32_t *)0x00301604) >> 12) & 3) + 1,
          (unsigned)(((*(FAR volatile uint32_t *)0x00301604) >> 8) & 7) + 1,
          (unsigned)(((*(FAR volatile uint32_t *)0x00301604) >> 4) & 15) + 1,
          (unsigned)((*(FAR volatile uint32_t *)0x00301608) & 0xfff));
  dprintf(fd, "# %-12s %6s %5s %9s %10s %10s\n", "loop", "bytes", "insn",
          "seconds", "cyc/pass", "cyc/instr");

  for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++)
    {
      double seconds = ub_run(&cases[i]);
      double cycles = seconds * CONFIG_S1C33E07_MCLK / cases[i].passes;
      size_t bytes = (uintptr_t)cases[i].end - (uintptr_t)cases[i].fn;

      /* The byte count is the whole routine including its entry and return;
       * the loop body is what repeats, and is four bytes less.
       */

      dprintf(fd, "UB %-12s %6zu %5u %9.3f %10.2f %10.2f\n", cases[i].name,
              bytes, cases[i].instructions, seconds, cycles,
              cycles / cases[i].instructions);
    }

  dprintf(fd, "# the same 26-byte loop, placed to straddle each boundary\n");
  ub_boundary(fd, (uintptr_t)ub_alu_end - (uintptr_t)ub_alu);

  if (standalone)
    {
      bench_card_close(fd, path);
    }

  return EXIT_SUCCESS;
}
