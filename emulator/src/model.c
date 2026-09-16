#include <stdlib.h>
#include <string.h>

#include "model.h"

/*
 * Hardware-calibrated defaults for the 32 MB WikiReader (board 7), fitted
 * 2026-09-07 and three of them refitted 2026-09-13 against a device running
 * the NuttX port's ubench: a loop of known size and content, run at a range
 * of sizes and from both memories, which separates the cost of a fetch from
 * the cost of what was fetched. See README.md, Calibration.
 * The manual-only values are branch costs of 3 and every overhead 0.
 */
/* The PC of the instruction being executed, for the SDRAM row trace
 * (sdramc.c); kept here because every test links the model. */
uint32_t wremu_cur_pc;
bool wremu_fetch_restart;

struct model model = {
	/* Refitted 2026-09-13 against a 60 MHz guest clock. Everything here
	   was a fifth larger while the timer ran at 48. */
	.branch_taken = 5,          /* refitted 2026-09-13, see README */
	.branch_taken_iram = 6,     /* cpu-loop-a0/ivram/dstram all measure 5.0 */
	/* half-MCLK. The micro loops want eight and Dhrystone wants zero:
	   they all fit the queue, so they measure the fill of a line that is
	   refetched every pass, where a program with a real code footprint
	   measures a queue that misses constantly. Four splits it. */
	.iqb_first = 1,
	.iqb_word_gap = 0,
	.dq_extra = 0,
	.wr_ticks = 9,         /* refitted once writes were posted */
	/* Zero since the row model: three clocks of bus turn stood in for
	   what a copy really pays, which is a row change on every access,
	   and charging both now costs more than the device does. */
	.wr_rd_turn = 0,
	.row_ports = 1,
	.sdclk_half = 2,
	.row_change_extra = 1,
	.bank_floors = 1,
	/* One: the device copies a word at a time faster than four at a
	   time, which only happens if a store retires before the bus has
	   taken it and a run of stores with nothing between them fills up. */
	.write_post = 1,
	.call_extra = 2,
	/* Eight MCLK a register access. The device runs eight reads of a
	   controller register at 82.50 cycles a pass where the same loop of
	   adds costs 15.15; this was zero until it was asked. */
	.mmio_wait = 8,
	/* Zero: the first halfword lands on the CAS cycle itself, which is
	   what CAS latency means. Charging the cycle after it made every
	   external read two MCLK dear -- loadseq, loadseq8 and copyloop were
	   all 1.16 to 1.21 of the device for that one reason, and the fetch
	   loops carried it too. */
	.cas_first = 0,
	/* The internal bus is 32 bits wide: the wait falls on the fetch that
	   starts a word, not on the halfword after it. Charging every
	   instruction made a loop of two-byte ones cost twice as much a byte
	   as a loop of four-byte ones, and the device's loops out of
	   internal RAM all cost about 0.9 cycles a code byte. */
	.iram_word_fetch = 1,
	.dma_extra = 30,
	/* Both refitted 2026-09-13 against a cardb sweep whose filesystem has
	   one sector per cluster, so the driver issues a command per 512
	   bytes and a per-command cost is most of the time rather than a
	   rounding error. Read latency had been 60000 -- a millisecond a
	   command -- which nothing could see while the only card workload
	   measured was grifo reading 255 sectors at a time. */
	.sd_read_latency = 12000,
	/* What the card is busy for after taking a block: the device spends
	   longer writing a sector than reading one, and the difference is the
	   card programming itself. */
	.sd_write_latency = 47000,
	.iram_fetch_wait = 0,       /* fetch-a0 measured exactly 1.0 cycle */
	.ivram_fetch_wait = 1,      /* ...but ubench measures 1.9 from IVRAM */
	.iq_row_evict = 1,          /* measured: a page crossing costs 3.2x */
	/* Six, and it was off for four months for a reason that act_overlap
	   removed.  The device runs a loop body fast while its offset inside
	   the 16-byte line plus its size is at most about 27 bytes and slowly
	   from 30 -- not by how many lines it spans, which is what charging by
	   span gets wrong in both directions.  A fetcher running six bytes
	   ahead reproduces that rule: the bc* family goes from 0.386 to 0.188
	   RMS log error and its 2.5-3x misclassifications land inside 10%
	   (bcs18o12 2.963 -> 0.917, bcal4 2.607 -> 1.022).

	   It was off because the bus time a speculative fill charges made real
	   code worse -- CoreMark 1.05 -> 0.87, Dhrystone 1.01 -> 0.92, and the
	   rv32 interpreter 0.0793 -> 0.0955.  That was measured before
	   activations overlapped the data bus.  They do now, the fill no
	   longer queues ahead of real work, and the same interpreter measures
	   0.0785 with the lookahead on: slightly better than without it.

	   Two decompositions were tried and are not the answer.  Filling only
	   into an idle interface gates the eviction too, which is the half
	   that matters, and the bc* family gets worse (0.386 -> 0.445).
	   Evicting without filling makes real code refetch constantly
	   (0.0793 -> 0.2111).

	   What it costs: ubench's whole RMS falls 0.2595 -> 0.1986 but nine
	   loops leave the 10% band as moderate error spreads -- pure-fetch
	   loops become a little too cheap (br32 0.991 -> 1.142) and rowthrash
	   0.805.  A model that is never wrong by 3x and more often wrong by
	   12% is the better instrument, because the 3x cases were calling
	   fast loops slow. */
	.iq_lookahead = 6,
	.iq_lookahead_seq = 1,
	.dq_iram_extra = 2,
	/* Four, from ubench's br32: `long` with half its adds replaced by an
	   undelayed jump to the following instruction, the same 67
	   instructions in the same 138 bytes, which the device runs in 312
	   cycles against long's 183. This model ran it in 193 -- the queue had
	   already fetched the line, so the jump cost nothing, and the manual's
	   three execute cycles hid under a fetch schedule the device does not
	   get to overlap. At four the loop lands on 318 against 312, and every
	   other loop in the file is unchanged to the cycle.

	   Two other shapes were tried against the same measurement and are
	   not here. A flat charge on every control transfer breaks the
	   conditional case that already fits: alu's one taken jrne a pass is
	   priced by branch_taken and wants nothing more. Evicting the queue on
	   a branch does not fix br32 at all -- its 138-byte body thrashes the
	   queue either way -- and costs alu 15.15 -> 39.60 against a device
	   that says 15.00, because a resident loop then refetches itself every
	   pass. */
	.branch_bubble = 4,
	/* One, and now measured rather than assumed. Two streams read
	   alternately is the shape of every loop this model overcharges, so a
	   second entry was the obvious explanation; it is wrong twice over.
	   rowthrash, which alternates between two addresses a kilobyte apart
	   and exists to probe this, collapses from 138.15 to 23.10 against a
	   device that says 145.95 -- so the hardware really does hold one
	   word. And the loops it was meant to fix do not move at all: st2
	   stays at 0.697 of the device and ld2w at 0.732, which says they were
	   never queue-depth-limited in the first place. */
	.dq_entries = 1,
	/* One. A row activation is a command and tRCD elapses inside the bank,
	   so another bank may move data throughout; this model had the whole
	   access, activation included, queue behind the data bus, and the
	   error showed up wherever code had to be fetched *and* the data
	   changed rows -- the two together, neither alone.

	   ubench's rowthrash and rtbig differ in nothing but twelve adds
	   padding the body past the fetch window: the device charges 24.90
	   cycles for them and the model charged 75.75, which is 2.7 cycles a
	   code byte where a fetch-only loop measures 1.33 in both. Overlapping
	   takes the whole two-stream cluster from 0.67-0.78 of the device to
	   0.80-0.88, ubench from 0.273 to 0.261 RMS log error with six more
	   loops inside 10%, and the rv32 interpreter's four builds from 0.0902
	   to 0.0789. loadseq lands exactly.

	   It has to clamp the transfer to the bus in schedule_write as well as
	   schedule_read, and for a while it did not. A store then began at the
	   moment it was issued however busy the bus was, a run of them never
	   filled it up, and ramspeed's memset ran at 2.11x the device's rate --
	   which the loops called 12% because storeseq is a loop and memset is
	   nothing else. With both clamped it is 1.08x, and the whole-program
	   suite goes from 0.236 to 0.090 RMS log error. */
	.act_overlap = 1,
	.dq_hit = 1,
};

