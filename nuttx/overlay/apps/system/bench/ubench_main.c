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

/* One SDRAM bank: col_bits 9 + row_bits 12 + one for the halfword, so the
   bank bits start at 4 MB and this pins every stream's bank and row. */
#define UB_STREAM_ALIGN  (4 * 1024 * 1024)
#define UB_STREAM_PASSES (UB_STREAM_BYTES / 16)

/* One walk of a megabyte is 65536 passes and takes about twenty
 * milliseconds, which a ten millisecond clock measures to two ticks: the
 * answers came out as multiples of 9.155 cycles a pass and moved by that
 * much when nothing had changed. Walk it enough times to be worth timing.
 */

#define UB_STREAM_REPEAT 32
/* Sixteen bytes a pass through the first half, writing to the second. */
#define UB_COPY_PASSES   (UB_STREAM_BYTES / 2 / 16)
/* ...and four bytes a pass for the loop that copies a word at a time. */
#define UB_WORD_PASSES   (UB_STREAM_BYTES / 2 / 4)
/* ...and thirty-two for the one that reads eight words. */
#define UB_WIDE_PASSES   (UB_STREAM_BYTES / 32)
/* Thirty-two bytes a pass through the first half, and eight for the byte
 * copy, which would take all day over a megabyte otherwise. */
#define UB_COPY32_PASSES (UB_STREAM_BYTES / 2 / 32)
#define UB_BYTE_PASSES   (UB_STREAM_BYTES / 2 / 8)
/* ...and one for the padded loops, so they cover the same ground. */
#define UB_PAD_PASSES    (UB_STREAM_BYTES / 2)
/* ...and a quarter of the buffer each for the four-pointer loop. */
#define UB_ACC_PASSES    (UB_STREAM_BYTES / 8)

/* Far enough apart to be in another bank: four megabytes, plus the half a
 * megabyte each stream walks. Skipped if there is no room for it.
 */

#define UB_FAR_BYTES     (5 * 1024 * 1024)
#define UB_LONG_PASSES 400000

/* A 2 KB body at 400,000 passes is a billion instructions; the fetch path it
   measures does not need that many to settle. */
