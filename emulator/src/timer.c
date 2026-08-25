#include <stdio.h>
#include <stdlib.h>
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

/* Channel 2, the suspend wake timer. */
#define OFF_CR2A       (0x790u - T16_BASE)
#define OFF_CTL2       (0x796u - T16_BASE)
#define OFF_CLKCTL_2   (0x7e4u - T16_BASE)

#define PRUNx          (1u << 0)     /* run/stop */
/*
 * The prescaler select is a power-of-two divider of MCLK; suspend.c picks
 * P16TSx_MCLK_DIV_4096 and computes its reload as
 * (MCLK / 32 / 4096) * seconds, so the effective tick is MCLK/4096 with a
 * further divide by 32 folded into the count.
 */
/*
 * The prescaler divides by 4096, but the suspend code switches the system
 * clock to OSC3/32 before arming the timer, which is the other factor in
 * its own reload calculation: (MCLK / 32 / 4096) * seconds. Modelling only
 * the 4096 made the timeout fire 32x early, so the firmware concluded it
 * had timed out and powered the device off instead of resuming.
 */
#define T2_PRESCALE    (4096u * 32u)

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

void timer_poll(struct timerblk *t, struct c33 *cpu)
{
	if (!t->t2_running || !t->cycles)
		return;
	if (*t->cycles < t->t2_deadline)
		return;
	t->t2_running = false;
	t->t2_fires++;
	if (t->itc)
		itc_set_flag((struct itc *)t->itc, VECTOR_T16_CH2);
	c33_raise_irq(cpu, VECTOR_T16_CH2,
		      t->itc ? itc_priority(t->itc, VECTOR_T16_CH2) : 7);
}

static bool timer_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
		       bool is_write)
{
	struct timerblk *t = ctx;
	uint32_t reg = off - T16_BASE;

	if (is_write) {
		if (reg == OFF_CNT_PAUSE)
			t->paused = (*val != 0);
		if (reg / 2 < 0x80 / 2)
			t->reg[reg / 2] = (uint16_t)*val;
		if (reg == OFF_CLKCTL_2)
			t->clkctl2 = (uint16_t)*val;
		if (reg == OFF_CTL2) {
			/* Starting the timer arms the wake-up deadline. */
			if ((*val & PRUNx) && !t->t2_running) {
				uint64_t n = t->reg[OFF_CR2A / 2];
				t->t2_running = true;
				uint64_t span = (uint64_t)n * T2_PRESCALE;
				/*
				 * Development knob: the firmware asks for a
				 * 120 s suspend timeout, which is a long wait
				 * to reproduce anything that happens at the
				 * far end of it. Dividing the span here rather
				 * than editing the firmware keeps the guest
				 * byte-identical to the shipping build.
				 */
				const char *sc = getenv("WREMU_SUSPEND_DIV");
				if (sc && atoi(sc) > 1)
					span /= (unsigned)atoi(sc);
				t->t2_deadline = (t->cycles ? *t->cycles : 0) + span;
			} else if (!(*val & PRUNx)) {
				t->t2_running = false;
			}
		}
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

void timer_reset(struct timerblk *t)
{
	const uint64_t *cycles = t->cycles;
	const struct itc *itc = t->itc;
	bool wall = t->wallclock;
	uint64_t t0 = t->t0_ns;
	memset(t, 0, sizeof *t);
	t->cycles = cycles;
	t->itc = itc;
	t->wallclock = wall;
	t->t0_ns = t0;
}

void timer_attach(struct mem *m, struct timerblk *t, const uint64_t *cycles,
		  const struct itc *itc)
{
	memset(t, 0, sizeof *t);
	t->cycles = cycles;
	t->itc = itc;
	mem_add_mmio(m, "t16", T16_BASE, T16_LEN, timer_mmio, t);
}
