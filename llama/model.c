/* Int8 Llama 2 forward pass, GPL-3.0-or-later.
 *
 * Structurally this is llama2.c's run.c.  What changed, and why:
 *
 *   - every weight is int8 with one fp32 scale per output row, because a
 *     soft-float multiply-accumulate costs around 250 cycles on this part
 *     and an integer one about twelve;
 *   - activations are quantized to int8 before each matmul, so the inner
 *     loop is `mlt.h` on two bytes rather than a call into __mulsf3;
 *   - the RoPE tables come from the weight file: run.c calls powf, cosf
 *     and sinf per token and there is no libm here.
 *
 * Everything outside the matmuls -- rmsnorm, softmax, SwiGLU, the residual
 * adds -- is still fp32.  It is O(dim) rather than O(weights), and leaving
 * it alone keeps this comparable against upstream while the profiler says
 * where the time really goes.
 */

#include <string.h>

#include "fmath.h"
#include "model.h"

/* ---- the weight image ------------------------------------------------ */

/* Header, little-endian, 64 bytes.  Matches llama/tools/convert.py. */
enum {
	HDR_MAGIC = 0,
	HDR_VERSION = 4,
	HDR_DIM = 8,
	HDR_HIDDEN = 12,
	HDR_LAYERS = 16,
	HDR_HEADS = 20,
	HDR_KV_HEADS = 24,
	HDR_VOCAB = 28,
	HDR_SEQ = 32,
	HDR_SHARED = 36,
	HDR_BYTES = 64,
};

static uint32_t rd32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const char *llama_strerror(llama_status s)
{
	switch (s) {
	case LLAMA_OK:            return "ok";
	case LLAMA_ERR_OPEN:      return "cannot open weights";
	case LLAMA_ERR_READ:      return "short read on weights";
	case LLAMA_ERR_MAGIC:     return "not a WRL1 weight file";
	case LLAMA_ERR_VERSION:   return "unsupported weight file version";
	case LLAMA_ERR_TRUNCATED: return "weight file is truncated";
	case LLAMA_ERR_SHAPE:     return "weight file shapes are inconsistent";
	case LLAMA_ERR_MEMORY:    return "out of memory";
	}
	return "unknown error";
}

/* A cursor over the image that refuses to hand out more than is there.  A
   truncated file otherwise reads as garbage weights and generates plausible
   nonsense, which is the hardest kind of failure to recognize. */
typedef struct {
	const unsigned char *base;
	size_t offset, bytes;
	int overrun;
} cursor;

static const void *take(cursor *c, size_t n)
{
	const void *p;

	if (c->overrun || n > c->bytes - c->offset) {
		c->overrun = 1;
		return NULL;
	}
	p = c->base + c->offset;
	c->offset += n;
	return p;
}

static const float *take_f32(cursor *c, size_t count)
{
	return (const float *)take(c, count * sizeof(float));
}

/* One quantized tensor: d rows of n weights, then d scales. */
static void take_q(cursor *c, llama_qtensor *t, size_t d, size_t n)
{
	t->q = (const int8_t *)take(c, d * n);
	t->s = take_f32(c, d);
}

llama_status llama_open(llama_model *m, void *image, size_t bytes,
			const char *tag)
{
	const unsigned char *p = image;
	llama_config *cfg = &m->cfg;
	cursor c;
	int l;

	memset(m, 0, sizeof *m);
	m->blob = image;
	m->blob_bytes = bytes;

	if (bytes < HDR_BYTES)
		return LLAMA_ERR_TRUNCATED;
	if (memcmp(p + HDR_MAGIC, LLAMA_MAGIC, 4) != 0)
		return LLAMA_ERR_MAGIC;
	if (rd32(p + HDR_VERSION) != LLAMA_VERSION)
		return LLAMA_ERR_VERSION;

	cfg->dim = (int)rd32(p + HDR_DIM);
	cfg->hidden_dim = (int)rd32(p + HDR_HIDDEN);
	cfg->n_layers = (int)rd32(p + HDR_LAYERS);
	cfg->n_heads = (int)rd32(p + HDR_HEADS);
	cfg->n_kv_heads = (int)rd32(p + HDR_KV_HEADS);
	cfg->vocab_size = (int)rd32(p + HDR_VOCAB);
	cfg->seq_len = (int)rd32(p + HDR_SEQ);
	cfg->shared_classifier = (int)rd32(p + HDR_SHARED);

	if (cfg->dim <= 0 || cfg->hidden_dim <= 0 || cfg->n_layers <= 0 ||
	    cfg->n_heads <= 0 || cfg->n_kv_heads <= 0 ||
	    cfg->vocab_size <= 0 || cfg->seq_len <= 0)
		return LLAMA_ERR_SHAPE;
	if (cfg->dim % cfg->n_heads || cfg->n_heads % cfg->n_kv_heads)
		return LLAMA_ERR_SHAPE;

	cfg->head_size = cfg->dim / cfg->n_heads;
	cfg->kv_dim = cfg->head_size * cfg->n_kv_heads;
	cfg->kv_mul = cfg->n_heads / cfg->n_kv_heads;
	if (cfg->head_size % 2)
		return LLAMA_ERR_SHAPE;

	/* The int32 accumulator has to hold a whole row: the largest
	   possible magnitude is 127 * 127 * n.  That allows rows of about
	   133 thousand, far past anything these checkpoints use, but a
	   damaged header could claim one. */
	if ((uint32_t)cfg->hidden_dim > 0x7fffffffu / (127u * 127u) ||
	    (uint32_t)cfg->dim > 0x7fffffffu / (127u * 127u))
		return LLAMA_ERR_SHAPE;

	m->layers = llama_alloc((size_t)cfg->n_layers * sizeof *m->layers, tag);
	if (!m->layers)
		return LLAMA_ERR_MEMORY;

	c.base = p;
	c.offset = HDR_BYTES;
	c.bytes = bytes;
	c.overrun = 0;

	for (l = 0; l < cfg->n_layers; l++)
		m->layers[l].rms_att = take_f32(&c, (size_t)cfg->dim);
	for (l = 0; l < cfg->n_layers; l++)
		m->layers[l].rms_ffn = take_f32(&c, (size_t)cfg->dim);
	m->rms_final = take_f32(&c, (size_t)cfg->dim);
	m->rope = take_f32(&c, (size_t)cfg->seq_len * cfg->head_size);

	take_q(&c, &m->embed, (size_t)cfg->vocab_size, (size_t)cfg->dim);
	for (l = 0; l < cfg->n_layers; l++) {
		llama_layer *ly = &m->layers[l];
		size_t dim = cfg->dim, kv = cfg->kv_dim, hid = cfg->hidden_dim;

		take_q(&c, &ly->wq, dim, dim);
		take_q(&c, &ly->wk, kv, dim);
		take_q(&c, &ly->wv, kv, dim);
		take_q(&c, &ly->wo, dim, dim);
		take_q(&c, &ly->w1, hid, dim);
		take_q(&c, &ly->w2, dim, hid);
		take_q(&c, &ly->w3, hid, dim);
	}
	if (cfg->shared_classifier)
		m->wcls = m->embed;
	else
		take_q(&c, &m->wcls, (size_t)cfg->vocab_size, (size_t)cfg->dim);

	if (c.overrun)
		return LLAMA_ERR_TRUNCATED;

	/* Activations.  One allocation per buffer keeps the tags readable in
	   the heap dump; they are all small next to the weights. */
	{
		size_t dim = (size_t)cfg->dim;
		size_t hid = (size_t)cfg->hidden_dim;
		size_t kvc = (size_t)cfg->n_layers * cfg->seq_len * cfg->kv_dim;

		m->x = llama_alloc(dim * sizeof(float), tag);
		m->xb = llama_alloc(dim * sizeof(float), tag);
		m->xb2 = llama_alloc(dim * sizeof(float), tag);
		m->hb = llama_alloc(hid * sizeof(float), tag);
		m->hb2 = llama_alloc(hid * sizeof(float), tag);
		m->q = llama_alloc(dim * sizeof(float), tag);
		m->att = llama_alloc((size_t)cfg->n_heads * cfg->seq_len *
				     sizeof(float), tag);
		m->logits = llama_alloc((size_t)cfg->vocab_size *
					sizeof(float), tag);
		/* The two quantized activation vectors are the only buffers
		   the matmul reads over and over, and the only ones small
		   enough to fit internal RAM.  See llama_alloc_fast. */
		m->xq = llama_alloc_fast(dim);
		m->xq_is_fast = m->xq != NULL;
		if (!m->xq)
			m->xq = llama_alloc(dim, tag);
		m->hq = llama_alloc_fast(hid);
		m->hq_is_fast = m->hq != NULL;
		if (!m->hq)
			m->hq = llama_alloc(hid, tag);
		m->key_cache = llama_alloc(kvc * sizeof(float), tag);
		m->value_cache = llama_alloc(kvc * sizeof(float), tag);

		if (!m->x || !m->xb || !m->xb2 || !m->hb || !m->hb2 ||
		    !m->q || !m->att || !m->logits || !m->xq || !m->hq ||
		    !m->key_cache || !m->value_cache)
			return LLAMA_ERR_MEMORY;
	}
	return LLAMA_OK;
}

void llama_free(llama_model *m, const char *tag)
{
	llama_release(m->x, tag);
	llama_release(m->xb, tag);
	llama_release(m->xb2, tag);
	llama_release(m->hb, tag);
	llama_release(m->hb2, tag);
	llama_release(m->q, tag);
	llama_release(m->att, tag);
	llama_release(m->logits, tag);
	/* The fast ones live in a static arena and are not freed. */
	if (!m->xq_is_fast)
		llama_release(m->xq, tag);
	if (!m->hq_is_fast)
		llama_release(m->hq, tag);
	llama_release(m->key_cache, tag);
	llama_release(m->value_cache, tag);
	llama_release(m->layers, tag);
	llama_release(m->blob, tag);
	memset(m, 0, sizeof *m);
}

/* ---- kernels --------------------------------------------------------- */

void rmsnorm(float *out, const float *x, const float *weight, int n)
{
	float ss = 0.0f;
	int i;

	for (i = 0; i < n; i++)
		ss += x[i] * x[i];
	ss = wr_rsqrtf(ss / (float)n + 1e-5f);
	for (i = 0; i < n; i++)
		out[i] = weight[i] * (ss * x[i]);
}

void softmax(float *x, int n)
{
	float max_val = x[0], sum = 0.0f, inv;
	int i;

	for (i = 1; i < n; i++)
		if (x[i] > max_val)
			max_val = x[i];
	for (i = 0; i < n; i++) {
		x[i] = wr_expf(x[i] - max_val);
		sum += x[i];
	}
	/* One divide, not n: division is a libgcc call on this part. */
	inv = 1.0f / sum;
	for (i = 0; i < n; i++)
		x[i] *= inv;
}

/* Quantize a vector to int8 with a single scale.  Per-vector rather than
   per-group: the vector is dim or hidden_dim long, one pass finds its
   magnitude, and the matmul then folds the one scale into the row scale it
   is already multiplying by. */
float quantize(int8_t *q, const float *x, int n)
{
	float max_abs = 0.0f, scale, inv;
	int i;

	for (i = 0; i < n; i++) {
		float a = x[i] < 0.0f ? -x[i] : x[i];
		if (a > max_abs)
			max_abs = a;
	}
	/* An all-zero vector quantizes to zeros under any scale. */
	scale = max_abs == 0.0f ? 1.0f : max_abs * (1.0f / 127.0f);
	inv = 1.0f / scale;
	for (i = 0; i < n; i++) {
		/* Round half away from zero without calling roundf. */
		float v = x[i] * inv;
		int r = (int)(v + (v < 0.0f ? -0.5f : 0.5f));
		if (r > 127)
			r = 127;
		else if (r < -127)
			r = -127;
		q[i] = (int8_t)r;
	}
	return scale;
}

/* xout[i] = ws[i] * xs * sum_j w[i][j] * xq[j]
 *
 * This is the whole cost of the model: every weight is read once here and
 * nowhere else.  The accumulator is int32 and cannot overflow for any row
 * length llama_open accepts.
 */
void matmul(float *xout, const int8_t *xq, float xs,
		   const llama_qtensor *w, int n, int d)
{
	int i;

	for (i = 0; i < d; i++) {
		const int8_t *row = w->q + (size_t)i * n;
		int32_t acc = 0;
		int j;

		for (j = 0; j < n; j++)
			acc += (int32_t)row[j] * (int32_t)xq[j];
		xout[i] = (float)acc * (w->s[i] * xs);
	}
}

uint32_t llama_macs(const llama_model *m, int pos)
{
	const llama_config *c = &m->cfg;
	uint32_t per_layer, attn;

	per_layer = (uint32_t)c->dim * c->dim		/* wq */
		  + (uint32_t)c->kv_dim * c->dim * 2	/* wk, wv */
		  + (uint32_t)c->dim * c->dim		/* wo */
		  + (uint32_t)c->hidden_dim * c->dim * 2 /* w1, w3 */
		  + (uint32_t)c->dim * c->hidden_dim;	/* w2 */
	/* Scores and the weighted sum of values, both over pos + 1 steps. */
	attn = 2u * (uint32_t)c->n_heads * (pos + 1) * c->head_size;
	return (uint32_t)c->n_layers * (per_layer + attn) +
	       (uint32_t)c->dim * c->vocab_size;
}

/* ---- forward --------------------------------------------------------- */

float *llama_forward(llama_model *m, int token, int pos)
{
	const llama_config *c = &m->cfg;
	const int dim = c->dim, kv_dim = c->kv_dim, hidden_dim = c->hidden_dim;
	const int head_size = c->head_size, kv_mul = c->kv_mul;
	float *x = m->x;
	float xs, hs;
	int i, l, h, t;

	/* Dequantize the token's embedding row. */
	{
		const int8_t *row = m->embed.q + (size_t)token * dim;
		float s = m->embed.s[token];

		for (i = 0; i < dim; i++)
			x[i] = (float)row[i] * s;
	}

	for (l = 0; l < c->n_layers; l++) {
		const llama_layer *ly = &m->layers[l];
		size_t loff = (size_t)l * c->seq_len * kv_dim;
		float *k = m->key_cache + loff + (size_t)pos * kv_dim;
		float *v = m->value_cache + loff + (size_t)pos * kv_dim;

		rmsnorm(m->xb, x, ly->rms_att, dim);
		xs = quantize(m->xq, m->xb, dim);
		matmul(m->q, m->xq, xs, &ly->wq, dim, dim);
		matmul(k, m->xq, xs, &ly->wk, dim, kv_dim);
		matmul(v, m->xq, xs, &ly->wv, dim, kv_dim);

		/* RoPE, from the table rather than from powf/cosf/sinf.  The
		   rotation for dimension i depends on i only through
		   i % head_size, so one row of head_size/2 pairs serves every
		   head at this position. */
		{
			const float *rot = m->rope +
					   (size_t)pos * head_size;

			for (i = 0; i < dim; i += 2) {
				int pair = (i % head_size) >> 1;
				float fcr = rot[pair * 2];
				float fci = rot[pair * 2 + 1];
				float v0 = m->q[i], v1 = m->q[i + 1];

				m->q[i] = v0 * fcr - v1 * fci;
				m->q[i + 1] = v0 * fci + v1 * fcr;
				if (i < kv_dim) {
					v0 = k[i];
					v1 = k[i + 1];
					k[i] = v0 * fcr - v1 * fci;
					k[i + 1] = v0 * fci + v1 * fcr;
				}
			}
		}

		for (h = 0; h < c->n_heads; h++) {
			const float *q = m->q + h * head_size;
			float *att = m->att + (size_t)h * c->seq_len;
			float *xb = m->xb + h * head_size;
			size_t hoff = loff + (size_t)(h / kv_mul) * head_size;
			/* head_size is fixed for a checkpoint, so its inverse
			   square root is a constant, not a call per score. */
			float inv_root = wr_rsqrtf((float)head_size);

			for (t = 0; t <= pos; t++) {
				const float *kt = m->key_cache + hoff +
						  (size_t)t * kv_dim;
				float score = 0.0f;

				for (i = 0; i < head_size; i++)
					score += q[i] * kt[i];
				att[t] = score * inv_root;
			}

			softmax(att, pos + 1);

			memset(xb, 0, (size_t)head_size * sizeof(float));
			for (t = 0; t <= pos; t++) {
				const float *vt = m->value_cache + hoff +
						  (size_t)t * kv_dim;
				float a = att[t];

				for (i = 0; i < head_size; i++)
					xb[i] += a * vt[i];
			}
		}

		xs = quantize(m->xq, m->xb, dim);
		matmul(m->xb2, m->xq, xs, &ly->wo, dim, dim);
		for (i = 0; i < dim; i++)
			x[i] += m->xb2[i];

		rmsnorm(m->xb, x, ly->rms_ffn, dim);
		xs = quantize(m->xq, m->xb, dim);
		matmul(m->hb, m->xq, xs, &ly->w1, dim, hidden_dim);
		matmul(m->hb2, m->xq, xs, &ly->w3, dim, hidden_dim);

		/* SwiGLU: silu(w1 x) * (w3 x), silu(v) = v * sigmoid(v). */
		for (i = 0; i < hidden_dim; i++) {
			float val = m->hb[i];

			val *= 1.0f / (1.0f + wr_expf(-val));
			m->hb[i] = val * m->hb2[i];
		}

		hs = quantize(m->hq, m->hb, hidden_dim);
		matmul(m->xb, m->hq, hs, &ly->w2, hidden_dim, dim);
		for (i = 0; i < dim; i++)
			x[i] += m->xb[i];
	}

	rmsnorm(x, x, m->rms_final, dim);
	xs = quantize(m->xq, x, dim);
	matmul(m->logits, m->xq, xs, &m->wcls, dim, c->vocab_size);
	return m->logits;
}
