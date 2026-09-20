/* The generation loop, GPL-3.0-or-later. */

#include <string.h>

#include "runner.h"

const char *llama_stop_text(llama_stop_reason r)
{
	switch (r) {
	case LLAMA_STOP_END_OF_TEXT: return "end of story";
	case LLAMA_STOP_TOKEN_LIMIT: return "-n token limit";
	case LLAMA_STOP_CONTEXT_FULL: return "context full";
	}
	return "unknown";
}

int llama_run(llama_model *m, llama_tokenizer *tok,
	      const llama_run_options *opt, llama_run_stats *stats)
{
	int *prompt_tokens;
	int n_prompt, steps, pos, token, next = TOK_BOS;
	uint32_t began;

	memset(stats, 0, sizeof *stats);

	/* No limit given means run until the model finishes the story or the
	   context fills, which is what someone who did not ask for a limit
	   is asking for. */
	steps = opt->steps;
	if (steps <= 0 || steps > m->cfg.seq_len) {
		stats->stop = LLAMA_STOP_CONTEXT_FULL;
		steps = m->cfg.seq_len;
	} else {
		stats->stop = LLAMA_STOP_TOKEN_LIMIT;
	}

	prompt_tokens = llama_alloc((size_t)steps * sizeof *prompt_tokens,
				    "llama.prompt");
	if (!prompt_tokens)
		return -1;

	n_prompt = llama_encode(tok, opt->prompt ? opt->prompt : "",
				1, 0, prompt_tokens, steps);
	if (n_prompt < 1) {
		llama_release(prompt_tokens, "llama.prompt");
		return -1;
	}

	began = llama_now_us();
	token = prompt_tokens[0];
	for (pos = 0; pos < steps; pos++) {
		uint32_t t0 = llama_now_us();
		int32_t *logits = llama_forward(m, token, pos);

		stats->forward_us += llama_now_us() - t0;
		stats->macs += llama_macs(m, pos);
		stats->tokens++;

		if (pos + 1 < n_prompt) {
			next = prompt_tokens[pos + 1];
		} else {
			/* Argmax over the logits. */
			int best = 0, i;
			int32_t best_val = logits[0];

			for (i = 1; i < m->cfg.vocab_size; i++)
				if (logits[i] > best_val) {
					best_val = logits[i];
					best = i;
				}
			next = best;
		}

		if (opt->emit) {
			char scratch[8];
			const char *piece = llama_decode(tok, token, next,
							 scratch);

			opt->emit(opt->ctx, piece);
		}

		/* BOS marks the end of a story in these checkpoints. */
		if (next == TOK_BOS || next == TOK_EOS) {
			stats->stop = LLAMA_STOP_END_OF_TEXT;
			break;
		}
		token = next;
	}
	stats->total_us = llama_now_us() - began;

	llama_release(prompt_tokens, "llama.prompt");
	return stats->tokens;
}