#define UB_FETCH_PASSES 100000

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
  bool far;                /* wants the buffer with four megabytes in it */
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
extern void ub_copyb(unsigned long passes, volatile void *buffer);
extern void ub_copyb_end(void);
extern void ub_copyfar(unsigned long passes, volatile void *buffer);
extern void ub_copyfar_end(void);
extern void ub_straight(unsigned long passes, volatile void *buffer);
extern void ub_straight_end(void);
extern void ub_ext2(unsigned long passes, volatile void *buffer);
extern void ub_ext2_end(void);
extern void ub_rowthrash(unsigned long passes, volatile void *buffer);
extern void ub_rowthrash_end(void);
extern void ub_bankpair(unsigned long passes, volatile void *buffer);
extern void ub_bankpair_end(void);
extern void ub_copyloop(unsigned long passes, volatile void *buffer);
extern void ub_copyloop_end(void);
extern void ub_lddisp(unsigned long passes, volatile void *buffer);
extern void ub_lddisp_end(void);
extern void ub_mult(unsigned long passes, volatile void *buffer);
extern void ub_mult_end(void);
extern void ub_callret(unsigned long passes, volatile void *buffer);
extern void ub_callret_end(void);
extern void ub_pushpop(unsigned long passes, volatile void *buffer);
extern void ub_pushpop_end(void);
extern void ub_mmio(unsigned long passes, volatile void *buffer);
extern void ub_mmio_end(void);
extern void ub_loadseq8(unsigned long passes, volatile void *buffer);
extern void ub_loadseq8_end(void);
extern void ub_copydisp(unsigned long passes, volatile void *buffer);
extern void ub_copydisp_end(void);
extern void ub_bytecopy(unsigned long passes, volatile void *buffer);
extern void ub_bytecopy_end(void);
extern void ub_bcskew1(unsigned long passes, volatile void *buffer);
extern void ub_bcskew1_end(void);
extern void ub_bcskew2(unsigned long passes, volatile void *buffer);
extern void ub_bcskew2_end(void);
extern void ub_bcskew3(unsigned long passes, volatile void *buffer);
extern void ub_bcskew3_end(void);
extern void ub_ld2(unsigned long passes, volatile void *buffer);
extern void ub_ld2_end(void);
extern void ub_ld2skew(unsigned long passes, volatile void *buffer);
extern void ub_ld2skew_end(void);
extern void ub_st2(unsigned long passes, volatile void *buffer);
extern void ub_st2_end(void);
extern void ub_st2skew(unsigned long passes, volatile void *buffer);
extern void ub_st2skew_end(void);
extern void ub_ld2bf(unsigned long passes, volatile void *buffer);
extern void ub_ld2bf_end(void);
extern void ub_ld2w(unsigned long passes, volatile void *buffer);
extern void ub_ld2w_end(void);
extern void ub_mix16(unsigned long passes, volatile void *buffer);
extern void ub_mix16_end(void);
extern void ub_mix64(unsigned long passes, volatile void *buffer);
extern void ub_mix64_end(void);
extern void ub_mix96(unsigned long passes, volatile void *buffer);
extern void ub_mix96_end(void);
extern void ub_bcpad0(unsigned long passes, volatile void *buffer);
extern void ub_bcpad0_end(void);
extern void ub_bcpad4(unsigned long passes, volatile void *buffer);
extern void ub_bcpad4_end(void);
extern void ub_bcpad8(unsigned long passes, volatile void *buffer);
extern void ub_bcpad8_end(void);
extern void ub_bcwide4(unsigned long passes, volatile void *buffer);
extern void ub_bcwide4_end(void);
extern void ub_bcacc1(unsigned long passes, volatile void *buffer);
extern void ub_bcacc1_end(void);
extern void ub_bcacc2(unsigned long passes, volatile void *buffer);
extern void ub_bcacc2_end(void);
extern void ub_bcacc4(unsigned long passes, volatile void *buffer);
extern void ub_bcacc4_end(void);
extern void ub_bcal0(unsigned long passes, volatile void *buffer);
extern void ub_bcal0_end(void);
extern void ub_bcal2(unsigned long passes, volatile void *buffer);
extern void ub_bcal2_end(void);
extern void ub_bcal4(unsigned long passes, volatile void *buffer);
extern void ub_bcal4_end(void);
extern void ub_bcal6(unsigned long passes, volatile void *buffer);
extern void ub_bcal6_end(void);
extern void ub_bcal8(unsigned long passes, volatile void *buffer);
extern void ub_bcal8_end(void);
extern void ub_bcal10(unsigned long passes, volatile void *buffer);
extern void ub_bcal10_end(void);
extern void ub_bcal12(unsigned long passes, volatile void *buffer);
extern void ub_bcal12_end(void);
extern void ub_bcal14(unsigned long passes, volatile void *buffer);
extern void ub_bcal14_end(void);
extern void ub_bcs10o0(unsigned long passes, volatile void *buffer);
extern void ub_bcs10o0_end(void);
extern void ub_bcs10o4(unsigned long passes, volatile void *buffer);
extern void ub_bcs10o4_end(void);
extern void ub_bcs10o8(unsigned long passes, volatile void *buffer);
extern void ub_bcs10o8_end(void);
extern void ub_bcs10o12(unsigned long passes, volatile void *buffer);
extern void ub_bcs10o12_end(void);
extern void ub_bcs18o0(unsigned long passes, volatile void *buffer);
extern void ub_bcs18o0_end(void);
extern void ub_bcs18o4(unsigned long passes, volatile void *buffer);
extern void ub_bcs18o4_end(void);
extern void ub_bcs18o8(unsigned long passes, volatile void *buffer);
extern void ub_bcs18o8_end(void);
extern void ub_bcs18o12(unsigned long passes, volatile void *buffer);
extern void ub_bcs18o12_end(void);
extern void ub_bcs26o0(unsigned long passes, volatile void *buffer);
extern void ub_bcs26o0_end(void);
extern void ub_bcs26o4(unsigned long passes, volatile void *buffer);
extern void ub_bcs26o4_end(void);
extern void ub_bcs26o8(unsigned long passes, volatile void *buffer);
extern void ub_bcs26o8_end(void);
extern void ub_bcs26o12(unsigned long passes, volatile void *buffer);
extern void ub_bcs26o12_end(void);
extern void ub_bcs34o0(unsigned long passes, volatile void *buffer);
extern void ub_bcs34o0_end(void);
extern void ub_bcs34o4(unsigned long passes, volatile void *buffer);
extern void ub_bcs34o4_end(void);
extern void ub_bcs34o8(unsigned long passes, volatile void *buffer);
extern void ub_bcs34o8_end(void);
extern void ub_bcs34o12(unsigned long passes, volatile void *buffer);
extern void ub_bcs34o12_end(void);
extern void ub_f128(unsigned long passes, volatile void *buffer);
extern void ub_f128_end(void);
extern void ub_f256(unsigned long passes, volatile void *buffer);
extern void ub_f256_end(void);
extern void ub_br32(unsigned long passes, volatile void *buffer);
extern void ub_br32_end(void);
extern void ub_ld2fix(unsigned long passes, volatile void *buffer);
extern void ub_ld2fix_end(void);
extern void ub_ld1walk(unsigned long passes, volatile void *buffer);
extern void ub_ld1walk_end(void);
extern void ub_rowrate2(unsigned long passes, volatile void *buffer);
extern void ub_rowrate2_end(void);
extern void ub_rowrate4(unsigned long passes, volatile void *buffer);
extern void ub_rowrate4_end(void);
extern void ub_rowrate8(unsigned long passes, volatile void *buffer);
extern void ub_rowrate8_end(void);
extern void ub_f1024(unsigned long passes, volatile void *buffer);
extern void ub_f1024_end(void);
extern void ub_ld2mix(unsigned long passes, volatile void *buffer);
extern void ub_ld2mix_end(void);
extern void ub_ld2near(unsigned long passes, volatile void *buffer);
extern void ub_ld2near_end(void);
extern void ub_ld2far(unsigned long passes, volatile void *buffer);
extern void ub_ld2far_end(void);
extern void ub_ld2small(unsigned long passes, volatile void *buffer);
extern void ub_ld2small_end(void);
extern void ub_rtbig(unsigned long passes, volatile void *buffer);
extern void ub_rtbig_end(void);

