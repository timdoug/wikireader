/* SentencePiece BPE tokenizer, GPL-3.0-or-later.
 *
 * A port of llama2.c's tokenizer with the allocations flattened: upstream
 * mallocs one buffer per vocabulary entry, which would be 512 allocator
 * headers here for a 6 KB file.  The pieces are terminated in place inside
 * the image instead, which works because every entry's length field is
 * immediately followed by its bytes and then the next entry's score -- so
 * the low byte of the next score becomes the terminator, and the score is
 * copied out before that happens.
 */

#include <string.h>

#include "model.h"
#include "tokenizer.h"

static uint32_t rd32(const unsigned char *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static float rdf32(const unsigned char *p)
{
	uint32_t b = rd32(p);
	float f;

	memcpy(&f, &b, sizeof f);
	return f;
}

static int piece_cmp(const llama_tokenizer *t, int a, const char *s)
{
	return strcmp(t->piece[a], s);
}

/* Insertion into a sorted index.  The vocabulary is 512 entries for the
   models that fit on this device and 32000 at the largest; an O(n log n)
   sort would need a comparison callback and a qsort, and mini-libc has
   neither.  Binary insertion keeps it to one pass with memmove. */
static void build_index(llama_tokenizer *t)
{
	int i;

	for (i = 0; i < t->vocab_size; i++) {
		int lo = 0, hi = i;

		while (lo < hi) {
			int mid = (lo + hi) >> 1;

			if (piece_cmp(t, t->sorted[mid], t->piece[i]) < 0)
				lo = mid + 1;
			else
				hi = mid;
		}
		memmove(t->sorted + lo + 1, t->sorted + lo,
			(size_t)(i - lo) * sizeof *t->sorted);
		t->sorted[lo] = i;
	}
}

static int lookup(const llama_tokenizer *t, const char *s)
{
	int lo = 0, hi = t->vocab_size - 1;

	while (lo <= hi) {
		int mid = (lo + hi) >> 1;
		int c = piece_cmp(t, t->sorted[mid], s);

		if (c == 0)
			return t->sorted[mid];
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return -1;
}

int llama_tokenizer_open(llama_tokenizer *t, void *image, size_t bytes,
			 int vocab_size, const char *tag)
{
	unsigned char *p = image;
	size_t off = 0;
	int i;

	memset(t, 0, sizeof *t);
	t->blob = image;
	t->blob_bytes = bytes;
	t->vocab_size = vocab_size;

	if (bytes < 4 || vocab_size <= 0)
		return -1;
	t->max_token_length = (int)rd32(p);
	off = 4;

	t->piece = llama_alloc((size_t)vocab_size * sizeof *t->piece, tag);
	{
		float *scores = llama_alloc((size_t)vocab_size * sizeof(float),
					    tag);
		t->score = scores;
		t->sorted = llama_alloc((size_t)vocab_size * sizeof *t->sorted,
					tag);
		t->scratch = llama_alloc((size_t)t->max_token_length * 2 + 3,
					 tag);
		if (!t->piece || !scores || !t->sorted || !t->scratch)
			return -1;

		for (i = 0; i < vocab_size; i++) {
			uint32_t len;

			if (bytes - off < 8)
				return -1;
			scores[i] = rdf32(p + off);
			len = rd32(p + off + 4);
			off += 8;
			if (len > bytes - off)
				return -1;
			t->piece[i] = (const char *)(p + off);
			off += len;
			/* Terminate in place.  For the last entry this is the
			   byte one past the pieces, which the caller's buffer
			   must own -- llama_read_file always allocates one
			   spare byte for exactly this. */
			p[off] = '\0';
		}
	}
	build_index(t);
	return 0;
}

void llama_tokenizer_free(llama_tokenizer *t, const char *tag)
{
	llama_release((void *)t->piece, tag);
	llama_release((void *)t->score, tag);
	llama_release(t->sorted, tag);
	llama_release(t->scratch, tag);
	llama_release(t->blob, tag);
	memset(t, 0, sizeof *t);
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

const char *llama_decode(const llama_tokenizer *t, int prev, int token,
			 char out[8])
{
	const char *piece;

	if (token < 0 || token >= t->vocab_size)
		return "";
	piece = t->piece[token];
	/* SentencePiece strips one leading space after BOS. */
	if (prev == TOK_BOS && piece[0] == ' ')
		piece++;

	/* Raw-byte pieces are spelled "<0xAB>".  Upstream parses these with
	   sscanf; mini-libc has no scanf, and two hex digits do not need
	   one. */
	if (piece[0] == '<' && piece[1] == '0' && piece[2] == 'x') {
		int hi = hexval(piece[3]), lo = hexval(piece[4]);

		if (hi >= 0 && lo >= 0 && piece[5] == '>' && piece[6] == '\0') {
			out[0] = (char)((hi << 4) | lo);
			out[1] = '\0';
			return out;
		}
	}
	return piece;
}

int llama_encode(llama_tokenizer *t, const char *text, int bos, int eos,
		 int *tokens, int capacity)
{
	char *buf = t->scratch;
	int n = 0;
	size_t len = 0;
	const char *c;

	if (!text)
		return -1;

#define PUSH(id) do { if (n >= capacity) return -1; tokens[n++] = (id); } while (0)

	if (bos)
		PUSH(TOK_BOS);
	/* SentencePiece's add_dummy_prefix. */
	if (text[0] != '\0') {
		int id = lookup(t, " ");

		if (id != -1)
			PUSH(id);
	}

	for (c = text; *c != '\0'; c++) {
		int id;

		/* Restart the buffer at any byte that is not a UTF-8
		   continuation, so a codepoint is looked up whole. */
		if ((*c & 0xC0) != 0x80)
			len = 0;
		buf[len++] = *c;
		buf[len] = '\0';
		if ((c[1] & 0xC0) == 0x80 && len < 4)
			continue;

		id = lookup(t, buf);
		if (id != -1) {
			PUSH(id);
		} else {
			size_t i;

			/* Byte fallback: every byte has its own token. */
			for (i = 0; i < len; i++)
				PUSH((unsigned char)buf[i] + TOK_BYTE_BASE);
		}
		len = 0;
	}

	/* Merge the highest-scoring adjacent pair until none is left. */
	for (;;) {
		float best_score = -1e10f;
		int best_id = -1, best_idx = -1, i;

		for (i = 0; i < n - 1; i++) {
			size_t a = strlen(t->piece[tokens[i]]);
			size_t b = strlen(t->piece[tokens[i + 1]]);
			int id;

			if (a + b > (size_t)t->max_token_length * 2)
				continue;
			memcpy(buf, t->piece[tokens[i]], a);
			memcpy(buf + a, t->piece[tokens[i + 1]], b);
			buf[a + b] = '\0';
			id = lookup(t, buf);
			if (id != -1 && t->score[id] > best_score) {
				best_score = t->score[id];
				best_id = id;
				best_idx = i;
			}
		}
		if (best_idx == -1)
			break;
		tokens[best_idx] = best_id;
		memmove(tokens + best_idx + 1, tokens + best_idx + 2,
			(size_t)(n - best_idx - 2) * sizeof *tokens);
		n--;
	}

	if (eos)
		PUSH(TOK_EOS);
#undef PUSH
	return n;
}
