/****************************************************************************
 * apps/system/bench/ubench.S
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/* Loops with nothing in them but the thing being measured.
 *
 * CoreMark answers "how fast is this machine" with one number, which is no
 * use for working out why the number is what it is: it fetches, loads,
 * stores and branches all at once.  These separate them.  Every routine
 * takes an iteration count in %r6 and runs a fixed, known number of
 * instructions per pass, so the time divided by the count is the cost of
 * exactly one thing.
 *
 * All of it is position independent -- relative branches, no literals, no
 * calls -- because the point of the exercise is to run the same instructions
 * out of internal RAM and out of SDRAM and compare.  There is no instruction
 * cache on this part, so every fetch is a memory access, and the difference
 * between those two runs is what an instruction fetch costs.
 */

	.text
	.global	ub_block_start
	.type	ub_block_start,@function
ub_block_start:

/* Four adds and the loop: seven instructions, no memory operands at all.
 * Whatever this costs above four adds is fetch and branch.
 */

	.global	ub_alu
	.type	ub_alu,@function
ub_alu:
1:
	xadd	%r4,1
	xadd	%r4,1
	xadd	%r4,1
	xadd	%r4,1
	xsub	%r6,1
	cmp	%r6,0
	jrne	1b
	ret
	.size	ub_alu,.-ub_alu

/* The same loop body with four word loads from one place, so the row stays
 * open and what is left is the cost of issuing a load.
 */

	.global	ub_load
	.type	ub_load,@function
ub_load:
1:
	ld.w	%r4,[%r7]
	ld.w	%r4,[%r7]
	ld.w	%r4,[%r7]
	ld.w	%r4,[%r7]
	xsub	%r6,1
	cmp	%r6,0
	jrne	1b
	ret
	.size	ub_load,.-ub_load

/* ...and four stores to one place, because writes and reads are not the
 * same price: ramspeed's memset came within half of the model where its
 * memcpy was nine times off it.
 */

	.global	ub_store
	.type	ub_store,@function
ub_store:
1:
	ld.w	[%r7],%r4
	ld.w	[%r7],%r4
	ld.w	[%r7],%r4
	ld.w	[%r7],%r4
	xsub	%r6,1
	cmp	%r6,0
	jrne	1b
	ret
	.size	ub_store,.-ub_store

/* Sixty-four adds in a row before the branch. A tight loop re-reads the
 * same few words and should find the row open every time; a long run of
 * straight-line code walks through memory instead. The difference is what
 * locality is worth to the fetch path.
 */

	.global	ub_straight
	.type	ub_straight,@function
ub_straight:
1:
	.rept	64
	xadd	%r4,1
	.endr
	xsub	%r6,1
	cmp	%r6,0
	jrne	1b
	ret
	.size	ub_straight,.-ub_straight

	.global	ub_block_end
	.type	ub_block_end,@function
ub_block_end:
	.size	ub_block_start,.-ub_block_start