static uint32_t g_scratch[8];
static FAR uint8_t *g_stream;
static FAR uint8_t *g_far;

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
  unsigned repeat = test->stream ? UB_STREAM_REPEAT : 1;
  unsigned i;

  clock_gettime(CLOCK_MONOTONIC, &start);
  for (i = 0; i < repeat; i++)
    {
      /* A walking loop starts again at the beginning; the pointer it was
       * given is its own, so each repeat covers the same ground.
       */

      fn(test->passes, test->far ? (FAR void *)g_far :
                   test->stream ? (FAR void *)g_stream : g_scratch);
    }

  clock_gettime(CLOCK_MONOTONIC, &end);

  return ((end.tv_sec - start.tv_sec) +
          (end.tv_nsec - start.tv_nsec) / 1000000000.0) / repeat;
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
 * What an SDCLK is
 ****************************************************************************/

/* The controller's timing fields count SDCLK, and every figure the memory
 * model produces rests on what an SDCLK is in MCLK.  That number has never
 * been measured.  It was inferred: the device charges 8.5 MCLK for a read
 * that changes rows against four programmed clocks, which says two MCLK an
 * SDCLK only if the controller's own overhead is nothing, and the overhead
 * is plainly not nothing.  The technical manual says one -- III.1.9.4 has
 * the SDRAM interface running on OSC_W, the clock MCLK is divided from, and
 * MCLKDIV is 0 on this board, so OSC_W is MCLK -- and the loader's own
 * comment (samo_a1.h, "48MHz => 20ns clock cycle") agrees with the manual.
 *
 * A slope settles it and needs no overhead assumption at all: lengthen one
 * field, leave everything else alone, and divide the extra MCLK by the extra
 * SDCLK.  Whatever the fixed cost of an access is, it subtracts out.
 *
 * rowthrash alternates two addresses 1 KB apart, which is the next row of
 * the same bank, so every one of its accesses precharges and activates and
 * all three fields are on its critical path.  alu never leaves the loop it
 * is in and load never leaves one row: those two are the control, and they
 * should not move at all except when the refresh interval does.
 *
 * Nothing here is a risk to the part.  Every setting is longer than the one
 * the machine is already running, and the refresh sweep only ever refreshes
 * more often than it has to.
 */

typedef void (*ub_retime_t)(uint32_t ctl, uint32_t ref);

extern void ub_retime(uint32_t ctl, uint32_t ref);
extern void ub_retime_end(void);

#define UB_SDRAMC_CTL  0x00301604
#define UB_SDRAMC_REF  0x00301608

#define UB_T24NS(x)    (((x) - 1) << 12)   /* tRP, and tRCD with it */
#define UB_T60NS(x)    (((x) - 1) << 8)    /* tRAS */
#define UB_T80NS(x)    (((x) - 1) << 4)    /* tRC, and tRFC with it */
#define UB_FIELDS      (UB_T24NS(4) | UB_T60NS(8) | UB_T80NS(16))

/* Long enough that the 10 ms tick the clock counts in is a fraction of a
 * percent of the answer, short enough that the whole sweep is a minute.
 */