static const struct {
	const char *name;
	unsigned long *ul;
	unsigned *u;
} fields[] = {
	{ "branch_taken", NULL, &model.branch_taken },
	{ "branch_taken_iram", NULL, &model.branch_taken_iram },
	{ "iqb_first", NULL, &model.iqb_first },
	{ "iqb_word_gap", NULL, &model.iqb_word_gap },
	{ "dq_extra", NULL, &model.dq_extra },
	{ "wr_ticks", NULL, &model.wr_ticks },
	{ "dma_extra", NULL, &model.dma_extra },
	{ "dma_mem_extra", NULL, &model.dma_mem_extra },
	{ "sd_read_latency", &model.sd_read_latency, NULL },
	{ "sd_init_latency", &model.sd_init_latency, NULL },
	{ "sd_read_gap", &model.sd_read_gap, NULL },
	{ "sd_write_latency", &model.sd_write_latency, NULL },
	{ "iram_fetch_wait", NULL, &model.iram_fetch_wait },
	{ "ivram_fetch_wait", NULL, &model.ivram_fetch_wait },
	{ "iq_row_evict", NULL, &model.iq_row_evict },
	{ "iq_lookahead", NULL, &model.iq_lookahead },
	{ "iq_lookahead_seq", NULL, &model.iq_lookahead_seq },
	{ "wr_rd_turn", NULL, &model.wr_rd_turn },
	{ "row_ports", NULL, &model.row_ports },
	{ "sdclk_half", NULL, &model.sdclk_half },
	{ "row_change_extra", NULL, &model.row_change_extra },
	{ "bank_floors", NULL, &model.bank_floors },
	{ "write_post", NULL, &model.write_post },
	{ "call_extra", NULL, &model.call_extra },
	{ "mmio_wait", NULL, &model.mmio_wait },
	{ "cas_first", NULL, &model.cas_first },
	{ "iram_word_fetch", NULL, &model.iram_word_fetch },
	{ "dq_iram_extra", NULL, &model.dq_iram_extra },
	{ "dq_hit", NULL, &model.dq_hit },
	{ "branch_bubble", NULL, &model.branch_bubble },
	{ "dq_entries", NULL, &model.dq_entries },
	{ "act_overlap", NULL, &model.act_overlap },
};

