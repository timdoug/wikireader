#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include <stdbool.h>

#include "mem.h"
#include "c33.h"
#include "itc.h"
#include "cmu.h"

struct timerblk {
	const uint64_t *cycles;   /* points at the emulator's 60 MHz cpu.clk */
	unsigned long reads;
	/*
	 * Interactive runs derive the tick from wall-clock time instead of
	 * from the instruction count. See the note in timer.c.
	 */
	bool     wallclock;
	uint64_t t0_ns;

	uint16_t reg[0x80 / 2];      /* T16 block, by halfword */
	/*
	 * Comparison registers have a separately addressable staging buffer.
	 * CTLx.SELCRB selects which bank the CRxA/CRxB MMIO addresses expose;
	 * PRESET or comparison B copies the buffer into the active bank.
	 */
	uint16_t compare[6][2];
	uint16_t compare_buffer[6][2];
	uint16_t count[6];
	uint64_t phase[6];           /* fractional input below one timer tick */
	uint64_t last_raw;
	bool     deadline_valid;
	uint64_t next_deadline;      /* next internal compare, in raw MCLK clocks */
	const struct itc *itc;
	const struct cmu *cmu;
	unsigned long fires[6][2];   /* comparison A/B matches */
};

#define VECTOR_T16_B(ch) (30u + 4u * (ch))
#define VECTOR_T16_A(ch) (VECTOR_T16_B(ch) + 1u)
#define VECTOR_T16_CH2   VECTOR_T16_B(2)

/* Drive the tick from real elapsed time rather than emulated cycles. */
void timer_use_wallclock(struct timerblk *t);
/* Advance all running timers and present any resulting interrupt. */
void timer_poll(struct timerblk *t, struct c33 *cpu);
/* Clear counters and pending deadlines, keeping the clock sources. */
void timer_reset(struct timerblk *t);

void timer_attach(struct mem *m, struct timerblk *t, const uint64_t *cycles,
		  const struct itc *itc, const struct cmu *cmu);

#endif /* TIMER_H */
