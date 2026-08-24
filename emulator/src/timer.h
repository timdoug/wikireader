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
};

void timer_attach(struct mem *m, struct timerblk *t, const uint64_t *cycles);

#endif /* TIMER_H */
