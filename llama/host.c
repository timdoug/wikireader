/* Host driver, GPL-3.0-or-later.
 *
 * Builds the same model.c, tokenizer.c and runner.c the device runs, so
 * that a change can be checked in a second here rather than in a minute in
 * the emulator.  The device build differs only in these four functions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "runner.h"

void *llama_alloc(size_t bytes, const char *tag)
{
	(void)tag;
	return malloc(bytes ? bytes : 1);
}

void llama_release(void *p, const char *tag)
{
	(void)tag;
	free(p);
}

/* The host has no internal RAM to be fast about, and no SDRAM rows to
   thrash; the caller falls back to llama_alloc. */
void *llama_alloc_fast(size_t bytes)
{
	(void)bytes;
	return NULL;
}

uint32_t llama_now_us(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (uint32_t)tv.tv_sec * 1000000u + (uint32_t)tv.tv_usec;
}

/* One byte past the end is left readable and writable: the tokenizer
   terminates its last piece there. */
static void *read_file(const char *path, size_t *bytes)
{
	FILE *f = fopen(path, "rb");
	long size;
	void *buf;

	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
	    fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)size + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	((unsigned char *)buf)[size] = 0;
	*bytes = (size_t)size;
	return buf;
}

static void emit(void *ctx, const char *piece)
{
	(void)ctx;
	fputs(piece, stdout);
	fflush(stdout);
}

/* Teacher-forced top-1 predictions.
 *
 * Free-running generation is a bad accuracy measure: greedy decoding turns
 * one differing logit into a different token, and from there the two runs
 * write different stories for reasons that have nothing to do with
 * arithmetic.  Feeding a fixed token sequence and reporting what the model
 * would have predicted at each position measures the arithmetic alone, and
 * a reference implementation can be compared against it position by
 * position.
 */
static int teacher(llama_model *model, llama_tokenizer *tok, const char *text)
{
	int *tokens = malloc((size_t)model->cfg.seq_len * sizeof *tokens);
	int n, pos;

	if (!tokens)
		return 1;
	n = llama_encode(tok, text, 1, 0, tokens, model->cfg.seq_len);
	if (n < 1) {
		free(tokens);
		return 1;
	}
	for (pos = 0; pos < n; pos++) {
		int32_t *logits = llama_forward(model, tokens[pos], pos);
		int best = 0, i;

		for (i = 1; i < model->cfg.vocab_size; i++)
			if (logits[i] > logits[best])
				best = i;
		printf("%d %d\n", tokens[pos], best);
	}
	free(tokens);
	return 0;
}

int main(int argc, char **argv)
{
	const char *weights = NULL, *vocab = NULL, *prompt = NULL;
	const char *forced = NULL;
	int steps = 0, quiet = 0, i;
	void *wimage, *timage;
	size_t wbytes, tbytes;
	llama_model model;
	llama_tokenizer tok;
	llama_run_options opt;
	llama_run_stats stats;
	llama_status st;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-z") && i + 1 < argc)
			vocab = argv[++i];
		else if (!strcmp(argv[i], "-n") && i + 1 < argc)
			steps = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-i") && i + 1 < argc)
			prompt = argv[++i];
		else if (!strcmp(argv[i], "-q"))
			quiet = 1;
		else if (!strcmp(argv[i], "-teacher") && i + 1 < argc)
			forced = argv[++i];
		else if (!weights)
			weights = argv[i];
		else {
			fprintf(stderr, "unexpected argument %s\n", argv[i]);
			return 2;
		}
	}
	if (!weights || !vocab) {
		fprintf(stderr,
			"usage: %s <model.wrl> -z <tokenizer.bin> "
			"[-n steps] [-i prompt] [-q] [-teacher text]\n",
			argv[0]);
		return 2;
	}

	wimage = read_file(weights, &wbytes);
	if (!wimage) {
		fprintf(stderr, "cannot read %s\n", weights);
		return 1;
	}
	timage = read_file(vocab, &tbytes);
	if (!timage) {
		fprintf(stderr, "cannot read %s\n", vocab);
		return 1;
	}

	st = llama_open(&model, wimage, wbytes, "llama");
	if (st != LLAMA_OK) {
		fprintf(stderr, "%s: %s\n", weights, llama_strerror(st));
		return 1;
	}
	if (llama_tokenizer_open(&tok, timage, tbytes, model.cfg.vocab_size,
				 "llama") != 0) {
		fprintf(stderr, "%s: bad tokenizer\n", vocab);
		return 1;
	}

	if (forced)
		return teacher(&model, &tok, forced);

	memset(&opt, 0, sizeof opt);
	opt.emit = emit;
	opt.steps = steps;
	opt.prompt = prompt;

	if (llama_run(&model, &tok, &opt, &stats) < 0) {
		fprintf(stderr, "generation failed\n");
		return 1;
	}
	putchar('\n');

	if (!quiet) {
		double secs = stats.total_us / 1e6;

		fprintf(stderr,
			"%d tokens in %.3f s (%.1f tok/s), %u MACs, "
			"%.1f MAC/token; stopped: %s\n",
			stats.tokens, secs,
			secs > 0 ? stats.tokens / secs : 0.0,
			stats.macs,
			stats.tokens ? (double)stats.macs / stats.tokens : 0.0,
			llama_stop_text(stats.stop));
	}

	llama_tokenizer_free(&tok, "llama");
	llama_free(&model, "llama");
	return 0;
}
