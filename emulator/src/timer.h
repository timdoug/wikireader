#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include <stdbool.h>

#include "mem.h"
#include "c33.h"
#include "itc.h"

struct timerblk {
	const uint64_t *cycles;   /* points at cpu.cycles */
	uint32_t latched;
	bool     paused;
	uint64_t tick_bias;      /* raw ticks excluded while the pair is paused */
	unsigned long reads;
	/*
	 * Interactive runs derive the tick from wall-clock time instead of
	 * from the instruction count. See the note in timer.c.
	 */
	bool     wallclock;
	uint64_t t0_ns;

	/*
	 * Channel 2, which grifo's suspend path uses as its wake source: it
	 * programs a timeout, enables the underflow interrupt and halts.
	 * Without this the machine suspends and never comes back.
	 */
	uint16_t reg[0x80 / 2];      /* T16 block, by halfword */
	/*
	 * Comparison registers have a separately addressable staging buffer.
	 * CTLx.SELCRB selects which bank the CRxA/CRxB MMIO addresses expose;
	 * PRESET or comparison B copies the buffer into the active bank.
	 */
	uint16_t compare[6][2];
	uint16_t compare_buffer[6][2];
	uint16_t count[6];
	uint16_t clkctl2;
	bool     t2_running;
	uint64_t t2_deadline;        /* in MCLK cycles */
	const struct itc *itc;
	unsigned long t2_fires;
};

#define VECTOR_T16_CH2  38           /* 16-bit timer 2 compare match B */

/* Drive the tick from real elapsed time rather than emulated cycles. */
void timer_use_wallclock(struct timerblk *t);
/* Fire the channel-2 wake interrupt when its timeout expires. */
void timer_poll(struct timerblk *t, struct c33 *cpu);
/* Clear counters and the channel-2 wake timer, keeping the clock source. */
void timer_reset(struct timerblk *t);

void timer_attach(struct mem *m, struct timerblk *t, const uint64_t *cycles,
		  const struct itc *itc);

#endif /* TIMER_H */
