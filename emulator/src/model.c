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

struct model model = {
	.branch_taken = 8,          /* refitted 2026-09-13, see README */
	.branch_taken_iram = 4,     /* cpu-loop-a0/ivram/dstram all measure 5.0 */
	.iqb_first = 8,          /* half-MCLK, refitted 2026-09-13 */
	.iqb_word_gap = 0,
	.dq_extra = 1,
	.wr_ticks = 6,
	/* Zero since the row model: three clocks of bus turn stood in for
	   what a copy really pays, which is a row change on every access,
	   and charging both now costs more than the device does. */
	.wr_rd_turn = 0,
	.row_ports = 1,
	.sdclk_half = 4,
	.row_change_extra = 4,
	.dma_extra = 30,
	.sd_read_latency = 60000,
	.iram_fetch_wait = 0,       /* fetch-a0 measured exactly 1.0 cycle */
	.ivram_fetch_wait = 1,      /* ...but ubench measures 1.9 from IVRAM */
	.iq_row_evict = 1,          /* measured: a page crossing costs 3.2x */
	.dq_iram_extra = 2,
	.dq_hit = 2,
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
	{ "wr_rd_turn", NULL, &model.wr_rd_turn },
	{ "row_ports", NULL, &model.row_ports },
	{ "sdclk_half", NULL, &model.sdclk_half },
	{ "row_change_extra", NULL, &model.row_change_extra },
	{ "dq_iram_extra", NULL, &model.dq_iram_extra },
	{ "dq_hit", NULL, &model.dq_hit },
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