#define UB_SWEEP_PASSES 400000
#define UB_QUIET_PASSES 2000000

/* Accesses per pass of rowthrash: four each of two addresses. */

#define UB_THRASH_ACCESSES 8

struct ub_sweep_s
{
  FAR const char *field;   /* which one this row is moving */
  unsigned trp;
  unsigned tras;
  unsigned trc;
  unsigned aurco;
};

/* The shipped interval, from grifo's sdram.h: held fixed everywhere except
 * where it is the thing being swept.
 */

#define UB_REFRESH 0xe0

static const struct ub_sweep_s g_sweep[] =
{
  /* tRP is tRCD as well, so a step of one puts two more SDCLK on the path
   * from precharge to data.  A straight line, no knee: the cleanest of the
   * three.
   */

  { "tRP",  1, 3, 4,  UB_REFRESH },
  { "tRP",  2, 3, 4,  UB_REFRESH },
  { "tRP",  3, 3, 4,  UB_REFRESH },
  { "tRP",  4, 3, 4,  UB_REFRESH },

  /* tRC is the floor between one activation of a bank and the next, so it
   * does nothing until it exceeds what the access already costs and is a
   * straight line after that.  Where the knee falls is a second answer:
   * near 14 if an SDCLK is one MCLK, near 7 if it is two.
   */

  { "tRC",  1, 3, 4,  UB_REFRESH },
  { "tRC",  1, 3, 6,  UB_REFRESH },
  { "tRC",  1, 3, 8,  UB_REFRESH },
  { "tRC",  1, 3, 10, UB_REFRESH },
  { "tRC",  1, 3, 12, UB_REFRESH },
  { "tRC",  1, 3, 14, UB_REFRESH },
  { "tRC",  1, 3, 16, UB_REFRESH },

  /* tRAS holds the row open before it may be precharged, and with tRC short
   * it is tRAS + tRP that decides how soon the next activation may go.
   */

  { "tRAS", 1, 3, 4,  UB_REFRESH },
  { "tRAS", 1, 4, 4,  UB_REFRESH },
  { "tRAS", 1, 5, 4,  UB_REFRESH },
  { "tRAS", 1, 6, 4,  UB_REFRESH },
  { "tRAS", 1, 7, 4,  UB_REFRESH },
  { "tRAS", 1, 8, 4,  UB_REFRESH },

  /* The refresh counter counts SDCLK too, and refreshing steals the bus
   * from everything including instruction fetch -- so this one moves the
   * controls, and what it costs per refresh is a number the model wants
   * anyway.  Only downwards: more often than the part needs is safe.
   */

  { "ref",  1, 3, 4,  0x20 },
  { "ref",  1, 3, 4,  0x40 },
  { "ref",  1, 3, 4,  0x80 },
  { "ref",  1, 3, 4,  UB_REFRESH },
};

