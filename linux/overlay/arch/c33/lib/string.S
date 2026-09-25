/* SPDX-License-Identifier: GPL-2.0 */
/*
 * memset, memcpy and memmove a word at a time.
 *
 * lib/string.c moves one byte per pass: four or five instructions a byte and
 * a separate SDRAM access for each.  Page clearing, copy_{to,from}_user and
 * the initramfs unpack all come through here.
 *
 * C ABI: arguments in %r6-%r8, the result in %r4, and %r4-%r14 free.  A loop
 * runs from the two-slot fetch queue only while its offset in the 16-byte
 * line plus its length is at most 27 bytes, so the block loops are kept to
 * 12 bytes, which fit wherever they land, and the one longer loop is
 * aligned.
 */
#include <linux/export.h>
#include <linux/linkage.h>

/* void *memset(void *s, int c, size_t n) */
SYM_FUNC_START(memset)
	ld.w	%r4,%r6
	xand	%r7,0xff
	cmp	%r8,8
	jrult	.Lset_bytes		/* too short to pay for the setup */
	ld.w	%r9,%r7			/* c in every byte of a word */
	xsll	%r9,8
	or	%r7,%r9
	ld.w	%r9,%r7
	xsll	%r9,16
	or	%r7,%r9
.Lset_align:				/* bring the destination to a word */
	ld.w	%r9,%r6
	and	%r9,3
	jreq	.Lset_aligned
	ld.b	[%r6]+,%r7
	sub	%r8,1
	jp	.Lset_align
.Lset_aligned:				/* at least five bytes are left */
	ld.w	%r5,%r8
	and	%r5,15			/* what the 16-byte blocks leave */
	sub	%r8,%r5
	jreq	.Lset_words
	add	%r8,%r6			/* end of the blocks */
1:	ld.w	[%r6]+,%r7
	ld.w	[%r6]+,%r7
	ld.w	[%r6]+,%r7
	ld.w	[%r6]+,%r7
	cmp	%r6,%r8
	jrne	1b
.Lset_words:
	ld.w	%r8,%r5
	cmp	%r8,4
	jrult	.Lset_bytes
2:	ld.w	[%r6]+,%r7
	sub	%r8,4
	cmp	%r8,4
	jruge	2b
.Lset_bytes:
	cmp	%r8,0
	jreq	3f
4:	ld.b	[%r6]+,%r7
	sub	%r8,1
	jrne	4b
3:	ret
SYM_FUNC_END(memset)
EXPORT_SYMBOL(memset)

/* void *memcpy(void *dest, const void *src, size_t n) */
SYM_FUNC_START(memcpy)
	ld.w	%r4,%r6
	ld.w	%r9,%r6
	xor	%r9,%r7
	and	%r9,3
	jrne	.Lcpy_mismatch		/* no number of bytes lines these up */
	cmp	%r8,8
	jrult	.Lcpy_bytes
.Lcpy_align:				/* bring both to a word */
	ld.w	%r9,%r6
	and	%r9,3
	jreq	.Lcpy_aligned
	ld.ub	%r9,[%r7]+
	ld.b	[%r6]+,%r9
	sub	%r8,1
	jp	.Lcpy_align
.Lcpy_aligned:				/* at least five bytes are left */
	ld.w	%r5,%r8
	and	%r5,7			/* what the 8-byte blocks leave */
	sub	%r8,%r5
	jreq	.Lcpy_word
	add	%r8,%r6			/* destination end of the blocks */
1:	ld.w	%r9,[%r7]+
	ld.w	[%r6]+,%r9
	ld.w	%r10,[%r7]+
	ld.w	[%r6]+,%r10
	cmp	%r6,%r8
	jrne	1b
.Lcpy_word:
	ld.w	%r8,%r5
	cmp	%r8,4
	jrult	.Lcpy_bytes
	ld.w	%r9,[%r7]+
	ld.w	[%r6]+,%r9
	sub	%r8,4
