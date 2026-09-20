/* Int8 Llama 2 inference for the WikiReader, GPL-3.0-or-later.
 *
 * A port of llama2.c's forward pass with every weight held as int8 and one
 * fp32 scale per output row.  The arithmetic differences from run.c are all
 * forced by the part: there is no FPU, so a float multiply is a call into
 * libgcc, and no MAC instruction, so even the integer product costs a
 * five-cycle `mlt.h` plus a read of ALR.
 */

#ifndef WR_LLAMA_MODEL_H
#define WR_LLAMA_MODEL_H

#include <stddef.h>
#include <stdint.h>

#define LLAMA_MAGIC   "WRL1"
#define LLAMA_VERSION 1

typedef struct {
	int dim;
	int hidden_dim;
	int n_layers;
	int n_heads;
	int n_kv_heads;
	int vocab_size;
	int seq_len;
	/* Derived, so the forward pass does not divide: there is no hardware
	   divide on this core either. */
	int head_size;
	int kv_dim;
	int kv_mul;
	int shared_classifier;
} llama_config;

/* One weight matrix: d rows of n int8 weights, each row with its own scale.
   Upstream's Q8_0 uses fixed groups of 64, which silently drops weights
   when a row length is not a multiple of 64 -- stories260K's hidden_dim is
   172.  A scale per row divides every shape exactly. */
typedef struct {
	const int8_t *q;
	const float *s;
} llama_qtensor;

typedef struct {
	const float *rms_att;
	const float *rms_ffn;
	llama_qtensor wq, wk, wv, wo, w1, w2, w3;
} llama_layer;

typedef struct {
	llama_config cfg;

	void *blob;			/* the whole weight file, one allocation */
	size_t blob_bytes;
	llama_layer *layers;
	llama_qtensor embed;		/* also the classifier, when shared */
	llama_qtensor wcls;
	const float *rms_final;
	const float *rope;		/* [pos][head_size/2][cos, sin] */

	/* Activations. */
	float *x, *xb, *xb2, *hb, *hb2, *q, *att, *logits;
	int8_t *xq, *hq;
	int xq_is_fast, hq_is_fast;
	float *key_cache, *value_cache;
} llama_model;

/* Platform hooks; host.c and llama.c supply them. */
void *llama_alloc(size_t bytes, const char *tag);
void llama_release(void *p, const char *tag);

/* Internal RAM, if the platform has any to spare; NULL if not, and the
   caller falls back to llama_alloc.  This is not a micro-optimization: the
   matmul reads the whole quantized activation vector once per output row,
   interleaved with the weight stream, and two streams in SDRAM re-activate
   a row on *every* load.  Measured at 2.00 row activations per
   multiply-accumulate with both in SDRAM.  Never freed -- the platform
   hands out of a static arena. */
void *llama_alloc_fast(size_t bytes);

typedef enum {
	LLAMA_OK = 0,
	LLAMA_ERR_OPEN,
	LLAMA_ERR_READ,
	LLAMA_ERR_MAGIC,
	LLAMA_ERR_VERSION,
	LLAMA_ERR_TRUNCATED,
	LLAMA_ERR_SHAPE,
	LLAMA_ERR_MEMORY,
} llama_status;

const char *llama_strerror(llama_status s);

/* Bind a model to a weight image already in memory.  The image is adopted:
   llama_free releases it with `tag`.  Activations are allocated here. */
llama_status llama_open(llama_model *m, void *image, size_t bytes,
			const char *tag);
void llama_free(llama_model *m, const char *tag);

/* Run one token at `pos`, returning the logits over the vocabulary. */
float *llama_forward(llama_model *m, int token, int pos);

/* The number of int8 multiply-accumulates llama_forward performs at `pos`.
   Reported alongside timings so a cycles-per-MAC figure falls out. */
uint32_t llama_macs(const llama_model *m, int pos);

#endif
