#ifndef SDRAMC_H
#define SDRAMC_H

#include <stdint.h>
#include <stdbool.h>

#include "mem.h"

/* SDRAM controller, REG_BASE+0x1600..0x1610. */
#define SDRAMC_BASE 0x1600u
#define SDRAMC_LEN  0x0014u

struct sdramc {
	uint32_t reg[SDRAMC_LEN / 4];
	bool     initialised;
	unsigned long writes;
};

void sdramc_attach(struct mem *m, struct sdramc *s);
void sdramc_reset(struct sdramc *s);

#endif /* SDRAMC_H */
