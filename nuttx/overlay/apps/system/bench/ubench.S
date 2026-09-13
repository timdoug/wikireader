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


/* Two streams at once, which is what a copy is: read here, write half a
 * megabyte away, and advance both. One walking stream leaves one row open
 * and this leaves two, in whichever banks the two addresses land in -- the
 * regime memcpy spends all its time in, and the one the model is 1.41x fast
 * on.
 *
 * Words, not bytes. The memcpy this board links is the architecture's, not
 * the byte loop in libs/libc/string that CONFIG_LIBC_ARCH_MEMCPY leaves out
 * of the build, and its inner loop is "ld.w %r9,[%r7]+ / ld.w [%r6]+,%r9".
 */

	.p2align 5
	.global	ub_copyb
	.type	ub_copyb,@function
ub_copyb:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
1:
	.rept	4
	ld.w	%r4,[%r7]+
	ld.w	[%r8]+,%r4
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_copyb_end
ub_copyb_end:

/* The same copy with the two streams four megabytes apart instead of half a
 * megabyte. The bank bits start at four megabytes on this geometry, so this
 * pair lands in different banks and both rows can stay open, where the pair
 * above lands in one bank and every alternation closes the other's row.
 *
 * If the device is much faster here, the cost of a copy is row thrashing
 * and belongs in the row accounting; if it is the same, it is bus
 * turnaround and wants a parameter of its own.
 */

	.p2align 5
	.global	ub_copyfar
	.type	ub_copyfar,@function
ub_copyfar:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x400000
1:
	.rept	4
	ld.w	%r4,[%r7]+
	ld.w	[%r8]+,%r4
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_copyfar_end
ub_copyfar_end:

/* Two ext prefixes on every instruction instead of one.
 *
 * ub_wide is eight instructions of four bytes where ub_alu is eight of two,
 * and the device charges twelve cycles a pass more for them from internal
 * RAM where the model charges six and a half. That gap is either the extra
 * halfword to fetch or the ext prefix itself, which is a separate
 * instruction on this part rather than a field of the one it extends, and
 * the two cannot be told apart from one point. Six bytes an instruction
 * gives the second point: whatever an ext costs, this pays it twice.
 */

	.p2align 5
	.global	ub_ext2
	.type	ub_ext2,@function
ub_ext2:
	ld.w	%r5,1
1:
	.rept	8
	xadd	%r4,0x100000
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_ext2_end
ub_ext2_end:

/* Two addresses a kilobyte apart, read alternately: one row of one bank
 * against another row of the same bank, so every access is a row miss and
 * what it measures is tRP + tRCD + CAS with nothing else in it. Everything
 * else here either sits in one row or walks through rows at a rate the loop
 * decides; this is the only loop that pays for a row change every time.
 */

	.p2align 5
	.global	ub_rowthrash
	.type	ub_rowthrash,@function
ub_rowthrash:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,1024
1:
	.rept	4
	ld.w	%r4,[%r7]
	ld.w	%r4,[%r8]
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_rowthrash_end
ub_rowthrash_end:

/* The same alternation four megabytes apart, which on this geometry is the
 * bank stride: if the two rows can stay open at once this costs what reading
 * one address costs, and if the controller holds one row for the whole
 * device it costs what the loop above costs. The emulator's geometry table
 * says the first, on the strength of a measurement made for the reader's
 * buffer placement, and the copy loops have since given reason to ask again.
 */

	.p2align 5
	.global	ub_bankpair
	.type	ub_bankpair,@function
ub_bankpair:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x400000
1:
	.rept	4
	ld.w	%r4,[%r7]
	ld.w	%r4,[%r8]
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bankpair_end
ub_bankpair_end:

/* The C library's memcpy, shape for shape: one word a turn round the loop
 * with a compare and a branch, where copyw does four with one.
 *
 * The device disagrees with itself about a copy. ramspeed puts the system
 * memcpy at 8866 KB/s, which is 26.4 cycles a word, and ub_copyb -- the same
 * two post-incrementing accesses, unrolled four times -- at 31.0. The model
 * cannot be right about both, and it matters which is real: memcpy is the
 * operation the machine actually spends its time in. If this loop comes back
 * near 26 the unrolling is what costs, and if it comes back near 31 the
 * fifteen percent is ramspeed's ten millisecond clock.
 *
 * Four bytes a pass, so the caller must give it four bytes per pass.
 */

	.p2align 5
	.global	ub_copyloop
	.type	ub_copyloop,@function
