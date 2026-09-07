; The Zstandard sequence loop for the C33, hand-written.
;
; Called from ZSTD_decompressSequences_default (zstddeclib.c) with a
; pointer to a ZSTD_c33_seqLoopState in %r6.  Decodes and executes
; sequences until either all are done (returns 0) or one needs the careful
; path near the end of the block or of the literals (returns 1 with ll, ml
; and offset filled in, the states already advanced and nbSeq already
; counted down; the caller executes that sequence and calls again).
;
; The compiler kept the bit reader and the FSE states in memory and
; reloaded them after every store into the output; this loop keeps the bit
; container, the bits consumed, the output and literal pointers, the last
; offset and the sequence count in registers for the whole block.  What the
; loop touches once per sequence (the three states, the older repeat
; offsets, the bounds) lives in the frame, a 2-cycle load on the DSTRAM
; stack.
;
; Registers, for the whole loop:
;   %r0  bit container (the four bytes at ptr, little endian)
;   %r1  bits consumed from the top of the container
;   %r2  op, the output pointer
;   %r3  litPtr, the literal pointer
;   %r13 rep0, the most recent offset
;   %r14 sequences left
;   %r4/%r5/%r6 the LL/ML/OF table words, %r7 ll, %r8 ml, %r9 offset,
;   %r10..%r12 scratch.  The copies borrow %r0/%r1 too, saved in the frame.
;
; Frame (word slots from %sp):
;   0 bits (spill)   1 consumed (spill)  2 ptr        3 start
;   4 sLL*4          5 sOF*4             6 sML*4      7 rep1
;   8 rep2           9 litEnd           10 oend_w    11 prefixStart
;  12 longOffsets   13 state pointer    14 scratch   15 scratch
;
; Table words (ZSTD_c33_packEntry): bits 31..23 nextState, so w >> 21 is
; the next state scaled to a word offset; bits 20..10 the base value or
; 0x7ff for one recomputed from the extra-bit count; bits 9..5 s =
; (32 - extraBits) & 31, the shift that extracts the extra bits; bits 4..0
; u = 30 - stateBits, the shift that extracts the state bits scaled by 4
; (two spare low bits, masked off).
;
; Reading n bits from the top: (bits << consumed) >> (32 - n).  With s from
; the table that is one shift; a read of possibly zero bits uses u and a
; final "and -4" so that a shift by 32, which this core treats as no shift,
; still yields zero.  A refill is needed when consumed + n > 32.

	.section .fastcode,"ax"
	.align 1

; The refill: move ptr back by whole bytes, clamped at the start of the
; stream, and reload the container.  Uses %r10 and %r11.  This is what
; BIT_reloadDStream does in each of its cases; at the very start it leaves
; everything as it is, which re-reading the same word also does.
	.macro	REFILL
	ld.w	%r11,[%sp+2]
	ld.w	%r10,[%sp+3]
	sub	%r11,%r10		; bytes available behind ptr
	ld.w	%r10,%r1
	srl	%r10,3			; whole bytes consumed
	cmp	%r11,%r10
	jruge	1f
	ld.w	%r10,%r11		; clamp
1:
	ld.w	%r11,[%sp+2]
	sub	%r11,%r10
	ld.w	[%sp+2],%r11		; ptr -= nb
	sll	%r10,3
	sub	%r1,%r10		; consumed -= nb * 8
	ld.ub	%r0,[%r11]+
	ld.ub	%r10,[%r11]+
	sll	%r10,8
	or	%r0,%r10
	ld.ub	%r10,[%r11]+
	sll	%r10,16
	or	%r0,%r10
	ld.ub	%r10,[%r11]
	sll	%r10,24
	or	%r0,%r10
	.endm

; One batch of eight: load all, then store all, so the two rows involved
; are each opened once per batch.  Over-copies to the end of the batch.
	.macro	BATCH8 ld st
	\ld	%r4,[%r1]+
	\ld	%r5,[%r1]+
	\ld	%r6,[%r1]+
	\ld	%r8,[%r1]+
	\ld	%r9,[%r1]+
	\ld	%r10,[%r1]+
	\ld	%r11,[%r1]+
	\ld	%r12,[%r1]+
	\st	[%r0]+,%r4
	\st	[%r0]+,%r5
	\st	[%r0]+,%r6
	\st	[%r0]+,%r8
	\st	[%r0]+,%r9
	\st	[%r0]+,%r10
	\st	[%r0]+,%r11
	\st	[%r0]+,%r12
	.endm

