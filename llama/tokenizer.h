/* SentencePiece BPE tokenizer, GPL-3.0-or-later.
 *
 * The vocabulary file is llama2.c's own `tokenizer.bin` unchanged, so the
 * same tok512.bin that upstream uses works here.
 */

#ifndef WR_LLAMA_TOKENIZER_H
#define WR_LLAMA_TOKENIZER_H

#include <stddef.h>
#include <stdint.h>

enum {
	TOK_UNK = 0,
	TOK_BOS = 1,
	TOK_EOS = 2,
	/* The vocabulary's first three entries are <unk>, <s> and </s>, so a
	   raw byte b is the token b + 3. */
	TOK_BYTE_BASE = 3,
};

typedef struct {
	void *blob;		/* the vocabulary file, adopted */
	size_t blob_bytes;
	int vocab_size;
	int max_token_length;
	const char **piece;	/* into blob; NUL-terminated in place */
	const float *score;
	int *sorted;		/* piece indices, ordered by strcmp */
	char *scratch;		/* 2 * max_token_length + 3 */
} llama_tokenizer;

/* Adopts `image`; llama_tokenizer_free releases it with `tag`.  The piece
   strings are terminated in place, so the image is modified. */
int llama_tokenizer_open(llama_tokenizer *t, void *image, size_t bytes,
			 int vocab_size, const char *tag);
void llama_tokenizer_free(llama_tokenizer *t, const char *tag);

/* The text for `token`, as a NUL-terminated string.  `prev` follows
   upstream's rule of stripping one leading space after BOS.  Raw-byte
   pieces spelled "<0xAB>" come back as the byte itself. */
const char *llama_decode(const llama_tokenizer *t, int prev, int token,
			 char out[8]);

/* Encode `text`, optionally with BOS/EOS.  Returns the token count, or -1
   if it would exceed `capacity`. */
int llama_encode(llama_tokenizer *t, const char *text, int bos, int eos,
		 int *tokens, int capacity);

#endif
