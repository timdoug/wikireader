/*
 * Timing-model parameters that the S1C33E07 manual does not pin down and
 * that were fitted to a real WikiReader instead (see README.md, "Calibration").
 *
 * The defaults are the fitted values.  WREMU_MODEL="name=value,..." overrides
 * any of them for an experiment; `model_describe()` prints the set in use.
 */
#ifndef WREMU_MODEL_H
#define WREMU_MODEL_H

#include <stdio.h>

struct model {
	/* Cycles for a taken conditional branch whose target is fetched from
	   SDRAM, and from internal RAM (manual: 2 or 3 for both). */
	unsigned branch_taken;
	unsigned branch_taken_iram;
	/* Extra SDCLK ticks before the first halfword of an instruction-queue
	   line fill (controller and bus overhead). */
	unsigned iqb_first;
	/* Extra ticks between successive words of a line fill: the controller
	   fetches a line as separate 32-bit reads, not one burst. */
	unsigned iqb_word_gap;
	/* Extra ticks on every data-queue fill (a 32-bit read). */
	unsigned dq_extra;
	/* Ticks a CPU write occupies the bus regardless of size; 0 keeps the
	   manual's one tick per 16-bit transfer. */
	unsigned wr_ticks;
	/* Extra MCLK cycles per HSDMA or IDMA transfer. */
	unsigned dma_extra;
	/* MCLK cycles from a READ command to the card's data token. */
	unsigned long sd_read_latency;
	/* Extra cycles per instruction fetched from internal RAM (A0, IVRAM),
	   which the manual-only model treats as zero-wait. */
	unsigned iram_fetch_wait;
	/* Extra SDCLK ticks before a read that follows a write on the SDRAM
	   bus (write recovery and bus turnaround). */
	unsigned wr_rd_turn;
};

extern struct model model;

/* Apply WREMU_MODEL overrides to the defaults. */
void model_init(void);
void model_describe(FILE *out);

#endif
