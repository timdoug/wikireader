/* The generation loop, shared by the host and device builds.
 * GPL-3.0-or-later.
 */

#ifndef WR_LLAMA_RUNNER_H
#define WR_LLAMA_RUNNER_H

#include "model.h"
#include "tokenizer.h"

typedef struct {
	/* Called with each decoded piece as it is produced, so the device
	   can paint a word at a time rather than after the whole story. */
	void (*emit)(void *ctx, const char *piece);
	void *ctx;
	int steps;		/* tokens to generate, including the prompt */
	const char *prompt;	/* NULL for an unprompted story */
} llama_run_options;

typedef struct {
	int tokens;		/* tokens generated, prompt included */
	uint32_t macs;		/* int8 multiply-accumulates performed */
	uint32_t forward_us;	/* time inside llama_forward */
	uint32_t total_us;
} llama_run_stats;

/* Microseconds from an arbitrary epoch; each build supplies its own. */
uint32_t llama_now_us(void);

/* Greedy (argmax) decoding.  A sampler with a temperature would need a
   softmax over the whole vocabulary -- 512 calls to expf a token here, and
   32000 on a full-vocabulary model, each one a run of soft-float. */
int llama_run(llama_model *m, llama_tokenizer *tok,
	      const llama_run_options *opt, llama_run_stats *stats);

#endif
