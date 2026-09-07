/*
 * Timing-model parameters that the S1C33E07 manual does not pin down and
 * that were fitted to a real WikiReader instead (see README.md, "Calibration").
 *
 * The defaults are the fitted values.  WREMU_MODEL="name=value,..." overrides
 * any of them for an experiment; `model_describe()` prints the set in use.
 */
#ifndef WREMU_MODEL_H
#define WREMU_MODEL_H

#include <stdint.h>
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
	/* Extra SDCLK ticks on a data access issued by code running from
	   internal RAM.  Fitted, not from the manual: with the fetch tests
	   matching the device exactly, the phases that run from A0 RAM and
	   the IVRAM overlays still came out a fifth fast, and reads from
	   such code measure 2.6 cycles on the device against 1.6 modelled. */
	unsigned dq_iram_extra;
	/* Extra SDCLK ticks on a read the data queue already holds.  The
	   model served those free, which is why a loop reading one word from
	   A0 RAM measured 1.6 cycles against the device's 2.6, and why the
	   decoder's byte traffic, which hits the queue three times in four,
	   came out a fifth fast. */
	unsigned dq_hit;
};

extern struct model model;
extern uint32_t wremu_cur_pc;

/* Apply WREMU_MODEL overrides to the defaults. */
void model_init(void);
void model_describe(FILE *out);

#endif
