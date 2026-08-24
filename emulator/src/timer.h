#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>
#include <stdbool.h>

#include "mem.h"

struct timerblk {
	const uint64_t *cycles;   /* points at cpu.cycles */
	uint32_t latched;
	bool     paused;
	unsigned long reads;
	/*
	 * Interactive runs derive the tick from wall-clock time instead of
	 * from the instruction count. See the note in timer.c.
	 */
	bool     wallclock;
	uint64_t t0_ns;
};

/* Drive the tick from real elapsed time rather than emulated cycles. */
void timer_use_wallclock(struct timerblk *t);

void timer_attach(struct mem *m, struct timerblk *t, const uint64_t *cycles);

#endif /* TIMER_H */