static void ub_sweep(int fd)
{
  /* Three probes: one that never touches memory beyond its own fetch, one
   * that touches one row of it, and one that changes row on every access.
   */

  static const struct ub_case_s probes[] =
  {
    { "alu",       ub_alu,       ub_alu_end,       UB_QUIET_PASSES,
      11, false, false, false },
    { "load",      ub_load,      ub_load_end,      UB_QUIET_PASSES,
      11, false, false, false },
    { "rowthrash", ub_rowthrash, ub_rowthrash_end, UB_SWEEP_PASSES,
      11, false, false, true },
  };

  FAR volatile uint32_t *ctlp = (FAR volatile uint32_t *)UB_SDRAMC_CTL;
  FAR volatile uint32_t *refp = (FAR volatile uint32_t *)UB_SDRAMC_REF;
  uint32_t ctl0 = *ctlp;
  uint32_t ref0 = *refp;
  size_t bytes = (uintptr_t)ub_retime_end - (uintptr_t)ub_retime;
  ub_retime_t retime = (ub_retime_t)UB_IVRAM_BASE;
  int i;

  if (g_far == NULL)
    {
      dprintf(fd, "# no 5 MB buffer, so no sweep\n");
      return;
    }

  /* Its own copy at the base of the window rather than the block offset the
   * measured loops use, so that adding it moved none of them.
   */

  memcpy((FAR void *)UB_IVRAM_BASE, (FAR const void *)ub_retime, bytes);

  dprintf(fd, "# sweep: one field at a time, the others held.  The slope of\n"
              "# cyc/access against the field is MCLK per SDCLK -- 1 if the\n"
              "# manual is right, 2 if the model's fitted value is.\n");
  dprintf(fd, "# %-5s %4s %4s %4s %6s %10s %8s %8s %9s %8s\n",
          "field", "tRP", "tRAS", "tRC", "ref", "ctl", "alu", "load",
          "rowthr", "rt/acc");

  for (i = 0; i < (int)(sizeof(g_sweep) / sizeof(g_sweep[0])); i++)
    {
      const struct ub_sweep_s *s = &g_sweep[i];
      uint32_t ctl = (ctl0 & ~(uint32_t)UB_FIELDS) |
                     UB_T24NS(s->trp) | UB_T60NS(s->tras) | UB_T80NS(s->trc);
      uint32_t ref = (ref0 & ~(uint32_t)0xfff) | s->aurco;
      double cycles[3];
      int p;

      retime(ctl, ref);

      for (p = 0; p < 3; p++)
        {
          cycles[p] = ub_run(&probes[p]) * CONFIG_S1C33E07_MCLK /
                      probes[p].passes;
        }

      /* The register is read back rather than reprinted: the device has
       * refused a write to this block before.
       */

      dprintf(fd, "SW %-5s %4u %4u %4u  0x%03x 0x%08" PRIx32
                  " %8.2f %8.2f %9.2f %8.2f\n",
              s->field, s->trp, s->tras, s->trc, s->aurco, *ctlp,
              cycles[0], cycles[1], cycles[2],
              cycles[2] / UB_THRASH_ACCESSES);
    }

  retime(ctl0, ref0);
  dprintf(fd, "# restored ctl 0x%08" PRIx32 " ref 0x%08" PRIx32 "\n",
          *ctlp, *refp);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  static const struct ub_case_s cases[] =
  {
    { "alu   sdram", ub_alu,      ub_alu_end,      UB_PASSES,      11, false , false , false },
    { "alu   ivram", ub_alu,      ub_alu_end,      UB_PASSES,      11, true  , false , false },
    { "alu+16 sdram", ub_alu_off, ub_alu_off_end,  UB_PASSES,      11, false , false , false },
    { "wide  sdram", ub_wide,     ub_wide_end,     UB_PASSES,      11, false , false , false },
    { "wide  ivram", ub_wide,     ub_wide_end,     UB_PASSES,      11, true  , false , false },
    { "load  sdram", ub_load,     ub_load_end,     UB_PASSES,      11, false , false , false },
    { "load  ivram", ub_load,     ub_load_end,     UB_PASSES,      11, true  , false , false },
    { "store sdram", ub_store,    ub_store_end,    UB_PASSES,      11, false , false , false },
    { "store ivram", ub_store,    ub_store_end,    UB_PASSES,      11, true  , false , false },
    { "size 4     ", ub_s4, ub_s4_end, UB_PASSES, 7, false , false , false },
    { "size 16    ", ub_s16, ub_s16_end, UB_PASSES, 19, false , false , false },
    { "size 32    ", ub_s32, ub_s32_end, UB_LONG_PASSES, 35, false , false , false },
    { "loadb sdram", ub_loadb,   ub_loadb_end,   UB_PASSES, 11, false, false , false },
    { "storeb sdrm", ub_storeb,  ub_storeb_end,  UB_PASSES, 11, false, false , false },
    { "loadseq    ", ub_loadseq,  ub_loadseq_end,  UB_STREAM_PASSES, 11, false, true , false },
    { "storeseq   ", ub_storeseq, ub_storeseq_end, UB_STREAM_PASSES, 11, false, true , false },
    { "copyw      ", ub_copyb, ub_copyb_end, UB_COPY_PASSES, 11, false, true , false },
    { "copyfar    ", ub_copyfar, ub_copyfar_end, UB_COPY_PASSES, 11, false, true , true  },
    { "long  sdram", ub_straight, ub_straight_end, UB_LONG_PASSES, 67, false , false , false },
    { "long  ivram", ub_straight, ub_straight_end, UB_LONG_PASSES, 67, true  , false , false },
    { "ext2  sdram", ub_ext2, ub_ext2_end, UB_PASSES, 11, false , false , false },
    { "ext2  ivram", ub_ext2, ub_ext2_end, UB_PASSES, 11, true  , false , false },
    { "rowthrash  ", ub_rowthrash, ub_rowthrash_end, UB_PASSES, 11, false , false , true  },
    { "bankpair   ", ub_bankpair, ub_bankpair_end, UB_PASSES, 11, false , false , true  },
    { "copyloop   ", ub_copyloop, ub_copyloop_end, UB_WORD_PASSES, 5, false , true , false },
    { "lddisp     ", ub_lddisp, ub_lddisp_end, UB_PASSES, 11, false , false , false },
    { "mult       ", ub_mult, ub_mult_end, UB_PASSES, 11, false , false , false },
    { "callret    ", ub_callret, ub_callret_end, UB_PASSES, 11, false , false , false },
    { "pushpop    ", ub_pushpop, ub_pushpop_end, UB_PASSES, 11, false , false , false },
    { "mmio       ", ub_mmio, ub_mmio_end, UB_PASSES, 11, false , false , false },
    { "loadseq8   ", ub_loadseq8, ub_loadseq8_end, UB_WIDE_PASSES, 11, false , true , false },
    { "copydisp   ", ub_copydisp, ub_copydisp_end, UB_COPY32_PASSES, 20, false , true , false },
    { "bytecopy   ", ub_bytecopy, ub_bytecopy_end, UB_BYTE_PASSES, 19, false , true , false },
    { "bcskew1    ", ub_bcskew1, ub_bcskew1_end, UB_BYTE_PASSES, 19, false , true , false },
    { "bcskew2    ", ub_bcskew2, ub_bcskew2_end, UB_BYTE_PASSES, 19, false , true , false },
    { "bcskew3    ", ub_bcskew3, ub_bcskew3_end, UB_BYTE_PASSES, 19, false , true , false },
    { "ld2        ", ub_ld2, ub_ld2_end, UB_BYTE_PASSES, 19, false , true , false },
    { "ld2skew    ", ub_ld2skew, ub_ld2skew_end, UB_BYTE_PASSES, 19, false , true , false },
    { "st2        ", ub_st2, ub_st2_end, UB_BYTE_PASSES, 19, false , true , false },
    { "st2skew    ", ub_st2skew, ub_st2skew_end, UB_BYTE_PASSES, 19, false , true , false },
    { "ld2bf      ", ub_ld2bf, ub_ld2bf_end, UB_PASSES, 11, false , false , false },
    { "ld2w       ", ub_ld2w, ub_ld2w_end, UB_COPY32_PASSES, 19, false , true , false },
    { "mix16      ", ub_mix16, ub_mix16_end, UB_COPY_PASSES, 27, false , true , false },
    { "mix64      ", ub_mix64, ub_mix64_end, UB_COPY_PASSES, 75, false , true , false },
    { "mix96      ", ub_mix96, ub_mix96_end, UB_COPY_PASSES, 107, false , true , false },

    /* A byte a pass with 0, 4 and 8 register instructions between the load
     * and the store, and then bcpad4's count again at bcpad8's size.  Two
     * of the four vary the count at one size per instruction, and two vary
     * the size at one count.
     */

    { "bcpad0     ", ub_bcpad0, ub_bcpad0_end, UB_PAD_PASSES, 5, false , true , false },
    { "bcpad4     ", ub_bcpad4, ub_bcpad4_end, UB_PAD_PASSES, 9, false , true , false },
    { "bcpad8     ", ub_bcpad8, ub_bcpad8_end, UB_PAD_PASSES, 13, false , true , false },
    { "bcwide4    ", ub_bcwide4, ub_bcwide4_end, UB_PAD_PASSES, 9, false , true , false },

    /* Thirteen instructions and twenty-six bytes each; one, two and four of
     * them touch memory.  Whether the body costs once or once per access.
     */

    { "bcacc1     ", ub_bcacc1, ub_bcacc1_end, UB_ACC_PASSES, 13, false , true , false },
    { "bcacc2     ", ub_bcacc2, ub_bcacc2_end, UB_ACC_PASSES, 13, false , true , false },
    { "bcacc4     ", ub_bcacc4, ub_bcacc4_end, UB_ACC_PASSES, 13, false , true , false },

    /* The same body at every even offset inside the fetch line. */

    { "bcal0      ", ub_bcal0, ub_bcal0_end, UB_ACC_PASSES, 13, false , true , false },
    { "bcal2      ", ub_bcal2, ub_bcal2_end, UB_ACC_PASSES, 13, false , true , false },
    { "bcal4      ", ub_bcal4, ub_bcal4_end, UB_ACC_PASSES, 13, false , true , false },
    { "bcal6      ", ub_bcal6, ub_bcal6_end, UB_ACC_PASSES, 13, false , true , false },
    { "bcal8      ", ub_bcal8, ub_bcal8_end, UB_ACC_PASSES, 13, false , true , false },
    { "bcal10     ", ub_bcal10, ub_bcal10_end, UB_ACC_PASSES, 13, false , true , false },
    { "bcal12     ", ub_bcal12, ub_bcal12_end, UB_ACC_PASSES, 13, false , true , false },
    { "bcal14     ", ub_bcal14, ub_bcal14_end, UB_ACC_PASSES, 13, false , true , false },

    /* Four sizes at four offsets. */

    { "bcs10o0", ub_bcs10o0, ub_bcs10o0_end, UB_ACC_PASSES, 5, false , true , false },
    { "bcs10o4", ub_bcs10o4, ub_bcs10o4_end, UB_ACC_PASSES, 5, false , true , false },
    { "bcs10o8", ub_bcs10o8, ub_bcs10o8_end, UB_ACC_PASSES, 5, false , true , false },
    { "bcs10o12", ub_bcs10o12, ub_bcs10o12_end, UB_ACC_PASSES, 5, false , true , false },
    { "bcs18o0", ub_bcs18o0, ub_bcs18o0_end, UB_ACC_PASSES, 9, false , true , false },
    { "bcs18o4", ub_bcs18o4, ub_bcs18o4_end, UB_ACC_PASSES, 9, false , true , false },
    { "bcs18o8", ub_bcs18o8, ub_bcs18o8_end, UB_ACC_PASSES, 9, false , true , false },
    { "bcs18o12", ub_bcs18o12, ub_bcs18o12_end, UB_ACC_PASSES, 9, false , true , false },
    { "bcs26o0", ub_bcs26o0, ub_bcs26o0_end, UB_ACC_PASSES, 13, false , true , false },
    { "bcs26o4", ub_bcs26o4, ub_bcs26o4_end, UB_ACC_PASSES, 13, false , true , false },
    { "bcs26o8", ub_bcs26o8, ub_bcs26o8_end, UB_ACC_PASSES, 13, false , true , false },
    { "bcs26o12", ub_bcs26o12, ub_bcs26o12_end, UB_ACC_PASSES, 13, false , true , false },
    { "bcs34o0", ub_bcs34o0, ub_bcs34o0_end, UB_ACC_PASSES, 17, false , true , false },
    { "bcs34o4", ub_bcs34o4, ub_bcs34o4_end, UB_ACC_PASSES, 17, false , true , false },
    { "bcs34o8", ub_bcs34o8, ub_bcs34o8_end, UB_ACC_PASSES, 17, false , true , false },
    { "bcs34o12", ub_bcs34o12, ub_bcs34o12_end, UB_ACC_PASSES, 17, false , true , false },
    /* Past the knee: every fetch misses the queue, so these say what a
       byte costs a program with a footprint, and ub_br32 what a taken
       branch costs when branches are half the instructions. */
    { "f128  sdram", ub_f128, ub_f128_end, UB_LONG_PASSES, 131, false , false , false },
    { "f256  sdram", ub_f256, ub_f256_end, UB_LONG_PASSES, 259, false , false , false },
    { "br32  sdram", ub_br32, ub_br32_end, UB_LONG_PASSES, 67, false , false , false },
    { "f1024 sdram", ub_f1024, ub_f1024_end, UB_FETCH_PASSES, 1027, false , false , false },
    /* ld2 with one property changed each, to find which one the model has
       wrong.  Same sixteen byte reads and same pass count as ld2 itself, so
       they compare against it directly. */
    { "ld2fix     ", ub_ld2fix, ub_ld2fix_end, UB_BYTE_PASSES, 19, false , true , false },
    { "ld1walk    ", ub_ld1walk, ub_ld1walk_end, UB_BYTE_PASSES, 19, false , true , false },
    { "rowrate2   ", ub_rowrate2, ub_rowrate2_end, UB_BYTE_PASSES, 19, false , true , false },
    { "rowrate4   ", ub_rowrate4, ub_rowrate4_end, UB_BYTE_PASSES, 19, false , true , false },
    { "rowrate8   ", ub_rowrate8, ub_rowrate8_end, UB_BYTE_PASSES, 19, false , true , false },
    { "ld2mix     ", ub_ld2mix, ub_ld2mix_end, UB_BYTE_PASSES, 19, false , true , false },
    { "ld2near    ", ub_ld2near, ub_ld2near_end, UB_BYTE_PASSES, 19, false , true , false },
    { "ld2far     ", ub_ld2far, ub_ld2far_end, UB_BYTE_PASSES, 19, false , false , true  },
    /* The pair that crosses code footprint over against row changes: a
       two-row loop small enough to stay resident, and a resident two-row
       loop padded until it is not. */
    { "ld2small   ", ub_ld2small, ub_ld2small_end, UB_PASSES, 7, false , true , false },
    { "rtbig      ", ub_rtbig, ub_rtbig_end, UB_PASSES, 23, false , false , true  },

    /* The same body again at four positions inside the fetch line. */

  };


  /* Standard output is where this goes when something else is collecting
   * it -- bench redirects it into the file with everything else. Run on
   * its own from the prompt it has a terminal instead, which nobody can
   * read a table off, so it keeps its own file and unmounts the card.
   */

  /* An argument beginning with a slash is where the table goes; anything
   * else selects the loops whose name contains it.  Running one loop is
   * what makes fitting a parameter bearable: the whole set is over three
   * minutes and a single loop is under one.
   */

  FAR const char *path = CONFIG_SYSTEM_BENCH_MOUNT "/ubench.txt";
  FAR const char *only = NULL;
  int a;

  for (a = 1; a < argc; a++)
    {
      if (argv[a][0] == '/')
        {
          path = argv[a];
        }
      else
        {
          only = argv[a];
        }
    }
  bool standalone = isatty(STDOUT_FILENO);
  int fd = standalone ? bench_card_open(path) : STDOUT_FILENO;
  int i;

  /* The internal-RAM copies keep each loop at its offset within the block,
   * so the copy has to reach the end of the last one that is going to run
   * -- and no further.  Copying the whole block was fine while every loop
   * in the file fitted in the 4 KB window, and stopped being fine when the
   * probe loops at the end pushed it past: 'ubench bcs' runs nothing out of
   * internal RAM and was refused for the size of loops it was not going to
   * use.
   */

  size_t need = 0;

  for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++)
    {
      size_t end;

      if (!cases[i].internal ||
          (only != NULL && strstr(cases[i].name, only) == NULL))
        {
          continue;
        }

      end = (uintptr_t)cases[i].end - (uintptr_t)ub_block_start;
      if (end > need)
        {
          need = end;
        }
    }

  if (need > UB_IVRAM_SIZE)
    {
      printf("ubench: %zu bytes will not fit in internal RAM\n", need);
      return EXIT_FAILURE;
    }

  if (need > 0)
    {
      memcpy((void *)UB_IVRAM_BASE, (const void *)ub_block_start, need);
    }

  g_far = malloc(UB_FAR_BYTES);   /* optional: only the far copy needs it */

  /* Aligned to a bank, not wherever the heap happens to be.  The two-stream
   * loops read addresses a fixed distance apart, so which rows and which
   * banks they land in is decided by this pointer's low bits -- and with a
   * plain malloc those move with whatever ran before.  ld2 measured 212.86
   * cycles a pass on its own and 254.06 inside bench, same binary, same
   * device, same controller settings: a 19% swing from heap state alone,
   * which is larger than the effects these loops exist to measure.
   */

  g_stream = memalign(UB_STREAM_ALIGN, UB_STREAM_BYTES);
  if (g_stream == NULL)
    {
      printf("ubench: no bank-aligned room; falling back, "
             "results will not compare across runs\n");
      g_stream = malloc(UB_STREAM_BYTES);
    }
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
  /* Where the streams live decides which rows and banks the two-stream
     loops touch, so a run that cannot be compared with another says so in
     its own header rather than in a later argument about the numbers. */

  dprintf(fd, "# stream %p, far %p\n", g_stream, g_far);

  /* Not a selection of loops but a different instrument: the loops are the
   * same three throughout and what varies is the controller underneath them.
   */

  if (only != NULL && strcmp(only, "sdclk") == 0)
    {
      ub_sweep(fd);
      if (standalone)
        {
          bench_card_close(fd, path);
        }

      return EXIT_SUCCESS;
    }

  dprintf(fd, "# %-12s %6s %5s %9s %10s %10s\n", "loop", "bytes", "insn",
          "seconds", "cyc/pass", "cyc/instr");

  for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++)
    {
      double seconds;

      if (only != NULL && strstr(cases[i].name, only) == NULL)
        {
          continue;
        }

      if (cases[i].far && g_far == NULL)
        {
          dprintf(fd, "UB %-12s   no room for a 5 MB buffer\n",
                  cases[i].name);
          continue;
        }

      seconds = ub_run(&cases[i]);
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
  if (only == NULL)
    {
      ub_boundary(fd, (uintptr_t)ub_alu_end - (uintptr_t)ub_alu);
    }

  if (standalone)
    {
      bench_card_close(fd, path);
    }

  return EXIT_SUCCESS;
}
