/* The generation loop, GPL-3.0-or-later. */

#include <string.h>

#include "fixed.h"
#include "runner.h"

/* "0.8" -> 205.  strtod would do it, but mini-libc has no strtod and this
   has to parse the same way on both builds or the two disagree about what
   a card says. */
int llama_parse_temperature(const char *text)
{
	int whole = 0, frac = 0, scale = 1, seen = 0;
	const char *p = text;

	while (*p >= '0' && *p <= '9') {
		whole = whole * 10 + (*p++ - '0');
		seen = 1;
		if (whole > 16)		/* past any useful temperature */
			return -1;
	}
	if (*p == '.') {
		p++;
		while (*p >= '0' && *p <= '9' && scale < 1000) {
			frac = frac * 10 + (*p++ - '0');
			scale *= 10;
			seen = 1;
		}
	}
	if (!seen || *p != '\0')
		return -1;
	return whole * 256 + (frac * 256) / scale;
}

/* xorshift32.  The sampler needs a stream of numbers that differ between
   runs, not randomness anyone should rely on; this is three shifts. */
static unsigned long next_random(unsigned long *state)
{
	unsigned long x = *state;

	x ^= (x << 13) & 0xffffffffUL;
	x ^= x >> 17;
	x ^= (x << 5) & 0xffffffffUL;
	*state = x;
	return x;
}

/* Sample a token from the logits at `temperature_q8`.
 *
 * The logits are int32 sharing one exponent; the softmax needs their real
 * spacing, so they are brought to Q12 first, divided by the temperature,
 * and then exponentiated.  Everything stays integer: the sum of up to
 * 32000 Q12 weights is 131 million, well inside an int32, and the choice
 * is a walk over the cumulative weights against one random number.
 */
static int sample(const int32_t *logits, int n, int logits_e,
		  int temperature_q8, unsigned long *rng, int32_t *scratch)
{
	int32_t best = logits[0], sum = 0, threshold;
	int inv_t = (256 * 256) / temperature_q8;
	int i;

	for (i = 1; i < n; i++)
		if (logits[i] > best)
			best = logits[i];

	for (i = 0; i < n; i++) {
		/* Difference from the maximum, in Q12, then divided by the
		   temperature.  Clamped before the multiply so the product
		   cannot overflow: exp of anything below -12 is already zero
		   in Q12, and the clamp is well past that. */
		int32_t d = wr_sshift(logits[i] - best, logits_e - 12);

		if (d < -200000)
			d = -200000;
		d = (d * inv_t) >> 8;
		scratch[i] = wr_exp_q12(d);
		sum += scratch[i];
	}

	if (sum <= 0)			/* every weight underflowed */
		return 0;

	threshold = (int32_t)(next_random(rng) % (unsigned long)sum);
	for (i = 0; i < n - 1; i++) {
		threshold -= scratch[i];
		if (threshold < 0)
			return i;
	}
	return n - 1;
}

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
	unsigned long rng = opt->seed ? opt->seed : llama_now_us();

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
		} else if (opt->temperature_q8 > 0) {
			next = sample(logits, m->cfg.vocab_size, m->logits_e,
				      opt->temperature_q8, &rng, m->scratch);
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