; Copy from %r1 to %r0 up to %r7 (exclusive), over-copying to the end of
; the last batch: the caller guarantees 32 bytes of slack after both.  %r12
; is the widest batch the source-destination distance allows: 4 words, 3
; the realigned word batch, 2 halfwords, 1 bytes.  Clobbers %r4..%r6 and
; %r8..%r12.  (The macro is instantiated twice so the paths stay inline;
; the numeric labels are local to each use.)
	.macro	COPY
	ld.w	%r4,%r7
	sub	%r4,%r0			; length
	cmp	%r4,8
	xjrugt	2f
	BATCH8	ld.ub,ld.b		; up to eight bytes: one batch
	xjp	9f
2:
	cmp	%r12,4
	xjrne	4f
	cmp	%r4,16
	xjrule	4f
	ld.w	%r5,%r0
	sub	%r5,%r1
	and	%r5,3
	xjrne	4f			; not word-aligned with each other
	ld.w	%r5,%r0
	and	%r5,3
	xjreq	3f
	BATCH8	ld.ub,ld.b		; bring the destination to a word boundary
	ld.w	%r5,%r0			; (the batch used %r5) net advance 4 - misalign:
	and	%r5,3			; back up by 4 + misalign from the batch's end
	add	%r5,4
	sub	%r0,%r5
	sub	%r1,%r5
3:
	BATCH8	ld.w,ld.w		; 32 bytes a batch
	cmp	%r0,%r7
	jrult	3b
	xjp	9f
4:
	cmp	%r12,3
	xjrult	6f
	ld.w	%r5,%r0
	sub	%r5,%r1
	and	%r5,3
	xjreq	6f			; word-aligned: halfwords will do
	ld.w	%r5,%r0
	and	%r5,3
	xjreq	5f
	BATCH8	ld.ub,ld.b
	ld.w	%r5,%r0
	and	%r5,3
	add	%r5,4
	sub	%r0,%r5
	sub	%r1,%r5
5:
	; Realigned words: 16 bytes a batch, each output word assembled
	; from two neighbouring aligned source words.
	ld.w	%r9,%r1
	and	%r9,3
	sll	%r9,3			; sh = (src & 3) * 8, 8..24
	xld.w	%r10,32
	sub	%r10,%r9		; sh2 = 32 - sh
	and	%r1,-4
	ld.w	%r8,[%r1]+		; the word holding the first byte
5:
	ld.w	%r4,[%r1]+
	ld.w	%r5,[%r1]+
	ld.w	%r6,[%r1]+
	ld.w	%r11,[%r1]+
	srl	%r8,%r9
	ld.w	%r12,%r4
	sll	%r12,%r10
	or	%r8,%r12
	srl	%r4,%r9
	ld.w	%r12,%r5
	sll	%r12,%r10
	or	%r4,%r12
	srl	%r5,%r9
	ld.w	%r12,%r6
	sll	%r12,%r10
	or	%r5,%r12
	srl	%r6,%r9
	ld.w	%r12,%r11
	sll	%r12,%r10
	or	%r6,%r12
	ld.w	[%r0]+,%r8
	ld.w	[%r0]+,%r4
	ld.w	[%r0]+,%r5
	ld.w	[%r0]+,%r6
	ld.w	%r8,%r11
	cmp	%r0,%r7
	jrult	5b
	xjp	9f
6:
	cmp	%r12,2
	xjrult	8f
	ld.w	%r5,%r0
	sub	%r5,%r1
	and	%r5,1
	xjrne	8f			; parity differs: bytes
	ld.w	%r5,%r0
	and	%r5,1
	xjreq	7f
	BATCH8	ld.ub,ld.b
	sub	%r0,7
	sub	%r1,7
7:
	BATCH8	ld.uh,ld.h		; 16 bytes a batch
	cmp	%r0,%r7
	jrult	7b
	xjp	9f
8:
	BATCH8	ld.ub,ld.b		; 8 bytes a batch
	cmp	%r0,%r7
	jrult	8b
9:
	.endm

	.global	ZSTD_c33_seqLoop
ZSTD_c33_seqLoop:
	pushn	%r3
	sub	%sp,16
	ld.w	[%sp+13],%r6
	ld.w	%r0,[%r6]
	xld.w	%r1,[%r6+4]
	xld.w	%r4,[%r6+8]
	ld.w	[%sp+2],%r4
	xld.w	%r4,[%r6+12]
	ld.w	[%sp+3],%r4
	xld.w	%r4,[%r6+16]
	ld.w	[%sp+4],%r4
	xld.w	%r4,[%r6+20]
	ld.w	[%sp+5],%r4
	xld.w	%r4,[%r6+24]
	ld.w	[%sp+6],%r4
	xld.w	%r13,[%r6+28]
	xld.w	%r4,[%r6+32]
	ld.w	[%sp+7],%r4
	xld.w	%r4,[%r6+36]
	ld.w	[%sp+8],%r4
	xld.w	%r2,[%r6+40]
	xld.w	%r3,[%r6+44]
	xld.w	%r4,[%r6+48]
	ld.w	[%sp+9],%r4
	xld.w	%r4,[%r6+52]
	ld.w	[%sp+10],%r4
	xld.w	%r4,[%r6+56]
	ld.w	[%sp+11],%r4
	xld.w	%r14,[%r6+60]
	xld.w	%r4,[%r6+64]
	ld.w	[%sp+12],%r4

