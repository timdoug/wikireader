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
	/* The controller's own overheads, in half-MCLK. They are the
	   pipelining inside the controller rather than anything the SDRAM
	   does, and the device puts several of them at half an SDCLK, which
	   is a figure the bus clock cannot express. The datasheet timings --
	   tRP, tRAS, tRC, CAS -- stay in SDCLK, through sd_tick. */
	unsigned iqb_first;
	/* Extra ticks between successive words of a line fill: the controller
	   fetches a line as separate 32-bit reads, not one burst. */
	unsigned iqb_word_gap;
	/* Extra ticks on every data-queue fill (a 32-bit read). */
	unsigned dq_extra;
	/* Ticks a CPU write occupies the bus regardless of size; 0 keeps the
	   manual's one tick per 16-bit transfer. */
	unsigned wr_ticks;
	/* Fitted extra MCLK cycles per SPI-triggered HSDMA or IDMA unit. */
	unsigned dma_extra;
	/* Uncalibrated memory-DMA overhead per unit. Keep separate from the
	   fitted SPI allowance; zero gives the documented bus-phase floor. */
	unsigned dma_mem_extra;
	/* MCLK cycles from a READ command to the card's data token. */
	unsigned long sd_read_latency;
	/* Card-specific waits, separate from CPU/DMA costs. Zero preserves
	   the historical immediate-ready model until a card is calibrated. */
	unsigned long sd_init_latency;  /* first ACMD41/CMD1 to ready */
	unsigned long sd_read_gap;      /* between streamed data blocks */
	unsigned long sd_write_latency; /* programming busy after a block */
	/* Extra cycles per instruction fetched from internal RAM (A0, IVRAM),
	   which the manual-only model treats as zero-wait. */
	unsigned iram_fetch_wait;
	/* ...and per fetch from IVRAM or DSTRAM, which are not the same
	   memory: A0 measured exactly one cycle and these measure about two.
	   Fitted 2026-09-13 against ubench on the device. */
	unsigned ivram_fetch_wait;
	/* Whether a queue line survives its row being closed. It does not on
	   the hardware; zero restores the old behaviour, which is the way to
	   ask what code straddling a page boundary is costing. */
	unsigned iq_row_evict;
	unsigned iq_lookahead;
	/* How many rows the controller can hold open, and what decides which
	   one a request lands on. Set, the row register is chosen by what the
	   access is -- a fetch, a load or a store, the display's DMA -- and
	   not by which bank the address decodes to, so two data addresses
	   evict one another however far apart they are. The device says they
	   do: reading two addresses a kilobyte apart costs 141.75 cycles a
	   pass and four megabytes apart, in another bank by the geometry
	   table, costs 140.10. */
	unsigned row_ports;
	/* An SDCLK in half-MCLK units, with DBF clear. Two is SDCLK = MCLK;
	   four is SDCLK = MCLK/2, which is what the device measures. */
	unsigned sdclk_half;
	/* What changing rows costs beyond tRP + tRCD, in half-MCLK. The
	   device reads a row it has open in CAS + data and one it has not in
	   about an SDCLK more than the datasheet's two timings account for,
	   which is the controller issuing the precharge rather than the
	   array performing it. Without it the two cannot both be right:
	   loadseq wants a cheaper read and rowthrash a dearer miss. */
	unsigned row_change_extra;
	/* How many writes the controller can hold without stopping the CPU.
	   Zero makes every write block until the bus has taken it. */
	unsigned write_post;
	/* What a call or a return costs beyond the manual's figure, for the
	   queue it discards. */
	unsigned call_extra;
	/* What one peripheral register access costs, in MCLK. */
	unsigned mmio_wait;
	/* Which cycle after the read command the first halfword lands on:
	   CAS, or the one after it. */
	unsigned cas_first;
	/* Whether the internal-RAM fetch wait falls per 32-bit word rather
	   than per instruction. */
	unsigned iram_word_fetch;
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
	unsigned dq_hit;        /* in half-MCLK: 2 is one cycle */
};

extern struct model model;
extern uint32_t wremu_cur_pc;

/* Apply WREMU_MODEL overrides to the defaults. */
void model_init(void);
void model_describe(FILE *out);

#endif