.Lcpy_bytes:
	cmp	%r8,0
	jreq	3f
4:	ld.ub	%r9,[%r7]+
	ld.b	[%r6]+,%r9
	sub	%r8,1
	jrne	4b
3:	ret

/*
 * The pointers sit at different offsets in their words.  Align the
 * destination, then read the source a whole word at a time and build each
 * output word from two neighbours: the source is read once and every store
 * is a word.  The last word read is the one holding the last source byte a
 * whole output word needs, so nothing past the source's last byte's word is
 * touched.
 */
.Lcpy_mismatch:
	cmp	%r8,16
	jrult	.Lcpy_bytes		/* the byte loop is cheaper */
5:	ld.w	%r9,%r6
	and	%r9,3
	jreq	6f
	ld.ub	%r9,[%r7]+
	ld.b	[%r6]+,%r9
	sub	%r8,1
	jp	5b
6:	ld.w	%r13,%r7
	and	%r13,3			/* 1-3: the source's offset in its word */
	sub	%r7,%r13		/* the source's word */
	ld.w	%r10,%r13
	xsll	%r10,3			/* right shift for the earlier word */
	xld.w	%r11,32
	sub	%r11,%r10		/* left shift for the later word */
	ld.w	%r14,%r8
	and	%r14,3			/* bytes after the last whole word */
	sub	%r8,%r14
	add	%r8,%r6			/* destination end of the words */
	ld.w	%r5,[%r7]+
	.balign	16
7:	ld.w	%r12,[%r7]+
	srl	%r5,%r10
	ld.w	%r9,%r12
	sll	%r9,%r11
	or	%r5,%r9
	ld.w	[%r6]+,%r5
	ld.w	%r5,%r12
	cmp	%r6,%r8
	jrne	7b
	sub	%r7,4			/* back to the word the tail starts in */
	add	%r7,%r13		/* and to its byte */
	ld.w	%r8,%r14
	jp	.Lcpy_bytes
SYM_FUNC_END(memcpy)
EXPORT_SYMBOL(memcpy)

/*
 * void *memmove(void *dest, const void *src, size_t n)
 *
 * memcpy goes forwards and never writes a word before it has read every
 * source byte that word overlaps, so it serves whenever the destination is
 * below the source or clear of it.  Only an overlap from above copies here,
 * backwards: words when the alignments agree, bytes otherwise.
 */
SYM_FUNC_START(memmove)
	cmp	%r6,%r7
	jrule	1f
	ld.w	%r9,%r7
	add	%r9,%r8
	cmp	%r6,%r9
	jrult	2f
1:	xjp	memcpy
2:	ld.w	%r4,%r6
	add	%r6,%r8			/* one past the end of each */
	add	%r7,%r8
	ld.w	%r9,%r6
	xor	%r9,%r7
	and	%r9,3
	jrne	.Lmove_bytes
	cmp	%r8,8
	jrult	.Lmove_bytes
3:	ld.w	%r9,%r6			/* bring the destination end to a word */
	and	%r9,3
	jreq	4f
	sub	%r7,1
	ld.ub	%r9,[%r7]
	sub	%r6,1
	ld.b	[%r6],%r9
	sub	%r8,1
	jp	3b
4:	ld.w	%r5,%r8
	and	%r5,3			/* bytes before the first whole word */
	sub	%r8,%r5
	ld.w	%r10,%r6
	sub	%r10,%r8		/* destination start of the words */
5:	sub	%r7,4
	ld.w	%r9,[%r7]
	sub	%r6,4
	ld.w	[%r6],%r9
	cmp	%r6,%r10
	jrne	5b
	ld.w	%r8,%r5
.Lmove_bytes:
	cmp	%r8,0
	jreq	7f
6:	sub	%r7,1
	ld.ub	%r9,[%r7]
	sub	%r6,1
	ld.b	[%r6],%r9
	sub	%r8,1
	jrne	6b
7:	ret
SYM_FUNC_END(memmove)
EXPORT_SYMBOL(memmove)
