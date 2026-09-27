/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The loops iram_bench.c times.  They are in .iram.text, so each exists
 * twice, as linked in SDRAM and as copied into A0 RAM, and the benchmark
 * runs both copies.  Each loop body is longer than the fetch queue holds.
 */
#include <linux/linkage.h>

	.section .iram.text,"ax"

/* void c33_bench_alu(unsigned int passes): 16 register adds a pass */
SYM_FUNC_START(c33_bench_alu)
1:
	add	%r9,%r10
	add	%r10,%r11
	add	%r11,%r12
	add	%r12,%r10
	add	%r9,%r11
	add	%r10,%r12
	add	%r11,%r10
	add	%r12,%r11
	add	%r9,%r12
	add	%r10,%r10
	add	%r11,%r11
	add	%r12,%r12
	add	%r9,%r10
	add	%r10,%r11
	add	%r11,%r12
	add	%r12,%r10
	sub	%r6,1
	jrne	1b
	ret
SYM_FUNC_END(c33_bench_alu)

/* void c33_bench_load(const u32 *p, unsigned int blocks): 8 words a block */
SYM_FUNC_START(c33_bench_load)
1:
	ld.w	%r9,[%r6]+
	ld.w	%r10,[%r6]+
	ld.w	%r11,[%r6]+
	ld.w	%r12,[%r6]+
	ld.w	%r13,[%r6]+
	ld.w	%r14,[%r6]+
	ld.w	%r9,[%r6]+
	ld.w	%r10,[%r6]+
	sub	%r7,1
	jrne	1b
	ret
SYM_FUNC_END(c33_bench_load)

/* void c33_bench_store(u32 *p, unsigned int blocks, u32 v): 8 words a block */
SYM_FUNC_START(c33_bench_store)
1:
	ld.w	[%r6]+,%r8
	ld.w	[%r6]+,%r8
	ld.w	[%r6]+,%r8
	ld.w	[%r6]+,%r8
	ld.w	[%r6]+,%r8
	ld.w	[%r6]+,%r8
	ld.w	[%r6]+,%r8
	ld.w	[%r6]+,%r8
	sub	%r7,1
	jrne	1b
	ret
SYM_FUNC_END(c33_bench_store)

/*
 * u32 c33_bench_lookup(const u32 *p, unsigned int words, const u16 *table):
 * a word from @p, then its four bytes looked up in @table, as the SD CRC does
 */
SYM_FUNC_START(c33_bench_lookup)
	ld.w	%r4,0
1:	ld.w	%r9,[%r6]+
	ld.w	%r10,%r9
	xsrl	%r10,24
	ld.ub	%r10,%r10
	add	%r10,%r10
	add	%r10,%r8
	ld.uh	%r10,[%r10]
	xor	%r4,%r10
	ld.w	%r10,%r9
	xsrl	%r10,16
	ld.ub	%r10,%r10
	add	%r10,%r10
	add	%r10,%r8
	ld.uh	%r10,[%r10]
	xor	%r4,%r10
	ld.w	%r10,%r9
	xsrl	%r10,8
	ld.ub	%r10,%r10
	add	%r10,%r10
	add	%r10,%r8
	ld.uh	%r10,[%r10]
	xor	%r4,%r10
	ld.w	%r10,%r9
	ld.ub	%r10,%r10
	add	%r10,%r10
	add	%r10,%r8
	ld.uh	%r10,[%r10]
	xor	%r4,%r10
	sub	%r7,1
	jrne	1b
	ret
SYM_FUNC_END(c33_bench_lookup)