void model_init(void)
{
	const char *env = getenv("WREMU_MODEL");
	char *copy, *item, *save = NULL;

	if (!env)
		return;
	copy = strdup(env);
	for (item = strtok_r(copy, ",", &save); item;
	     item = strtok_r(NULL, ",", &save)) {
		char *eq = strchr(item, '=');
		unsigned i;

		if (!eq)
			continue;
		*eq = '\0';
		for (i = 0; i < sizeof fields / sizeof fields[0]; i++) {
			if (strcmp(fields[i].name, item))
				continue;
			if (fields[i].ul)
				*fields[i].ul = strtoul(eq + 1, NULL, 0);
			else
				*fields[i].u = (unsigned)strtoul(eq + 1, NULL, 0);
			break;
		}
		if (i == sizeof fields / sizeof fields[0])
			fprintf(stderr, "WREMU_MODEL: unknown parameter %s\n", item);
	}
	free(copy);
}

void model_describe(FILE *out)
{
	fprintf(out, "--- model: branch_taken %u/%u, iqb_first %u, iqb_word_gap %u,"
		" dq_extra %u, wr_ticks %u, wr_rd_turn %u, row_change_extra %u,"
		" sdclk_half %u, row_ports %u, dma_extra %u, dma_mem_extra %u,"
		" sd_read_latency %lu, sd_init_latency %lu, sd_read_gap %lu, sd_write_latency %lu,"
		" iram_fetch_wait %u, ivram_fetch_wait %u, dq_iram_extra %u,"
		" dq_hit %u ---\n",
		model.branch_taken, model.branch_taken_iram, model.iqb_first,
		model.iqb_word_gap, model.dq_extra, model.wr_ticks,
		model.wr_rd_turn, model.row_change_extra, model.sdclk_half,
		model.row_ports, model.dma_extra, model.dma_mem_extra, model.sd_read_latency,
		model.sd_init_latency, model.sd_read_gap, model.sd_write_latency,
		model.iram_fetch_wait, model.ivram_fetch_wait,
		model.dq_iram_extra, model.dq_hit);
}
