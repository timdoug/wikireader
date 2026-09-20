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

	/* Sampling temperature, Q8: 256 is 1.0, and 0 means take the
	   argmax.  Greedy decoding on a model this small loops -- it has no
	   way out of a cycle once it enters one -- so a temperature is what
	   makes two runs of the same prompt differ. */
	int temperature_q8;
	unsigned long seed;	/* 0 asks the platform for one */
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

/* Temperature in Q8 from a decimal string: "0", "0.8", "1", "1.25".
   Returns -1 if it is not a number. */
int llama_parse_temperature(const char *text);

/* Generate.  Greedy when temperature_q8 is zero, otherwise sampled from
   the softmax of the logits.  Sampling was out of reach while the forward
   pass was fp32 -- a softmax over the vocabulary meant 512 calls into
   soft-float expf a token -- and costs about 1% now. */
int llama_run(llama_model *m, llama_tokenizer *tok,
	      const llama_run_options *opt, llama_run_stats *stats);

#endif
