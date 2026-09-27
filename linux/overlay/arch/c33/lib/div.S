/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Unsigned division, run from A0 RAM.
 *
 * The C33 PE has no divide instructions, so every division is a routine:
 * libgcc's __udivsi3 and __umodsi3, and lib/math/div64.c's __div64_32 behind
 * do_div(), which the scheduler's load tracking calls on every enqueue,
 * dequeue and tick.  Those are C, run from SDRAM at several cycles an
 * instruction, and __div64_32 does 64-bit shifts and compares a bit at a
 * time; together they were 4.4% of the boot.  These replace them: the same
 * shift-and-subtract, in registers and from A0 RAM (asm/iram.h), through
 * stubs like lib/string.S's.  They are position-independent: every branch
 * is PC-relative and they call nothing.
 *
 * C ABI: arguments in %r6-%r7, the result in %r4, %r4-%r14 free.
 */
#include <linux/export.h>
#include <linux/linkage.h>

	.data
	.balign	4
SYM_DATA(c33_udivsi3_fn, .long __c33_udivsi3)
SYM_DATA(c33_umodsi3_fn, .long __c33_umodsi3)
SYM_DATA(c33_div64_32_fn, .long __c33_div64_32)

	.text
SYM_FUNC_START(__udivsi3)
	xld.w	%r9,c33_udivsi3_fn
	ld.w	%r9,[%r9]
	jp	%r9
SYM_FUNC_END(__udivsi3)
EXPORT_SYMBOL(__udivsi3)

SYM_FUNC_START(__umodsi3)
	xld.w	%r9,c33_umodsi3_fn
	ld.w	%r9,[%r9]
	jp	%r9
SYM_FUNC_END(__umodsi3)
EXPORT_SYMBOL(__umodsi3)

SYM_FUNC_START(__div64_32)
	xld.w	%r9,c33_div64_32_fn
	ld.w	%r9,[%r9]
	jp	%r9
SYM_FUNC_END(__div64_32)
EXPORT_SYMBOL(__div64_32)

	.section .iram.text,"ax"

/*
 * \n / \d, leaving the quotient in \q and the remainder in \n; \bit and
 * \t are scratch.  \d is doubled up to the largest multiple of it not past
 * \n, then taken off again a bit at a time, so the loops run once for each
 * bit of the quotient rather than 32 times.  \d is not zero, and is used
 * up.
 */
.macro	udiv32	n, d, q, bit, t
	ld.w	\q,0
	cmp	\n,\d
	jrult	3f			/* quotient 0, remainder \n */
	ld.w	\bit,1
1:	cmp	\d,0
	jrlt	2f			/* \d cannot double */
	ld.w	\t,\d
	add	\t,\t
	cmp	\t,\n
	jrugt	2f
	ld.w	\d,\t
	add	\bit,\bit
	jp	1b
2:	cmp	\n,\d
	jrult	4f
	sub	\n,\d
	or	\q,\bit
4:	srl	\d,1
	srl	\bit,1
	jrne	2b
3:
.endm

/* u32 __udivsi3(u32 n, u32 d) */
SYM_FUNC_START(__c33_udivsi3)
	cmp	%r7,0
	jreq	9f
	udiv32	%r6, %r7, %r4, %r9, %r10
	ret
9:	ld.w	%r4,0
	ret
SYM_FUNC_END(__c33_udivsi3)

/* u32 __umodsi3(u32 n, u32 d) */
SYM_FUNC_START(__c33_umodsi3)
	cmp	%r7,0
	jreq	9f
	udiv32	%r6, %r7, %r4, %r9, %r10
9:	ld.w	%r4,%r6
	ret
SYM_FUNC_END(__c33_umodsi3)

/*
 * u32 __div64_32(u64 *n, u32 base): *n /= base, returning the remainder.
 * The high word goes first, a 32-bit division; what it leaves is below
 * @base, so the rest of the quotient fits in 32 bits and comes from 32
 * steps of shifting the 64-bit remainder left through the carry and taking
 * @base off whenever it fits.
 */
SYM_FUNC_START(__c33_div64_32)
	ld.w	%r10,[%r6]		/* low word */
	xld.w	%r11,[%r6+4]		/* high word */
	ld.w	%r12,0			/* high word of the quotient */
	cmp	%r7,0
	jreq	9f
	cmp	%r11,%r7
	jrult	5f
	ld.w	%r14,%r7		/* udiv32 uses up its divisor */
	udiv32	%r11, %r14, %r12, %r9, %r13
5:	xld.w	%r13,32
6:	add	%r10,%r10
	adc	%r11,%r11
	jrult	7f			/* a bit carried out: past base */
	cmp	%r11,%r7
	jrult	8f
7:	sub	%r11,%r7
	or	%r10,1
8:	sub	%r13,1
	jrne	6b
9:	ld.w	[%r6],%r10
	xld.w	[%r6+4],%r12
	ld.w	%r4,%r11
	ret
SYM_FUNC_END(__c33_div64_32)