.Lseq:
	ld.w	%r4,[%sp+4]
	ld.w	%r5,[%sp+6]
	ld.w	%r6,[%sp+5]
	xld.w	%r4,[%r4+0x81a04]	; LL entry (ZSTD_C33_IVRAM_LL + 1)
	xld.w	%r5,[%r5+0x8260c]	; ML entry
	xld.w	%r6,[%r6+0x82208]	; OF entry

	; literal length base
	ld.w	%r7,%r4
	srl	%r7,10
	xand	%r7,0x7ff
	xcmp	%r7,0x7ff
	xjreq	.Lllmark
.Lllok:
	; match length base
	ld.w	%r8,%r5
	srl	%r8,10
	xand	%r8,0x7ff
	xcmp	%r8,0x7ff
	xjreq	.Lmlmark
.Lmlok:
	; offset: s = (32 - ofBits) & 31 is 0 for no offset bits, 31 for one
	ld.w	%r9,%r6
	srl	%r9,5
	and	%r9,31
	cmp	%r9,0
	xjrne	.Lofnz
	cmp	%r7,0
	xjreq.d	.Lofswap		; ll == 0: the second repeat offset
	ld.w	%r9,%r13		; else the first
.Lofdone:
	; match length extra bits
	ld.w	%r10,%r5
	srl	%r10,5
	and	%r10,31
	cmp	%r10,0
	xjrne	.Lmlbits
.Lmldone:
	; literal length extra bits
	ld.w	%r10,%r4
	srl	%r10,5
	and	%r10,31
	cmp	%r10,0
	xjrne	.Lllbits
.Llldone:
	; next states, except after the last sequence
	cmp	%r14,1
	xjreq	.Lexec
	ld.w	%r10,%r4
	and	%r10,31			; u = 30 - LL state bits
	ld.w	%r11,%r5
	and	%r11,31			; u = 30 - ML state bits
	ld.w	%r12,%r10
	add	%r12,%r11
	sub	%r12,28
	cmp	%r1,%r12		; consumed + both > 32 ?
	xjrugt	.Lrefillst
.Lst1:
	ld.w	%r12,%r0
	sll	%r12,%r1
	srl	%r12,%r10
	and	%r12,-4			; LL state bits, scaled
	add	%r1,30
	sub	%r1,%r10
	ld.w	%r10,%r4
	srl	%r10,21
	add	%r10,%r12
	ld.w	[%sp+4],%r10
	ld.w	%r12,%r0
	sll	%r12,%r1
	srl	%r12,%r11
	and	%r12,-4			; ML state bits, scaled
	add	%r1,30
	sub	%r1,%r11
	ld.w	%r11,%r5
	srl	%r11,21
	add	%r11,%r12
	ld.w	[%sp+6],%r11
	ld.w	%r10,%r6
	and	%r10,31			; u = 30 - OF state bits
	ld.w	%r12,%r10
	add	%r12,2
	cmp	%r1,%r12		; consumed + OF bits > 32 ?
	xjrugt	.Lrefillof
.Lst2:
	ld.w	%r12,%r0
	sll	%r12,%r1
	srl	%r12,%r10
	and	%r12,-4
	add	%r1,30
	sub	%r1,%r10
	ld.w	%r10,%r6
	srl	%r10,21
	add	%r10,%r12
	ld.w	[%sp+5],%r10

.Lexec:
	sub	%r14,1
	ld.w	%r10,[%sp+9]
	sub	%r10,%r3		; literals left
	cmp	%r7,%r10
	xjrugt	.Lslow
	ld.w	%r10,[%sp+10]
	sub	%r10,%r2		; room before oend_w (signed: may be negative)
	ld.w	%r11,%r7
	add	%r11,%r8
	cmp	%r11,%r10
	xjrgt	.Lslow
	ld.w	%r11,%r2
	add	%r11,%r7		; end of the literals in the output
	ld.w	%r10,[%sp+11]
	sub	%r11,%r10		; distance back to the prefix start
	cmp	%r9,%r11
	xjrugt	.Lslow			; offset reaches before the prefix
	ld.w	%r11,%r2
	add	%r11,%r7
	sub	%r11,%r9		; match source
	ld.w	[%sp+0],%r0
	ld.w	[%sp+1],%r1
	cmp	%r7,0
	xjreq	.Lmatch

	; literals: %r0 dst, %r1 src, %r7 end, width 4
	ld.w	[%sp+14],%r8
	ld.w	[%sp+15],%r11
	ld.w	%r0,%r2
	ld.w	%r1,%r3
	add	%r7,%r2
	ld.w	%r12,4
	COPY
	ld.w	%r4,%r7
	sub	%r4,%r2
	add	%r3,%r4			; litPtr += ll
	ld.w	%r2,%r7			; op = end of literals
	ld.w	%r8,[%sp+14]
	ld.w	%r11,[%sp+15]

.Lmatch:
	; match: %r0 dst, %r1 src, %r7 end; the batch width from the offset
	ld.w	%r0,%r2
	ld.w	%r1,%r11
	ld.w	%r7,%r2
	add	%r7,%r8
	ld.w	%r9,%r0
	sub	%r9,%r1			; offset
	cmp	%r9,8
	xjrult	.Lclose
	ld.w	%r12,4
	xcmp	%r9,32
	jruge	1f
	ld.w	%r12,3
	cmp	%r9,20
	jruge	1f
	ld.w	%r12,2
	cmp	%r9,16
	jruge	1f
	ld.w	%r12,1
1:
	COPY
.Lmatchdone:
	ld.w	%r2,%r7			; op = end of match
	ld.w	%r0,[%sp+0]
	ld.w	%r1,[%sp+1]
	cmp	%r14,0
	xjrne	.Lseq
	ld.w	%r4,0
	xjp	.Lexit

.Lclose:
	; offset below 8: byte by byte, which is correct for any overlap
	ld.ub	%r4,[%r1]+
	ld.b	[%r0]+,%r4
	cmp	%r0,%r7
	jrult	.Lclose
	xjp	.Lmatchdone

.Lslow:
	ld.w	%r4,1
.Lexit:
	ld.w	%r6,[%sp+13]
	ld.w	[%r6],%r0
	xld.w	[%r6+4],%r1
	ld.w	%r5,[%sp+2]
	xld.w	[%r6+8],%r5
	ld.w	%r5,[%sp+4]
	xld.w	[%r6+16],%r5
	ld.w	%r5,[%sp+5]
	xld.w	[%r6+20],%r5
	ld.w	%r5,[%sp+6]
	xld.w	[%r6+24],%r5
	xld.w	[%r6+28],%r13
	ld.w	%r5,[%sp+7]
	xld.w	[%r6+32],%r5
	ld.w	%r5,[%sp+8]
	xld.w	[%r6+36],%r5
	xld.w	[%r6+40],%r2
	xld.w	[%r6+44],%r3
	xld.w	[%r6+60],%r14
	xld.w	[%r6+68],%r7
	xld.w	[%r6+72],%r8
	xld.w	[%r6+76],%r9
	add	%sp,16
	popn	%r3
	ret

; ---- out-of-line pieces ------------------------------------------------

.Lllmark:				; base 1 << extraBits, extraBits = 32 - s
	ld.w	%r10,%r4
	srl	%r10,5
	and	%r10,31
	xld.w	%r7,32
	sub	%r7,%r10
	ld.w	%r10,1
	sll	%r10,%r7
	ld.w	%r7,%r10
	xjp	.Lllok

.Lmlmark:				; base (1 << extraBits) + 3
	ld.w	%r10,%r5
	srl	%r10,5
	and	%r10,31
	xld.w	%r8,32
	sub	%r8,%r10
	ld.w	%r10,1
	sll	%r10,%r8
	ld.w	%r8,%r10
	add	%r8,3
	xjp	.Lmlok

.Lofswap:				; offset = rep1, rep1 = rep0
	ld.w	%r9,[%sp+7]
	ld.w	[%sp+7],%r13
	ld.w	%r13,%r9
	xjp	.Lofdone

.Lofnz:					; %r9 = s, 1..31
	cmp	%r9,31
	xjrne	.Lofbig
	; one offset bit: a repeat offset chosen by ll0 + bit
	cmp	%r1,31
	xjrugt	.Lrefillof1
.Lof1:
	ld.w	%r10,%r0
	sll	%r10,%r1
	srl	%r10,31
	add	%r1,1
	add	%r10,1			; 1 + bit
	cmp	%r7,0
	jrne	1f
	add	%r10,1			; + ll0
