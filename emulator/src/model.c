#include <stdlib.h>
#include <string.h>

#include "model.h"

/*
 * Fitted on 2026-09-05 and 06 with tools/fit_model.py against a
 * WikiReader's bench.txt (zim/bench-device-2026-09-06.txt, a 32 MB early
 * board with the retimed SDRAM controller); every micro-benchmark then
 * agrees with the device within 12%, most within 5%.  The manual-only
 * values are branch costs of 3 and every overhead 0.
 */
/* The PC of the instruction being executed, for the SDRAM row trace
 * (sdramc.c); kept here because every test links the model. */
uint32_t wremu_cur_pc;

struct model model = {
	.branch_taken = 5,
	.branch_taken_iram = 4,     /* cpu-loop-a0: 5.0 against 6.0 from SDRAM */
	.iqb_first = 3,
	.iqb_word_gap = 2,
	.dq_extra = 2,
	.wr_ticks = 1,
	.wr_rd_turn = 6,            /* copy-batch8 and pair-write-read */
	.dma_extra = 28,
	.sd_read_latency = 70000,
	.iram_fetch_wait = 0,       /* fetch-a0 measured exactly 1.0 cycle */
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
	{ "sd_read_latency", &model.sd_read_latency, NULL },
	{ "iram_fetch_wait", NULL, &model.iram_fetch_wait },
	{ "wr_rd_turn", NULL, &model.wr_rd_turn },
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
		" dq_extra %u, wr_ticks %u, wr_rd_turn %u, dma_extra %u,"
		" sd_read_latency %lu, iram_fetch_wait %u ---\n",
		model.branch_taken, model.branch_taken_iram, model.iqb_first,
		model.iqb_word_gap, model.dq_extra, model.wr_ticks,
		model.wr_rd_turn, model.dma_extra, model.sd_read_latency,
		model.iram_fetch_wait);
}
