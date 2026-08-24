/*
 * 16-bit timer block (REG_BASE+0x780), enough for grifo's tick source.
 *
 * Tick_get() in samo-lib/drivers/src/tick.c pauses the counters and reads a
 * 32-bit tick out of two 16-bit registers:
 *
 *     count = (REG_T16_TC5 << 16) | REG_T16_TC0;
 *
 * so TC0 is the low half and TC5 the high half of one free-running counter.
 * We derive it from the emulated instruction count, which gives a monotonic
 * clock without needing real-time pacing.
 */

#include <string.h>
#include <time.h>

#include "timer.h"

#define T16_BASE       0x0780u
#define T16_LEN        0x0080u

#define OFF_TC0        (0x784u - T16_BASE)
#define OFF_TC5        (0x7acu - T16_BASE)
#define OFF_CNT_PAUSE  (0x7dcu - T16_BASE)

/*
 * One tick per instruction.
 *
 * The tick is a 60 MHz timebase -- Tick_TicksPerMicroSecond = 60 in
 * samo-lib/drivers/include/tick.h, and grifo.h agrees with
 * TIMER_CountsPerMicroSecond = 60 -- and the C33 core runs at the same
 * clock, so roughly one instruction per tick is the right scale.
 *
 * This matters: wiki.app debounces its incremental search by
 * DELAYED_SEARCH_TIME (0.3s), which is 18 million ticks. A slower tick
 * meant that deadline was never reached and the search never ran.
 */
#define CYCLES_PER_TICK 1u

/* Tick_TicksPerMicroSecond in samo-lib/drivers/include/tick.h. */
#define TICKS_PER_MICROSECOND 60u

static uint64_t mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/*
 * Wall-clock tick source, used when there is a window.
 *
 * Deriving the tick from the instruction count makes the guest's notion of
 * elapsed time proportional to how fast the host happens to be emulating,
 * which is fine headless -- it is deterministic and reproducible -- but
 * wrong under a human's hand. This build runs at about 0.75x real time, so
 * a one-second drag looks like 0.75 s to the firmware, and wikilib derives
 * finger_move_speed as pixels per tick: the scroll momentum comes out
 * inflated by the same factor. The ratio also moves around with host load
 * and with what the firmware is doing, so the fling feels inconsistent
 * rather than merely fast.
 *
 * Interactively the honest clock is the real one: the user's seconds are
 * the seconds the application should measure.
 */
void timer_use_wallclock(struct timerblk *t)
{
	t->wallclock = true;
	t->t0_ns = mono_ns();
}

static uint32_t now(struct timerblk *t)
{
	if (t->wallclock) {
		uint64_t us = (mono_ns() - t->t0_ns) / 1000ull;
		return (uint32_t)(us * TICKS_PER_MICROSECOND);
	}
	if (!t->cycles)
		return 0;
	return (uint32_t)(*t->cycles / CYCLES_PER_TICK);
}

static bool timer_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
		       bool is_write)
{
	struct timerblk *t = ctx;
	uint32_t reg = off - T16_BASE;

	if (is_write) {
		if (reg == OFF_CNT_PAUSE)
			t->paused = (*val != 0);
		return true;             /* configuration accepted */
	}

	uint32_t tick = t->paused ? t->latched : now(t);

	switch (reg) {
	case OFF_TC0:
		/* Latch on the low half so the two reads stay coherent. */
		t->latched = tick;
		*val = tick & 0xffff;
		t->reads++;
		return true;
	case OFF_TC5:
		*val = (t->latched >> 16) & 0xffff;
		t->reads++;
		return true;
	default:
		*val = 0;
		return true;
	}
}

void timer_attach(struct mem *m, struct timerblk *t, const uint64_t *cycles)
{
	memset(t, 0, sizeof *t);
	t->cycles = cycles;
	mem_add_mmio(m, "t16", T16_BASE, T16_LEN, timer_mmio, t);
}