ub_copyloop:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
1:
	ld.w	%r4,[%r7]+
	ld.w	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_copyloop_end
ub_copyloop_end:

/* A load with a displacement rather than a bare register, which is what a
 * compiler emits for a structure field and a stack slot and is therefore most
 * of the loads in CoreMark and Dhrystone. Against ub_load, which addresses
 * [%r7] directly, the difference is what the ext prefix and the addition
 * cost on a load rather than on an add.
 */

	.p2align 5
	.global	ub_lddisp
	.type	ub_lddisp,@function
ub_lddisp:
	ld.w	%r5,1
1:
	.rept	8
	xld.w	%r4,[%r7+0x100]
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_lddisp_end
ub_lddisp_end:

/* Multiply, which the cost table puts at seven clocks and nothing has ever
 * checked. Whetstone and Dhrystone are 0.94-0.97 of the device and the error
 * leans the same way in all three of the big benchmarks, which is the shape
 * of one instruction class being wrong rather than the memory.
 */

	.p2align 5
	.global	ub_mult
	.type	ub_mult,@function
ub_mult:
	ld.w	%r5,1
1:
	.rept	8
	mlt.w	%r4,%r5
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_mult_end
ub_mult_end:

/* Call and return, at four clocks each by the table, plus whatever the stack
 * traffic costs -- and the stack is somewhere the rest of this file never
 * touches. Dhrystone is a procedure-call benchmark before it is anything
 * else.
 */

	.p2align 5
	.global	ub_callret
	.type	ub_callret,@function
ub_callret:
	ld.w	%r5,1
1:
	.rept	8
	call	2f
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
2:
	ret
	.global	ub_callret_end
ub_callret_end:

/* Push and pop, two clocks each by the table, and four words of stack
 * traffic a pass. Every call in a compiled program is wrapped in these.
 */

	.p2align 5
	.global	ub_pushpop
	.type	ub_pushpop,@function
ub_pushpop:
	ld.w	%r5,1
1:
	.rept	4
	push	%r4
	pop	%r4
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_pushpop_end
ub_pushpop_end:

/* Reading a peripheral register, which nothing else here does.
 *
 * The card benchmark says the model is 25% slow on reads, and the profile
 * says that time is not the card: the payload is a sixth of it and the rest
 * is the driver, running the same code the device runs in 73% of the
 * cycles. A driver is a loop over registers, and every register access in
 * this machine -- the display, the touch panel, the card, the serial port --
 * goes through the same bus, which no measurement has ever priced.
 *
 * The SDRAM controller's timing register is the one to read: it is on the
 * peripheral bus like the rest, it has no side effects at all, and ubench
 * already reads it once for the header it prints.
 */

	.p2align 5
	.global	ub_mmio
	.type	ub_mmio,@function
ub_mmio:
	ld.w	%r5,1
	xld.w	%r8,0x301604
1:
	.rept	8
	ld.w	%r4,[%r8]
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_mmio_end
ub_mmio_end:

/* Eight consecutive words a pass where loadseq reads four.
 *
 * loadseq is the loop the model is furthest out on, at 1.21 of the device:
 * reads that walk through an open row. If the controller pipelines them --
 * one CAS after another without paying the full latency each time -- then
 * doubling the run should cost less than twice as much, and the model, which
 * charges every read in full, would be wrong by more here than there. If it
 * costs exactly twice, there is no pipelining and the error is elsewhere.
 *
 * Thirty-two bytes a pass, so the caller must give it thirty-two per pass.
 */

	.p2align 5
	.global	ub_loadseq8
	.type	ub_loadseq8,@function
ub_loadseq8:
	ld.w	%r5,1
1:
	.rept	8
	ld.w	%r4,[%r7]+
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_loadseq8_end
ub_loadseq8_end:

	.global	ub_block_end
ub_block_end:
