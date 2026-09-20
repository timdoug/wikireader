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

#include "fixed.h"
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
	case LLAMA_ERR_MAGIC:     return "not a " LLAMA_MAGIC " weight file";
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

/* A run of int16 mantissas followed by their shared int32 exponent.  The
   file is little-endian and so is the part, so these are read in place
   rather than copied; convert.py keeps every count even and every row
   length a multiple of four, which is what keeps the alignment right. */
static void take_scaled(cursor *c, llama_scaled *s, size_t count)
{
	const int32_t *e;

	s->m = (const int16_t *)take(c, count * sizeof(int16_t));
	e = (const int32_t *)take(c, sizeof(int32_t));
	s->e = e ? *e : 0;
}

/* One quantized tensor: d rows of n weights, then the d row scales. */
static void take_q(cursor *c, llama_qtensor *t, size_t d, size_t n)
{
	t->q = (const int8_t *)take(c, d * n);
	take_scaled(c, &t->s, d);
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
		take_scaled(&c, &m->layers[l].rms_att, (size_t)cfg->dim);
	for (l = 0; l < cfg->n_layers; l++)
		take_scaled(&c, &m->layers[l].rms_ffn, (size_t)cfg->dim);
	take_scaled(&c, &m->rms_final, (size_t)cfg->dim);
	take_scaled(&c, &m->rope,
		    (size_t)cfg->seq_len * cfg->head_size);

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

	/* Activations.  Every one is integer; the only float left anywhere
	   in the forward pass is the one that is not there. */
	{
		size_t dim = (size_t)cfg->dim;
		size_t hid = (size_t)cfg->hidden_dim;
		/* Wide enough for the sampler too: it exponentiates a whole
		   vocabulary into this. */
		size_t wide = dim > hid ? dim : hid;

		if ((size_t)cfg->vocab_size > wide)
			wide = (size_t)cfg->vocab_size;
		size_t kvc = (size_t)cfg->n_layers * cfg->seq_len * cfg->kv_dim;
		size_t kve = (size_t)cfg->n_layers * cfg->seq_len;

		m->x = llama_alloc(dim * sizeof(int32_t), tag);
		m->xb = llama_alloc(dim * sizeof(int32_t), tag);
		m->q = llama_alloc(dim * sizeof(int32_t), tag);
		m->hb = llama_alloc(hid * sizeof(int32_t), tag);
		m->hb2 = llama_alloc(hid * sizeof(int32_t), tag);
		m->scratch = llama_alloc(wide * sizeof(int32_t), tag);
		m->att = llama_alloc((size_t)cfg->seq_len * sizeof(int32_t),
				     tag);
		m->logits = llama_alloc((size_t)cfg->vocab_size *
					sizeof(int32_t), tag);
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
		/* int8 with an exponent a position, a quarter of fp32. */
		m->key_cache = llama_alloc(kvc, tag);
		m->value_cache = llama_alloc(kvc, tag);
		m->key_exp = llama_alloc(kve * sizeof(int), tag);
		m->value_exp = llama_alloc(kve * sizeof(int), tag);

		if (!m->x || !m->xb || !m->q || !m->hb || !m->hb2 ||
		    !m->scratch || !m->att || !m->logits || !m->xq ||
		    !m->hq || !m->key_cache || !m->value_cache ||
		    !m->key_exp || !m->value_exp)
			return LLAMA_ERR_MEMORY;
	}

	/* 1/sqrt(head_size) in Q12: isqrt(h << 20) is sqrt(h) * 2^10. */
	m->inv_root_head = (int32_t)(((uint32_t)Q12_ONE << 10) /
				     wr_isqrt64((uint64_t)cfg->head_size << 20));

	return LLAMA_OK;
}

void llama_free(llama_model *m, const char *tag)
{
	llama_release(m->x, tag);
	llama_release(m->xb, tag);
	llama_release(m->q, tag);
	llama_release(m->hb, tag);
	llama_release(m->hb2, tag);
	llama_release(m->scratch, tag);
	llama_release(m->att, tag);
	llama_release(m->logits, tag);
	/* The fast ones live in a static arena and are not freed. */
	if (!m->xq_is_fast)
		llama_release(m->xq, tag);
	if (!m->hq_is_fast)
		llama_release(m->hq, tag);
	llama_release(m->key_cache, tag);
	llama_release(m->value_cache, tag);
	llama_release(m->key_exp, tag);
	llama_release(m->value_exp, tag);
	llama_release(m->layers, tag);
	llama_release(m->blob, tag);
	memset(m, 0, sizeof *m);
}

/* ---- kernels --------------------------------------------------------- */

/* The magnitude of the largest element, as a bit position. */
static int vec_bits(const int32_t *v, int n)
{
	uint32_t maxa = 0;
	int i;

	for (i = 0; i < n; i++) {
		uint32_t a = (uint32_t)(v[i] < 0 ? -v[i] : v[i]);

		if (a > maxa)
			maxa = a;
	}
	return wr_ilog2(maxa);
}

int renormalize(int32_t *v, int n, int e)
{
	int bits = vec_bits(v, n), i, up;

	if (bits < 0 || bits >= 28)
		return e;
	up = 28 - bits;
	for (i = 0; i < n; i++)
		v[i] = (int32_t)((uint32_t)v[i] << up);
	return e + up;
}

/* Bring a vector down to `keep` significant bits, returning the shift
   applied.  The caller adds it to the vector's exponent.  Rounding is done
   on the already-shifted value so the addition cannot overflow. */
int reduce_to(int32_t *v, int n, int keep)
{
	int bits = vec_bits(v, n);
	int sh = bits - keep, i;

	if (sh <= 0)
		return 0;
	for (i = 0; i < n; i++)
		v[i] = ((v[i] >> (sh - 1)) + 1) >> 1;
	return sh;
}

/* value[i] = v[i] * 2^-e  ->  q[i] * 2^-(returned exponent), |q| <= 127. */
int normalize_i8(int8_t *q, const int32_t *v, int n, int e)
{
	int bits = vec_bits(v, n);
	int sh = bits - 6, i;

	if (bits < 0) {			/* the vector is all zeros */
		memset(q, 0, (size_t)n);
		return e;
	}
	if (sh < 0)
		sh = 0;
	for (i = 0; i < n; i++) {
		int32_t r = sh ? (((v[i] >> (sh - 1)) + 1) >> 1) : v[i];

		if (r > 127)
			r = 127;
		else if (r < -127)
			r = -127;
		q[i] = (int8_t)r;
	}
	return e - sh;
}

/* out[i] = sum_j w[i][j] * xq[j], with the row scale applied.
 *
 * This is the whole cost of the model: every weight is read once here and
 * nowhere else.  The dot product is int32 -- 127 * 127 * n for any row
 * length llama_open accepts -- and the accumulator is then cut to fifteen
 * bits before meeting the fifteen-bit row mantissa, because a widening
 * multiply compiles to a call to __muldi3 on this part and that alone
 * measured 5% of the forward pass.
 */
void matmul(int32_t *out, int *out_e, const int8_t *xq, int xq_e,
	    const llama_qtensor *w, int n, int d)
{
	int i, sa;

	for (i = 0; i < d; i++) {
		const int8_t *row = w->q + (size_t)i * n;
		int32_t acc = 0;
		int j;

		for (j = 0; j < n; j++)
			acc += (int32_t)row[j] * (int32_t)xq[j];
		out[i] = acc;
	}

	sa = reduce_to(out, d, 14);
	for (i = 0; i < d; i++)
		out[i] *= w->s.m[i];
	*out_e = xq_e + w->s.e - sa;
}

/* out = (x / rms(x)) * weight, delivered as int8 with an exponent.
 *
 * The input exponent does not appear in the answer: normalising divides it
 * out, which is the one place the arithmetic is easier in fixed point than
 * in float.  What does appear is sqrt(n)/sqrt(sum x^2), the only
 * non-power-of-two scale in the whole forward pass, and it is multiplied
 * into the data here so that everything downstream carries an exponent
 * alone.
 */
int rmsnorm_i8(int8_t *q, int32_t *scratch, const int32_t *x, int n,
		      const llama_scaled *weight)
{
	uint64_t ss = 0;
	uint32_t root, mant;
	int i, sh, bits, sp, ratio_e;

	/* Squares have to fit: cut x to fifteen bits first, so each square
	   is thirty and the sum of up to a few thousand still fits the
	   64-bit accumulator gcc inlines here. */
	for (i = 0; i < n; i++)
		scratch[i] = x[i];
	reduce_to(scratch, n, 15);
	for (i = 0; i < n; i++)
		ss += (uint64_t)(uint32_t)(scratch[i] * scratch[i]);

	if (ss == 0) {
		memset(q, 0, (size_t)n);
		return 0;
	}

	/* ratio = sqrt(n) / sqrt(ss), as a mantissa in [2^14, 2^15) and an
	   exponent.  One 32-bit divide per call, 2 * n_layers + 1 times a
	   token; a 64-bit one would be a call to __udivdi3. */
	root = wr_isqrt64(ss);
	if (root == 0)
		root = 1;
	mant = ((uint32_t)wr_isqrt64((uint64_t)n << 20) << 15) / root;
	if (mant == 0)
		mant = 1;
	bits = wr_ilog2(mant);
	ratio_e = 25;
	if (bits > 14) {
		mant >>= bits - 14;
		ratio_e -= bits - 14;
	} else if (bits < 14) {
		mant <<= 14 - bits;
		ratio_e += 14 - bits;
	}

	/* p = x * weight, then fold the ratio in.  Both operands are cut to
	   fifteen bits before each multiply so nothing widens. */
	for (i = 0; i < n; i++)
		scratch[i] *= weight->m[i];
	sp = reduce_to(scratch, n, 14);
	for (i = 0; i < n; i++)
		scratch[i] *= (int32_t)mant;

	sh = ratio_e + weight->e - sp;
	return normalize_i8(q, scratch, n, sh);
}