1:
	cmp	%r10,3
	jrne	2f
	ld.w	%r9,%r13
	sub	%r9,1			; rep0 - 1
	jp	4f
2:
	cmp	%r10,1
	jrne	3f
	ld.w	%r9,[%sp+7]		; rep1
	jp	4f
3:
	ld.w	%r9,[%sp+8]		; rep2
4:
	cmp	%r9,0
	jrne	5f
	sub	%r9,1			; 0 is not valid: force -1, caught by the prefix check
5:
	cmp	%r10,1
	jreq	6f
	ld.w	%r11,[%sp+7]
	ld.w	[%sp+8],%r11		; rep2 = rep1
6:
	ld.w	[%sp+7],%r13		; rep1 = rep0
	ld.w	%r13,%r9		; rep0 = offset
	xjp	.Lofdone

.Lofbig:				; ofBits = 32 - s, 2..31
	cmp	%r9,7
	jrugt	1f
	ld.w	%r10,[%sp+12]
	cmp	%r10,0
	xjrne	.Loflong		; 25 bits or more with long offsets: two reads
1:
	cmp	%r1,%r9			; consumed + ofBits > 32 ?
	xjrugt	.Lrefillofbig
.Lofbig2:
	ld.w	%r10,%r0
	sll	%r10,%r1
	srl	%r10,%r9		; the offset bits
	add	%r1,32
	sub	%r1,%r9
	xld.w	%r11,32
	sub	%r11,%r9		; ofBits
	ld.w	%r12,1
	sll	%r12,%r11
	sub	%r12,3			; base (1 << ofBits) - 3
	add	%r10,%r12
.Lofbig3:
	ld.w	%r9,%r10
	ld.w	%r11,[%sp+7]
	ld.w	[%sp+8],%r11		; rep2 = rep1
	ld.w	[%sp+7],%r13		; rep1 = rep0
	ld.w	%r13,%r9		; rep0 = offset
	xjp	.Lofdone

.Loflong:				; first ofBits - 5 bits, refill, then 5
	ld.w	%r10,%r9
	add	%r10,5
	cmp	%r1,%r10		; consumed + ofBits - 5 > 32 ?
	xjrugt	.Lrefilllong
.Loflong2:
	ld.w	%r10,%r0
	sll	%r10,%r1
	ld.w	%r11,%r9
	add	%r11,5
	srl	%r10,%r11
	sll	%r10,5
	add	%r1,27
	sub	%r1,%r9			; consumed += ofBits - 5
	xld.w	%r11,32
	sub	%r11,%r9
	ld.w	%r12,1
	sll	%r12,%r11
	sub	%r12,3
	add	%r10,%r12		; base + high bits
	cmp	%r1,0
	jreq	2f
	ld.w	[%sp+14],%r10
	REFILL
	ld.w	%r10,[%sp+14]
2:
	ld.w	%r11,%r0
	sll	%r11,%r1
	srl	%r11,27			; the low five bits
	add	%r1,5
	add	%r10,%r11
	xjp	.Lofbig3

.Lmlbits:				; %r10 = s = 32 - extraBits
	cmp	%r1,%r10		; consumed + extraBits > 32 ?
	xjrugt	.Lrefillml
.Lmlbits2:
	ld.w	%r11,%r0
	sll	%r11,%r1
	srl	%r11,%r10
	add	%r8,%r11
	add	%r1,32
	sub	%r1,%r10
	xjp	.Lmldone

.Lllbits:
	cmp	%r1,%r10
	xjrugt	.Lrefillll
.Lllbits2:
	ld.w	%r11,%r0
	sll	%r11,%r1
	srl	%r11,%r10
	add	%r7,%r11
	add	%r1,32
	sub	%r1,%r10
	xjp	.Llldone

; Refills; each recomputes what the refill's scratch registers held.
.Lrefillml:
	REFILL
	ld.w	%r10,%r5
	srl	%r10,5
	and	%r10,31
	xjp	.Lmlbits2

.Lrefillll:
	REFILL
	ld.w	%r10,%r4
	srl	%r10,5
	and	%r10,31
	xjp	.Lllbits2

.Lrefillst:
	REFILL
	ld.w	%r10,%r4
	and	%r10,31
	ld.w	%r11,%r5
	and	%r11,31
	xjp	.Lst1

.Lrefillof:
	REFILL
	ld.w	%r10,%r6
	and	%r10,31
	xjp	.Lst2

.Lrefillof1:
	REFILL
	xjp	.Lof1

.Lrefillofbig:
	REFILL
	xjp	.Lofbig2

.Lrefilllong:
	REFILL
	xjp	.Loflong2
