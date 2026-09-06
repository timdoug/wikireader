#include <stdlib.h>
#include <string.h>

#include "model.h"

/*
 * Fitted on 2026-09-05 with tools/fit_model.py against a WikiReader's
 * bench.txt (zim/bench-device-2026-09-05.txt, a 32 MB early board with the
 * retimed SDRAM controller); every micro-benchmark then agrees with the
 * device within 10%.  The manual-only values are 3, 0, 0, 0, 0, 0, 0.
 */
struct model model = {
	.branch_taken = 5,
	.iqb_first = 3,
	.iqb_word_gap = 2,
	.dq_extra = 2,
	.wr_ticks = 1,
	.dma_extra = 27,
	.sd_read_latency = 70000,
};

static const struct {
	const char *name;
	unsigned long *ul;
	unsigned *u;
} fields[] = {
	{ "branch_taken", NULL, &model.branch_taken },
	{ "iqb_first", NULL, &model.iqb_first },
	{ "iqb_word_gap", NULL, &model.iqb_word_gap },
	{ "dq_extra", NULL, &model.dq_extra },
	{ "wr_ticks", NULL, &model.wr_ticks },
	{ "dma_extra", NULL, &model.dma_extra },
	{ "sd_read_latency", &model.sd_read_latency, NULL },
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
	fprintf(out, "--- model: branch_taken %u, iqb_first %u, iqb_word_gap %u,"
		" dq_extra %u, wr_ticks %u, dma_extra %u, sd_read_latency %lu ---\n",
		model.branch_taken, model.iqb_first, model.iqb_word_gap,
		model.dq_extra, model.wr_ticks, model.dma_extra,
		model.sd_read_latency);
}