/* Rotate q and k in place. The table is int16, so the vector is cut to
   fourteen bits first: each product is then 29 bits and their difference
   fits without widening. */
int rope(int32_t *v, int n, int v_e, const llama_scaled *rope,
		int pos, int head_size)
{
	const int16_t *rot = rope->m + (size_t)pos * head_size;
	int sh = reduce_to(v, n, 14);
	int i;

	for (i = 0; i < n; i += 2) {
		int pair = (i & (head_size - 1)) >> 1;
		int32_t fcr = rot[pair * 2], fci = rot[pair * 2 + 1];
		int32_t a = v[i], b = v[i + 1];

		v[i]     = a * fcr - b * fci;
		v[i + 1] = a * fci + b * fcr;
	}
	return v_e - sh + rope->e;
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

/* x += y, bringing both to a common exponent and then spreading the result
   back over the full width of an int32.  Without the renormalisation the
   residual stream would lose a bit or two at every one of the ten adds a
   token. */
int residual_add(int32_t *x, int x_e, const int32_t *y, int y_e, int n)
{
	int e = x_e < y_e ? x_e : y_e;
	int dx = x_e - e, dy = y_e - e, i;

	for (i = 0; i < n; i++)
		x[i] = wr_sshift(x[i], dx) + wr_sshift(y[i], dy);
	return renormalize(x, n, e);
}

int32_t *llama_forward(llama_model *m, int token, int pos)
{
	const llama_config *c = &m->cfg;
	const int dim = c->dim, kv_dim = c->kv_dim, hidden_dim = c->hidden_dim;
	const int head_size = c->head_size, kv_mul = c->kv_mul;
	int32_t *x = m->x;
	int i, l, h, t;

	/* The token's embedding row, scaled up to use the full width so the
	   first residual adds have bits to give away. */
	{
		const int8_t *row = m->embed.q + (size_t)token * dim;
		int32_t s = m->embed.s.m[token];

		for (i = 0; i < dim; i++)
			x[i] = (int32_t)row[i] * s;
		m->x_e = renormalize(x, dim, m->embed.s.e);
	}

	for (l = 0; l < c->n_layers; l++) {
		const llama_layer *ly = &m->layers[l];
		size_t koff = (size_t)l * c->seq_len * kv_dim;
		size_t eoff = (size_t)l * c->seq_len;
		int8_t *k = m->key_cache + koff + (size_t)pos * kv_dim;
		int8_t *v = m->value_cache + koff + (size_t)pos * kv_dim;
		int k_e, v_e, min_ke, min_ve;

		m->xq_e = rmsnorm_i8(m->xq, m->scratch, x, dim, &ly->rms_att);

		matmul(m->q, &m->q_e, m->xq, m->xq_e, &ly->wq, dim, dim);
		matmul(m->xb, &k_e, m->xq, m->xq_e, &ly->wk, dim, kv_dim);
		m->q_e = rope(m->q, dim, m->q_e, &m->rope, pos, head_size);
		k_e = rope(m->xb, kv_dim, k_e, &m->rope, pos, head_size);
		m->key_exp[eoff + pos] = normalize_i8(k, m->xb, kv_dim, k_e);

		matmul(m->xb, &v_e, m->xq, m->xq_e, &ly->wv, dim, kv_dim);
		m->value_exp[eoff + pos] = normalize_i8(v, m->xb, kv_dim, v_e);

		/* One exponent per cached position, so scores and values from
		   different positions have to be brought to a common frame
		   before they can be compared or summed.  Shifting the
		   smaller ones down is exact; shifting up would overflow. */
		min_ke = m->key_exp[eoff];
		min_ve = m->value_exp[eoff];
		for (t = 1; t <= pos; t++) {
			if (m->key_exp[eoff + t] < min_ke)
				min_ke = m->key_exp[eoff + t];
			if (m->value_exp[eoff + t] < min_ve)
				min_ve = m->value_exp[eoff + t];
		}

		/* The query is int8 for the score dot products. */
		m->q_e = normalize_i8(m->xq, m->q, dim, m->q_e);

		for (h = 0; h < c->n_heads; h++) {
			const int8_t *qh = m->xq + h * head_size;
			size_t hoff = koff + (size_t)(h / kv_mul) * head_size;
			int32_t *att = m->att;
			int32_t best, sum, recip;
			int score_e = m->q_e + min_ke;

			for (t = 0; t <= pos; t++) {
				const int8_t *kt = m->key_cache + hoff +
						   (size_t)t * kv_dim;
				int32_t acc = 0;

				for (i = 0; i < head_size; i++)
					acc += (int32_t)qh[i] * (int32_t)kt[i];
				/* To the common exponent, then to Q12 with
				   1/sqrt(head_size) folded in. */
				acc >>= m->key_exp[eoff + t] - min_ke;
				att[t] = wr_sshift(acc * m->inv_root_head,
						score_e);
			}

			best = att[0];
			for (t = 1; t <= pos; t++)
				if (att[t] > best)
					best = att[t];
			sum = 0;
			for (t = 0; t <= pos; t++) {
				att[t] = wr_exp_q12(att[t] - best);
				sum += att[t];
			}
			if (sum == 0)
				sum = 1;
			/* Q7 weights: 128 is one, so the weighted sum below
			   carries a power-of-two scale and not a division.
			   sum is at least Q12_ONE, so recip fits. */
			recip = (128 << 16) / sum;
			for (t = 0; t <= pos; t++) {
				int32_t a = (att[t] * recip) >> 16;

				if (a > 127)
					a = 127;
				/* Scaling the weight is the same as scaling
				   the value it multiplies, and there is one
				   weight against head_size values. */
				att[t] = a >> (m->value_exp[eoff + t] - min_ve);
			}

			{
				int32_t *xb = m->xb + h * head_size;

				memset(xb, 0, (size_t)head_size * sizeof *xb);
				for (t = 0; t <= pos; t++) {
					const int8_t *vt = m->value_cache +
							   hoff +
							   (size_t)t * kv_dim;
					int32_t a = att[t];

					if (a == 0)
						continue;
					for (i = 0; i < head_size; i++)
						xb[i] += a * (int32_t)vt[i];
				}
			}
		}
		m->xb_e = 7 + min_ve;

		m->xq_e = normalize_i8(m->xq, m->xb, dim, m->xb_e);
		matmul(m->xb, &m->xb_e, m->xq, m->xq_e, &ly->wo, dim, dim);
		m->x_e = residual_add(x, m->x_e, m->xb, m->xb_e, dim);

		m->xq_e = rmsnorm_i8(m->xq, m->scratch, x, dim, &ly->rms_ffn);
		matmul(m->hb, &m->hb_e, m->xq, m->xq_e, &ly->w1, dim,
		       hidden_dim);
		matmul(m->hb2, &m->hb2_e, m->xq, m->xq_e, &ly->w3, dim,
		       hidden_dim);

		/* SwiGLU: hb <- silu(hb) * hb2, silu(v) = v / (1 + exp(-v)).
		   The sigmoid needs the real magnitude of v, so hb goes to
		   Q12 for that one step while the value itself stays in its
		   own exponent. */
		{
			int sh1 = reduce_to(m->hb, hidden_dim, 14);
			int sh2 = reduce_to(m->hb2, hidden_dim, 14);
			int e1 = m->hb_e - sh1;

			for (i = 0; i < hidden_dim; i++) {
				int32_t vq = wr_sshift(m->hb[i], e1 - 12);
				int32_t ex = wr_exp_q12(vq > 0 ? -vq : vq);
				int32_t sig;

				/* 1/(1+e^-v) for v >= 0, e^v/(1+e^v) below. */
				sig = vq >= 0 ?
				      (Q12_ONE << 12) / (Q12_ONE + ex) :
				      (ex << 12) / (Q12_ONE + ex);
				m->hb[i] = (m->hb[i] * sig) >> 12;
			}
			reduce_to(m->hb, hidden_dim, 14);
			for (i = 0; i < hidden_dim; i++)
				m->hb[i] *= m->hb2[i];
			m->hb_e = e1 + (m->hb2_e - sh2);
		}

		m->hq_e = normalize_i8(m->hq, m->hb, hidden_dim, m->hb_e);
		matmul(m->xb, &m->xb_e, m->hq, m->hq_e, &ly->w2, hidden_dim,
		       dim);
		m->x_e = residual_add(x, m->x_e, m->xb, m->xb_e, dim);
	}

	m->xq_e = rmsnorm_i8(m->xq, m->scratch, x, dim, &m->rms_final);
	matmul(m->logits, &m->logits_e, m->xq, m->xq_e, &m->wcls, dim,
	       c->vocab_size);
	/* An argmax needs no scale -- every logit shares one exponent, so
	   comparing them compares the reals -- but sampling does, because
	   exp() cares how far apart they actually are. */
	return m->logits;
}
