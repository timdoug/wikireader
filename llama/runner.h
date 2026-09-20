/* The generation loop, shared by the host and device builds.
 * GPL-3.0-or-later.
 */

#ifndef WR_LLAMA_RUNNER_H
#define WR_LLAMA_RUNNER_H

#include "model.h"
#include "tokenizer.h"

/* Why generation ended.  The app reports this: a run that stops because
   the caller asked for forty tokens looks exactly like one that stopped
   because something broke, and the difference is the first thing anyone
   wants to know. */
typedef enum {
	LLAMA_STOP_END_OF_TEXT = 0,	/* the model finished the story */
	LLAMA_STOP_TOKEN_LIMIT,		/* hit the -n given on the card */
	LLAMA_STOP_CONTEXT_FULL,	/* reached the checkpoint's seq_len */
} llama_stop_reason;

typedef struct {
	/* Called with each decoded piece as it is produced, so the device
	   can paint a word at a time rather than after the whole story. */
	void (*emit)(void *ctx, const char *piece);
	void *ctx;
	int steps;		/* tokens to generate; 0 for no limit */
	const char *prompt;	/* NULL for an unprompted story */
} llama_run_options;

typedef struct {
	int tokens;		/* tokens generated, prompt included */
	uint32_t macs;		/* int8 multiply-accumulates performed */
	uint32_t forward_us;	/* time inside llama_forward */
	uint32_t total_us;
	llama_stop_reason stop;
} llama_run_stats;

const char *llama_stop_text(llama_stop_reason r);

/* Microseconds from an arbitrary epoch; each build supplies its own. */
uint32_t llama_now_us(void);

/* Greedy (argmax) decoding.  A sampler with a temperature would need a
   softmax over the whole vocabulary -- 512 calls to expf a token here, and
   32000 on a full-vocabulary model, each one a run of soft-float. */
int llama_run(llama_model *m, llama_tokenizer *tok,
	      const llama_run_options *opt, llama_run_stats *stats);

#endif
