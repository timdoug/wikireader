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

	/* SDRAM-interface time is kept in half-MCLK ticks (DBF uses one). */
	uint64_t bus_free;
	uint64_t next_refresh;
	uint64_t last_sdram_access;
	bool self_refresh;
	struct {
		bool valid;
		uint32_t row;
		uint64_t activated;
	} bank[4];
	struct {
		bool valid;
		uint32_t tag;
		uint64_t ready[8];
	} iq[2];
	unsigned iq_next;
	struct {
		bool valid;
		uint32_t tag;
		uint64_t ready[2];
	} dq;

	/* Diagnostics: controller transactions, not host memory copies. */
	uint64_t accesses[5];
	uint64_t wait_cycles;
	uint64_t iq_hits, iq_misses;
	uint64_t dq_hits, dq_misses;
	uint64_t writes_timed;
	uint64_t refreshes, self_refresh_exits;
};

void sdramc_attach(struct mem *m, struct sdramc *s);
void sdramc_reset(struct sdramc *s);

#endif /* SDRAMC_H */
