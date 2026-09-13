/****************************************************************************
 * apps/system/bench/ubench.S
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/* Loops with nothing in them but the thing being measured.
 *
 * CoreMark answers "how fast is this machine" with one number, which is no
 * use for working out why the number is what it is: it fetches, loads,
 * stores and branches all at once.  These separate them.
 *
 * Every loop below has the same shape -- eight instructions of work, then a
 * decrement, a compare and a branch -- and every instruction in them is a
 * plain sixteen-bit one, so the loops differ in exactly one thing each.
 * That matters more than it sounds: the first version of this used the
 * extended forms, which are twice the size, so the loops fetched different
 * numbers of bytes per instruction and the answers came out backwards.  Here
 * only ub_wide changes the size, and it changes nothing else, which is what
 * makes it the measurement of what a byte of instruction costs.
 *
 * Each routine is aligned to thirty-two bytes, which puts every loop body
 * at the same offset from a boundary. Without that the numbers move by a
 * factor of four when an unrelated source file is added to the program,
 * because what is being measured is then partly where the linker happened
 * to put the loop -- a real effect on the device and a useless one in an
 * instrument meant to vary size alone. The alignment goes before the entry
 * and not inside: padding between the entry and the loop is executed, and
 * it is zeros.
 *
 * All of it is position independent -- relative branches, no literals, no
 * calls -- so the same instructions can be run out of internal RAM and out
 * of SDRAM and compared.  There is no instruction cache on this part, so
 * that difference is what fetching from SDRAM costs.
 *
 * %r6 is the pass count, %r7 the scratch buffer, and %r5 holds one.
 */

	.text
	.p2align 5
	.global	ub_block_start
	.type	ub_block_start,@function
ub_block_start:

/* Eight adds: no memory operands, sixteen-bit instructions throughout. */

	.p2align 5
	.global	ub_alu
	.type	ub_alu,@function
ub_alu:
	ld.w	%r5,1
1:
	.rept	8
	add	%r4,%r5
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_alu_end
ub_alu_end:

/* The same eight instructions with an immediate too large for the short
 * form, so the assembler has to use the extended one: thirty-two bits each,
 * identical work, twice the bytes to fetch.  A small immediate will not do
 * it -- "xadd %r4,1" assembles back down to sixteen bits and the loop comes
 * out the same size as the one above, which is what happened first.
 */

	.p2align 5
	.global	ub_wide
	.type	ub_wide,@function
ub_wide:
	ld.w	%r5,1
1:
	.rept	8
	xadd	%r4,1000
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_wide_end
ub_wide_end:

/* Eight word loads from one address, so the row stays open and what is left
 * is the cost of the load itself.
 */

/* The alu loop again, deliberately out of step: the same instructions half
 * a boundary along. Alignment is worth a factor of three and a half on the
 * device and four on the model, which is too much to leave as something
 * that happens to a measurement rather than something it reports.
 */

	.p2align 5
	.space	16
	.global	ub_alu_off
	.type	ub_alu_off,@function
ub_alu_off:
	ld.w	%r5,1
1:
	.rept	8
	add	%r4,%r5
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_alu_off_end
ub_alu_off_end:

	.p2align 5
	.global	ub_load
	.type	ub_load,@function
ub_load:
	ld.w	%r5,1
1:
	.rept	8
	ld.w	%r4,[%r7]
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_load_end
ub_load_end:

/* ...and eight stores, because writes and reads are not the same price:
 * ramspeed's memset came within half of the model where its memcpy was nine
 * times off it.
 */

	.p2align 5
	.global	ub_store
	.type	ub_store,@function
ub_store:
	ld.w	%r5,1
1:
	.rept	8
	ld.w	[%r7],%r4
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_store_end
ub_store_end:

/* Sixty-four adds before the branch rather than eight. A tight loop pays
 * for a branch every few instructions and a long run does not, so the two
 * together say what a taken branch costs the fetch path -- which on a part
 * that bursts sequential reads and restarts them after a jump is most of
 * the story.
 */

	.p2align 5
	.global	ub_straight
	.type	ub_straight,@function
ub_straight:
	ld.w	%r5,1
1:
	.rept	64
	add	%r4,%r5
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_straight_end
ub_straight_end:

/*
 * The same loop at a range of sizes. There is no instruction cache, but
 * there is a queue, and the size at which a loop stops fitting in it is
 * where the cost of a byte changes -- which is the one number that decides
 * whether a model of the fetch path has the right shape or merely the right
 * answer for one loop.
 */

/* 4 adds before the branch: one point on the size sweep. */

	.p2align 5
	.global	ub_s4
	.type	ub_s4,@function
ub_s4:
	ld.w	%r5,1
1:
	.rept	4
	add	%r4,%r5
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_s4_end
ub_s4_end:

/* 16 adds before the branch: one point on the size sweep. */

	.p2align 5
	.global	ub_s16
	.type	ub_s16,@function
ub_s16:
	ld.w	%r5,1
1:
	.rept	16
	add	%r4,%r5
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_s16_end
ub_s16_end:

/* 32 adds before the branch: one point on the size sweep. */

	.p2align 5
	.global	ub_s32
	.type	ub_s32,@function
ub_s32:
	ld.w	%r5,1
1:
	.rept	32
	add	%r4,%r5
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_s32_end
ub_s32_end:



/* Byte accesses, which nothing above measures: the C library's memcpy is a
 * byte at a time, and its cost in the model is 1.41x the device's.
 */

	.p2align 5
	.global	ub_loadb
	.type	ub_loadb,@function
ub_loadb:
	ld.w	%r5,1
1:
	.rept	8
	ld.b	%r4,[%r7]
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_loadb_end
ub_loadb_end:

	.p2align 5
	.global	ub_storeb
	.type	ub_storeb,@function
ub_storeb:
	ld.w	%r5,1
1:
	.rept	8
	ld.b	[%r7],%r4
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_storeb_end
ub_storeb_end:

/* ...and walking through memory rather than standing still. Everything
 * above reads and writes one address, so the row is always open and the
 * cost of changing one never appears -- which is the regime memcpy and
 * memset spend all their time in. Four words a pass, sixteen bytes, so the
 * caller must give it sixteen bytes per pass to walk through.
 */

	.p2align 5
	.global	ub_loadseq
	.type	ub_loadseq,@function
ub_loadseq:
	ld.w	%r5,1
1:
	ld.w	%r4,[%r7]
	xadd	%r7,4
	ld.w	%r4,[%r7]
	xadd	%r7,4
	ld.w	%r4,[%r7]
	xadd	%r7,4
	ld.w	%r4,[%r7]
	xadd	%r7,4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_loadseq_end
ub_loadseq_end:

	.p2align 5
	.global	ub_storeseq
	.type	ub_storeseq,@function
ub_storeseq:
	ld.w	%r5,1
1:
	ld.w	[%r7],%r4
	xadd	%r7,4
	ld.w	[%r7],%r4
	xadd	%r7,4
	ld.w	[%r7],%r4
	xadd	%r7,4
	ld.w	[%r7],%r4
	xadd	%r7,4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_storeseq_end
ub_storeseq_end:

	.global	ub_block_end
ub_block_end:
