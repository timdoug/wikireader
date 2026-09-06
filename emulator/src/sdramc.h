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

	struct mem *mem;   /* receives the configured size for address aliasing */

	/* Diagnostics: controller transactions, not host memory copies. */
	uint64_t activations;   /* row activates: each one is a page miss */
	uint64_t act_kind[5][5]; /* [previous access kind][this kind] per bank */
	uint64_t act_bank[4];
	uint64_t kind_bank[5][4]; /* accesses by kind per bank */
	unsigned bank_last_kind[4];
	uint64_t accesses[5];
	uint64_t wait_cycles;
	uint64_t iq_hits, iq_misses;
	uint64_t dq_hits, dq_misses;
	uint64_t writes_timed;
	uint64_t refreshes, self_refresh_exits;
	/* WREMU_ROWHIST=1: activations per 1 KiB row of the whole SDRAM, so a
	   hot loop's row changes can be traced to the objects behind them. */
	uint64_t *row_hist;
	/* ... and, per bank, which row each activation replaced: an open
	   hash of (previous row, new row) pairs with counts. */
	struct sdramc_pair { uint32_t key; uint64_t n; } *pair_hist;
	uint32_t pair_last[4];
};
#define SDRAMC_PAIR_HIST_SIZE (1u << 16)
/* WREMU_ROWTRACE=0xADDR: print the PC and kind behind the first activations
   of that address's row while the profile window is open. */
extern bool sdramc_trace_on;

#define SDRAMC_ROW_HIST_ROWS (32u * 1024u)   /* 32 MB in 1 KiB rows */

void sdramc_attach(struct mem *m, struct sdramc *s);
void sdramc_reset(struct sdramc *s);

#endif /* SDRAMC_H */
