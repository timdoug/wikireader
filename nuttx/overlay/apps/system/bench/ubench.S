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

/* Eight words a pass, addressed by displacement rather than by
 * post-increment, with one bump of each pointer at the end.
 *
 * This is the shape a compiler emits for an unrolled copy, and it is what
 * ramspeed's own memcpy turns into: eight of "*d32 = *s32; d32++; s32++"
 * with the increments folded into offsets. The model is 0.73 of the device
 * there while being 0.99 on copyw, which is the same work by
 * post-increment, so the difference is in the addressing and not in the
 * copying. Thirty-two bytes a pass.
 */

	.p2align 5
	.global	ub_copydisp
	.type	ub_copydisp,@function
ub_copydisp:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
1:
	xld.w	%r4,[%r7+0]
	xld.w	[%r8+0],%r4
	xld.w	%r4,[%r7+4]
	xld.w	[%r8+4],%r4
	xld.w	%r4,[%r7+8]
	xld.w	[%r8+8],%r4
	xld.w	%r4,[%r7+12]
	xld.w	[%r8+12],%r4
	xld.w	%r4,[%r7+16]
	xld.w	[%r8+16],%r4
	xld.w	%r4,[%r7+20]
	xld.w	[%r8+20],%r4
	xld.w	%r4,[%r7+24]
	xld.w	[%r8+24],%r4
	xld.w	%r4,[%r7+28]
	xld.w	[%r8+28],%r4
	xadd	%r7,32
	xadd	%r8,32
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_copydisp_end
ub_copydisp_end:

/* ...and the same copy a byte at a time, which is what both libraries fall
 * back to when the two addresses are not aligned alike, and what the C
 * library's memcpy does for every byte of a short one. Eight bytes a pass.
 */

	.p2align 5
	.global	ub_bytecopy
	.type	ub_bytecopy,@function
ub_bytecopy:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
1:
	.rept	8
	ld.ub	%r4,[%r7]+
	ld.b	[%r8]+,%r4
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bytecopy_end
ub_bytecopy_end:

/* The same byte copy, with the two streams no longer agreeing modulo four.
 *
 * arch_libctest found this and nothing here had ever measured it: stpncpy
 * runs at 18.2 cycles a byte on the device when source and destination are
 * congruent -- which is exactly what ub_bytecopy costs, 145.91 for eight
 * bytes -- and at 66.7 when they are not.  The model has no such cliff; it
 * charges 20.7 either way, so it is 3.2x fast on the second case.  Neither
 * side takes a misaligned-access trap, so this is the same instruction
 * sequence on the same number of accesses, and the device is paying about
 * 46 cycles a byte that nothing in the model accounts for.
 *
 * ub_bytecopy is the skew-0 control.  The source moves rather than the
 * destination so that the walk stays inside the buffer.
 *
 * Skew 2 is the one to watch.  If the penalty is about congruence it will
 * cost what 1 and 3 do; if it is about the width the controller can merge
 * accesses into, a halfword-aligned stream should sit between skew 0 and
 * skew 1.
 */

	.p2align 5
	.global	ub_bcskew1
	.type	ub_bcskew1,@function
ub_bcskew1:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	add	%r7,1
1:
	.rept	8
	ld.ub	%r4,[%r7]+
	ld.b	[%r8]+,%r4
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcskew1_end
ub_bcskew1_end:

	.p2align 5
	.global	ub_bcskew2
	.type	ub_bcskew2,@function
ub_bcskew2:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	add	%r7,2
1:
	.rept	8
	ld.ub	%r4,[%r7]+
	ld.b	[%r8]+,%r4
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcskew2_end
ub_bcskew2_end:

	.p2align 5
	.global	ub_bcskew3
	.type	ub_bcskew3,@function
ub_bcskew3:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	add	%r7,3
1:
	.rept	8
	ld.ub	%r4,[%r7]+
	ld.b	[%r8]+,%r4
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcskew3_end
ub_bcskew3_end:

/* Which half of the copy pays it: two byte streams read, and two written.
 *
 * The copy alternates a load against a store, so a penalty that needs two
 * incongruent streams could belong to either side or to the pairing.  These
 * four are the same walk with both accesses the same kind -- congruent and
 * skewed, read and write -- so whichever pair separates is the one that
 * carries it.  If neither does, it is the read-against-write pairing.
 */

	.p2align 5
	.global	ub_ld2
	.type	ub_ld2,@function
ub_ld2:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
1:
	.rept	8
	ld.ub	%r4,[%r7]+
	ld.ub	%r9,[%r8]+
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_ld2_end
ub_ld2_end:

	.p2align 5
	.global	ub_ld2skew
	.type	ub_ld2skew,@function
ub_ld2skew:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	add	%r7,1
1:
	.rept	8
	ld.ub	%r4,[%r7]+
	ld.ub	%r9,[%r8]+
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_ld2skew_end
ub_ld2skew_end:

	.p2align 5
	.global	ub_st2
	.type	ub_st2,@function
ub_st2:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
1:
	.rept	8
	ld.b	[%r7]+,%r4
	ld.b	[%r8]+,%r4
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_st2_end
ub_st2_end:

	.p2align 5
	.global	ub_st2skew
	.type	ub_st2skew,@function
ub_st2skew:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	add	%r7,1
1:
	.rept	8
	ld.b	[%r7]+,%r4
	ld.b	[%r8]+,%r4
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_st2skew_end
ub_st2skew_end:

/* Byte against word, on a loop that changes rows on every access.
 *
 * ld2 and rowthrash are both two read streams with a row change every
 * access, and the device charges 15.88 cycles for ld2's and 17.72 for
 * rowthrash's while the model charges 20.46 and 17.25.  So the device makes
 * a byte read 1.84 cheaper than a word one -- one SDCLK, which is the extra
 * halfword a word read waits for -- and the model makes it 3.21 dearer.
 * Any uniform overlap moves the two together and one of them breaks; a
 * same-kind overlap fitted to ld2 took rowthrash from 0.97 to 0.71.
 *
 * But the two differ in a second way: ld2 walks and rowthrash re-reads two
 * fixed addresses.  These close the square.  ld2bf is rowthrash with byte
 * loads, ld2w is ld2 with word loads, so each differs from an existing loop
 * in the access size alone and whichever pair separates says which of the
 * two it is.
 */

	.p2align 5
	.global	ub_ld2bf
	.type	ub_ld2bf,@function
ub_ld2bf:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
1:
	.rept	4
	ld.ub	%r4,[%r7]
	ld.ub	%r9,[%r8]
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_ld2bf_end
ub_ld2bf_end:

	.p2align 5
	.global	ub_ld2w
	.type	ub_ld2w,@function
ub_ld2w:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
1:
	.rept	8
	ld.w	%r4,[%r7]+
	ld.w	%r9,[%r8]+
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_ld2w_end
ub_ld2w_end:

/* The same copying, behind three different amounts of code.
 *
 * copydisp is the one loop the model is badly out on, at 1.38, and the
 * arithmetic says why: its sixteen accesses at the device's own 15.7 cycles
 * each come to 251, which is the whole of its 250.63, so the eighty-two
 * bytes of code it fetches every pass cost the device nothing. The model
 * charges about a hundred cycles for them. Somewhere in there the
 * controller is filling instruction lines while the data accesses are in
 * flight -- but a blanket overlap breaks bytecopy and store, which are
 * happy with the serial account, so it is conditional and the condition is
 * what these three measure.
 *
 * Each one copies the same sixteen bytes with the same eight accesses, and
 * they differ only in how many adds follow: sixteen, sixty-four, ninety-six,
 * which is 32, 128 and 192 bytes of extra code. Ninety-six is as far as the
 * loop's own branch reaches.
 *
 * An add is one cycle, so if fetching hides entirely behind the copying then
 * mix64 costs mix16 plus 48 and mix96 plus 80. If it does not hide at all,
 * each is that plus about twenty cycles for every sixteen bytes of code --
 * 168 and 280 more. There is no reading those two apart wrongly.
 *
 * %r9 takes the adds so that the copy's own registers are left alone.
 */

	.p2align 5
	.global	ub_mix16
	.type	ub_mix16,@function
ub_mix16:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
1:
	.rept	4
	ld.w	%r4,[%r7]+
	ld.w	[%r8]+,%r4
	.endr
	.rept	16
	add	%r9,%r5
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_mix16_end
ub_mix16_end:

	.p2align 5
	.global	ub_mix64
	.type	ub_mix64,@function
ub_mix64:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
1:
	.rept	4
	ld.w	%r4,[%r7]+
	ld.w	[%r8]+,%r4
	.endr
	.rept	64
	add	%r9,%r5
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_mix64_end
ub_mix64_end:

	.p2align 5
	.global	ub_mix96
	.type	ub_mix96,@function
ub_mix96:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
1:
	.rept	4
	ld.w	%r4,[%r7]+
	ld.w	[%r8]+,%r4
	.endr
	.rept	96
	add	%r9,%r5
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_mix96_end
ub_mix96_end:

/* What a register instruction costs inside a byte copy, and whether the
 * answer depends on how many there are or on how much room they take.
 *
 * stpncpy's byte loop costs the device 66.7 cycles a byte where memcpy's
 * costs 16.2, and the two differ only in carrying seven more register
 * instructions between the same load and store.  Seven cycles apiece is not
 * what a register instruction costs anywhere else here -- ub_alu says 1.38
 * -- and the model charges about one, which is why it reads that loop three
 * times too fast.
 *
 * These four take one byte a pass, as stpncpy does, and pad the body with
 * instructions that touch no memory.  Three of them hold the size per
 * instruction and vary the count; the fourth holds the count of bcpad4 and
 * pads to the size of bcpad8 using extended forms, which are four bytes
 * where the others are two.  If the cost follows the count, it is issue; if
 * it follows the size, it is fetch, and 32 bytes -- two queue lines -- is
 * where it should show.
 */

	.p2align 5
	.global	ub_bcpad0
	.type	ub_bcpad0,@function
ub_bcpad0:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
1:
	ld.ub	%r4,[%r7]+
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcpad0_end
ub_bcpad0_end:

	.p2align 5
	.global	ub_bcpad4
	.type	ub_bcpad4,@function
ub_bcpad4:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
1:
	ld.ub	%r4,[%r7]+
	.rept	4
	add	%r9,%r5
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcpad4_end
ub_bcpad4_end:

	.p2align 5
	.global	ub_bcpad8
	.type	ub_bcpad8,@function
ub_bcpad8:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
1:
	ld.ub	%r4,[%r7]+
	.rept	8
	add	%r9,%r5
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcpad8_end
ub_bcpad8_end:

	.p2align 5
	.global	ub_bcwide4
	.type	ub_bcwide4,@function
ub_bcwide4:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
1:
	ld.ub	%r4,[%r7]+
	.rept	4
	xadd	%r9,0x10000
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcwide4_end
ub_bcwide4_end:

/* Does the cost follow the number of data accesses, at one body size?
 *
 * bcpad said the device pays for the bytes of a loop body and not for the
 * instructions in it, and UBO says a 22-byte body with no data access at all
 * is free at every offset.  So size only costs when something in the loop
 * touches memory, and what is not known is whether one access is enough to
 * pay for the whole body or whether each one pays again.
 *
 * All three bodies are thirteen instructions and twenty-six bytes.  They
 * differ only in how many of those instructions are loads or stores: one,
 * two, four.  The loop is aligned by itself rather than inherited from the
 * entry, because the prologues are different lengths here and the offset of
 * the body inside its line would otherwise vary with them.
 *
 * Each pointer walks its own quarter of the buffer so that four accesses
 * cost four misses rather than four hits in one queue entry.
 */

	.p2align 5
	.global	ub_bcacc1
	.type	ub_bcacc1,@function
ub_bcacc1:
	ld.w	%r5,1
	.p2align 4
1:
	ld.ub	%r4,[%r7]+
	.rept	9
	add	%r9,%r5
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcacc1_end
ub_bcacc1_end:

	.p2align 5
	.global	ub_bcacc2
	.type	ub_bcacc2,@function
ub_bcacc2:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
1:
	ld.ub	%r4,[%r7]+
	.rept	8
	add	%r9,%r5
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcacc2_end
ub_bcacc2_end:

	.p2align 5
	.global	ub_bcacc4
	.type	ub_bcacc4,@function
ub_bcacc4:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x40000
	ld.w	%r10,%r7
	xadd	%r10,0x80000
	ld.w	%r11,%r7
	xadd	%r11,0xc0000
	.p2align 4
1:
	ld.ub	%r4,[%r7]+
	ld.b	[%r8]+,%r4
	ld.ub	%r12,[%r10]+
	ld.b	[%r11]+,%r12
	.rept	6
	add	%r9,%r5
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcacc4_end
ub_bcacc4_end:


/* One loop at every even offset inside the sixteen-byte fetch line.
 *
 * bcacc2 and bcpad8 are the same thirteen instructions and twenty-six bytes
 * with the same load and store, and the device runs one at 24.32 cycles and
 * the other at 60.08.  The difference is where the body sits in its line.
 *
 * The offset is set after the prologue, not by counting its length: the
 * first version of this assumed eight bytes where xadd is six, so it tested
 * 2, 6, 10 and 14 while claiming 0, 4, 8 and 12.  Aligning first and then
 * padding makes the offset what it says regardless.  The padding is zeros,
 * which this core decodes as nop, and it runs once per call rather than once
 * per pass.
 */

	.p2align 5
	.global	ub_bcal0
	.type	ub_bcal0,@function
ub_bcal0:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
1:
	ld.ub	%r4,[%r7]+
	.rept	8
	add	%r9,%r5
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcal0_end
ub_bcal0_end:

	.p2align 5
	.global	ub_bcal2
	.type	ub_bcal2,@function
ub_bcal2:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
	.rept	1
	ld.w	%r4,%r4
	.endr
1:
	ld.ub	%r4,[%r7]+
	.rept	8
	add	%r9,%r5
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcal2_end
ub_bcal2_end:

	.p2align 5
	.global	ub_bcal4
	.type	ub_bcal4,@function
ub_bcal4:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
	.rept	2
	ld.w	%r4,%r4
	.endr
1:
	ld.ub	%r4,[%r7]+
	.rept	8
	add	%r9,%r5
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcal4_end
ub_bcal4_end:

	.p2align 5
	.global	ub_bcal6
	.type	ub_bcal6,@function
ub_bcal6:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
	.rept	3
	ld.w	%r4,%r4
	.endr
1:
	ld.ub	%r4,[%r7]+
	.rept	8
	add	%r9,%r5
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcal6_end
ub_bcal6_end:

	.p2align 5
	.global	ub_bcal8
	.type	ub_bcal8,@function
ub_bcal8:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
	.rept	4
	ld.w	%r4,%r4
	.endr
1:
	ld.ub	%r4,[%r7]+
	.rept	8
	add	%r9,%r5
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcal8_end
ub_bcal8_end:

	.p2align 5
	.global	ub_bcal10
	.type	ub_bcal10,@function
ub_bcal10:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
	.rept	5
	ld.w	%r4,%r4
	.endr
1:
	ld.ub	%r4,[%r7]+
	.rept	8
	add	%r9,%r5
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcal10_end
ub_bcal10_end:

	.p2align 5
	.global	ub_bcal12
	.type	ub_bcal12,@function
ub_bcal12:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
	.rept	6
	ld.w	%r4,%r4
	.endr
1:
	ld.ub	%r4,[%r7]+
	.rept	8
	add	%r9,%r5
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcal12_end
ub_bcal12_end:

	.p2align 5
	.global	ub_bcal14
	.type	ub_bcal14,@function
ub_bcal14:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
	.rept	7
	ld.w	%r4,%r4
	.endr
1:
	ld.ub	%r4,[%r7]+
	.rept	8
	add	%r9,%r5
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcal14_end
ub_bcal14_end:

/* The size and offset surface, one load and one store throughout.
 *
 * Two edges of it are measured and they disagree about what the rule is.
 * A 26-byte body costs 24.32 cycles at offset 0 and 58 to 61 at every other
 * offset, which says alignment.  But a 10-byte body at offset 10 costs
 * 17.49 while an 18-byte one at the same offset costs 52.54, which says
 * size.  Neither alone predicts the other, so this fills the square in:
 * four sizes at four offsets, everything else held still.
 *
 * Bodies are ld.ub, N adds, ld.b, sub, cmp, jrne -- ten bytes plus two a
 * pad instruction.  The offset is set by aligning after the prologue and
 * then padding with zeros, which decode as nop and run once per call.
 */

	.p2align 5
	.global	ub_bcs10o0
	.type	ub_bcs10o0,@function
ub_bcs10o0:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
1:
	ld.ub	%r4,[%r7]+
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcs10o0_end
ub_bcs10o0_end:

	.p2align 5
	.global	ub_bcs10o4
	.type	ub_bcs10o4,@function
ub_bcs10o4:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
	.rept	2
	ld.w	%r4,%r4
	.endr
1:
	ld.ub	%r4,[%r7]+
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcs10o4_end
ub_bcs10o4_end:

	.p2align 5
	.global	ub_bcs10o8
	.type	ub_bcs10o8,@function
ub_bcs10o8:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
	.rept	4
	ld.w	%r4,%r4
	.endr
1:
	ld.ub	%r4,[%r7]+
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcs10o8_end
ub_bcs10o8_end:

	.p2align 5
	.global	ub_bcs10o12
	.type	ub_bcs10o12,@function
ub_bcs10o12:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
	.rept	6
	ld.w	%r4,%r4
	.endr
1:
	ld.ub	%r4,[%r7]+
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcs10o12_end
ub_bcs10o12_end:

	.p2align 5
	.global	ub_bcs18o0
	.type	ub_bcs18o0,@function
ub_bcs18o0:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
1:
	ld.ub	%r4,[%r7]+
	.rept	4
	add	%r9,%r5
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcs18o0_end
ub_bcs18o0_end:

	.p2align 5
	.global	ub_bcs18o4
	.type	ub_bcs18o4,@function
ub_bcs18o4:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
	.rept	2
	ld.w	%r4,%r4
	.endr
1:
	ld.ub	%r4,[%r7]+
	.rept	4
	add	%r9,%r5
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcs18o4_end
ub_bcs18o4_end:

	.p2align 5
	.global	ub_bcs18o8
	.type	ub_bcs18o8,@function
ub_bcs18o8:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
	.rept	4
	ld.w	%r4,%r4
	.endr
1:
	ld.ub	%r4,[%r7]+
	.rept	4
	add	%r9,%r5
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcs18o8_end
ub_bcs18o8_end:

	.p2align 5
	.global	ub_bcs18o12
	.type	ub_bcs18o12,@function
ub_bcs18o12:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
	.rept	6
	ld.w	%r4,%r4
	.endr
1:
	ld.ub	%r4,[%r7]+
	.rept	4
	add	%r9,%r5
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcs18o12_end
ub_bcs18o12_end:

	.p2align 5
	.global	ub_bcs26o0
	.type	ub_bcs26o0,@function
ub_bcs26o0:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
1:
	ld.ub	%r4,[%r7]+
	.rept	8
	add	%r9,%r5
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcs26o0_end
ub_bcs26o0_end:

	.p2align 5
	.global	ub_bcs26o4
	.type	ub_bcs26o4,@function
ub_bcs26o4:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
	.rept	2
	ld.w	%r4,%r4
	.endr
1:
	ld.ub	%r4,[%r7]+
	.rept	8
	add	%r9,%r5
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcs26o4_end
ub_bcs26o4_end:

	.p2align 5
	.global	ub_bcs26o8
	.type	ub_bcs26o8,@function
ub_bcs26o8:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
	.rept	4
	ld.w	%r4,%r4
	.endr
1:
	ld.ub	%r4,[%r7]+
	.rept	8
	add	%r9,%r5
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcs26o8_end
ub_bcs26o8_end:

	.p2align 5
	.global	ub_bcs26o12
	.type	ub_bcs26o12,@function
ub_bcs26o12:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
	.rept	6
	ld.w	%r4,%r4
	.endr
1:
	ld.ub	%r4,[%r7]+
	.rept	8
	add	%r9,%r5
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcs26o12_end
ub_bcs26o12_end:

	.p2align 5
	.global	ub_bcs34o0
	.type	ub_bcs34o0,@function
ub_bcs34o0:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
1:
	ld.ub	%r4,[%r7]+
	.rept	12
	add	%r9,%r5
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcs34o0_end
ub_bcs34o0_end:

	.p2align 5
	.global	ub_bcs34o4
	.type	ub_bcs34o4,@function
ub_bcs34o4:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
	.rept	2
	ld.w	%r4,%r4
	.endr
1:
	ld.ub	%r4,[%r7]+
	.rept	12
	add	%r9,%r5
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcs34o4_end
ub_bcs34o4_end:

	.p2align 5
	.global	ub_bcs34o8
	.type	ub_bcs34o8,@function
ub_bcs34o8:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
	.rept	4
	ld.w	%r4,%r4
	.endr
1:
	ld.ub	%r4,[%r7]+
	.rept	12
	add	%r9,%r5
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcs34o8_end
ub_bcs34o8_end:

	.p2align 5
	.global	ub_bcs34o12
	.type	ub_bcs34o12,@function
ub_bcs34o12:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
	.p2align 4
	.rept	6
	ld.w	%r4,%r4
	.endr
1:
	ld.ub	%r4,[%r7]+
	.rept	12
	add	%r9,%r5
	.endr
	ld.b	[%r8]+,%r4
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_bcs34o12_end
ub_bcs34o12_end:

/* What a byte of instruction costs a program with a real code footprint.
 *
 * Everything above is a loop small enough to argue about: the queue holds
 * two sixteen-byte lines, so a body of 26 or 42 bytes is partly resident
 * and what it measures is residency rather than fetch.  The model splits
 * the difference -- iqb_first is four half-MCLK because "the micro loops
 * want eight and Dhrystone wants zero" -- and nothing here could say which
 * end was right, because nothing here had a footprint.
 *
 * The rv32 interpreter built entirely into SDRAM does, and the device runs
 * it 10% cheaper than the model does, uniformly across ten guest workloads.
 * These extend the size sweep past the point where residency can help at
 * all: at 266 and 522 bytes every fetch misses, so the cost per byte is the
 * asymptote and the difference from ub_straight's 138 is what one fill
 * costs.  Two points beyond the knee is what separates them.
 *
 * They stay under a kilobyte on purpose.  A body that straddles a 1 KB page
 * costs 3.2x, which would be measuring iq_row_evict instead.  They are also
 * SDRAM-only and last in the block: the internal-RAM copy reaches the end
 * of the last loop that will run from there, so a case added here can never
 * be one of those.
 */

	.p2align 5
	.global	ub_f128
	.type	ub_f128,@function
ub_f128:
	ld.w	%r5,1
1:
	.rept	128
	add	%r4,%r5
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	xjrne	1b			/* jrne reaches +-256 bytes; this body is longer */
	ret
	.global	ub_f128_end
ub_f128_end:

	.p2align 5
	.global	ub_f256
	.type	ub_f256,@function
ub_f256:
	ld.w	%r5,1
1:
	.rept	256
	add	%r4,%r5
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	xjrne	1b			/* jrne reaches +-256 bytes; this body is longer */
	ret
	.global	ub_f256_end
ub_f256_end:

/* The same instruction count as ub_straight with a taken branch every
 * other one, so the pair prices a taken branch in SDRAM on its own.
 *
 * branch_taken is 5 and was fitted where branches were a small part of
 * what was being measured; here they are half of it.  The jump goes to the
 * next instruction, which is the cheapest target there is -- no row change,
 * the line already being fetched -- so what is left is what the fetch path
 * pays for restarting, and nothing else.
 */

	.p2align 5
	.global	ub_br32
	.type	ub_br32,@function
ub_br32:
	ld.w	%r5,1
1:
	.rept	32
	add	%r4,%r5
	jp	9f
9:
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_br32_end
ub_br32_end:

/* What it is about two streams that this model gets wrong.
 *
 * ld2 and st2 -- two streams read alternately, both walking forward -- are
 * the loops the model overcharges most, by a quarter to a third.  Nothing
 * else in the file is: one stream walking is right (loadseq 0.993), and two
 * addresses alternated while standing still is right (rowthrash 1.056).
 * Queue depth, the row-change price and the write buffer have each been
 * tried against that and are each refuted, so the thing to do is take ld2
 * apart one property at a time rather than guess at another parameter.
 *
 * Every loop below is ld2 with exactly one property changed: the same
 * sixteen byte reads a pass, the same loop, the same pass count, so any
 * difference is the property and not the shape.
 *
 *   ld2fix    the two addresses stand still        (walking is the thing?)
 *   ld1walk   one stream instead of two            (two is the thing?)
 *   ld2mix    one walks, one stands still          (which of them matters?)
 *   ld2near   the two streams a row apart, not 512K   (separation?)
 *   ld2far    ...and four megabytes apart, another bank
 */

	.p2align 5
	.global	ub_ld2fix
	.type	ub_ld2fix,@function
ub_ld2fix:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
1:
	.rept	8
	ld.ub	%r4,[%r7]
	ld.ub	%r9,[%r8]
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_ld2fix_end
ub_ld2fix_end:

	.p2align 5
	.global	ub_ld1walk
	.type	ub_ld1walk,@function
ub_ld1walk:
	ld.w	%r5,1
1:
	.rept	16
	ld.ub	%r4,[%r7]+
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_ld1walk_end
ub_ld1walk_end:

	.p2align 5
	.global	ub_ld2mix
	.type	ub_ld2mix,@function
ub_ld2mix:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
1:
	.rept	8
	ld.ub	%r4,[%r7]+
	ld.ub	%r9,[%r8]
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_ld2mix_end
ub_ld2mix_end:

	.p2align 5
	.global	ub_ld2near
	.type	ub_ld2near,@function
ub_ld2near:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x400
1:
	.rept	8
	ld.ub	%r4,[%r7]+
	ld.ub	%r9,[%r8]+
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_ld2near_end
ub_ld2near_end:

	.p2align 5
	.global	ub_ld2far
	.type	ub_ld2far,@function
ub_ld2far:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x400000
1:
	.rept	8
	ld.ub	%r4,[%r7]+
	ld.ub	%r9,[%r8]+
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_ld2far_end
ub_ld2far_end:

/* Is it the code footprint and the row changes together?
 *
 * Sorted by whether the body outruns the instruction queue and whether the
 * data changes rows, every loop in this file falls where it should: the
 * model overcharges by a fifth to a third exactly when both are true, and
 * is right within 2% when either is false.  rowthrash alternates two rows
 * in a 32-byte body and fits; ld2fix alternates two rows in a 50-byte body
 * and is 0.782.  Nothing else distinguishes them -- not walking, not the
 * separation, not the access width, all of which have been measured and
 * ruled out.
 *
 * These two cross the pair over.  ld2small is ld2fix with a body small
 * enough to stay resident and nothing else changed; rtbig is rowthrash with
 * adds padding its body past the window and its accesses untouched.  If the
 * reading is right, ld2small comes back near 1.0 and rtbig drops to about
 * 0.8, and the fault is bus contention between fetch and data rather than
 * anything in the data path on its own.
 */

	.p2align 5
	.global	ub_ld2small
	.type	ub_ld2small,@function
ub_ld2small:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
1:
	.rept	2
	ld.ub	%r4,[%r7]
	ld.ub	%r9,[%r8]
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_ld2small_end
ub_ld2small_end:

	.p2align 5
	.global	ub_rtbig
	.type	ub_rtbig,@function
ub_rtbig:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,1024
1:
	.rept	4
	ld.w	%r4,[%r7]
	ld.w	%r4,[%r8]
	.endr
	.rept	12
	add	%r9,%r5
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_rtbig_end
ub_rtbig_end:

/*
 * How often a row changes, at a fixed number of accesses.
 *
 * ld2 changes rows on every access and ld1walk on almost none, and between
 * those two the cost of a change and the cost of an access cannot be told
 * apart: one number, two unknowns.  These take N bytes from one stream
 * before N from another half a megabyte away -- a row change either way,
 * whatever the geometry says -- so a pass makes the same sixteen accesses
 * with 8, 4 or 2 row changes among them.  Same instruction count and same
 * body length as ld2, so the instruction queue behaves identically and only
 * the rate differs.  Cost per access against changes per access is then a
 * straight line whose slope is the change and whose intercept is the access.
 */

	.p2align 5
	.global	ub_rowrate2
	.type	ub_rowrate2,@function
ub_rowrate2:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
1:
	.rept	4
	ld.ub	%r4,[%r7]+
	ld.ub	%r4,[%r7]+
	ld.ub	%r9,[%r8]+
	ld.ub	%r9,[%r8]+
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_rowrate2_end
ub_rowrate2_end:

	.p2align 5
	.global	ub_rowrate4
	.type	ub_rowrate4,@function
ub_rowrate4:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
1:
	.rept	2
	.rept	4
	ld.ub	%r4,[%r7]+
	.endr
	.rept	4
	ld.ub	%r9,[%r8]+
	.endr
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_rowrate4_end
ub_rowrate4_end:

	.p2align 5
	.global	ub_rowrate8
	.type	ub_rowrate8,@function
ub_rowrate8:
	ld.w	%r5,1
	ld.w	%r8,%r7
	xadd	%r8,0x80000
1:
	.rept	8
	ld.ub	%r4,[%r7]+
	.endr
	.rept	8
	ld.ub	%r9,[%r8]+
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	jrne	1b
	ret
	.global	ub_rowrate8_end
ub_rowrate8_end:

/*
 * Instruction fetch that outgrows a row.
 *
 * f128 and f256 are 256 and 512 bytes of straight line, so they fetch
 * through several queue lines but never leave one 1 KB row.  This one is
 * 2 KB and row-aligned, so half its fetches land in the row after the one
 * before them.  Real code does this constantly and the tiny loops here never
 * do, which is the gap the model has to be right about: the device gained a
 * quarter of its CoreMark from faster row changes where the model expected
 * a twenty-fifth.
 */

	.p2align 10
	.global	ub_f1024
	.type	ub_f1024,@function
ub_f1024:
	ld.w	%r5,1
1:
	.rept	1024
	add	%r4,%r5
	.endr
	sub	%r6,%r5
	cmp	%r6,0
	xjrne	1b			/* jrne reaches +-256 bytes; this body is longer */
	ret
	.global	ub_f1024_end
ub_f1024_end:

	.global	ub_block_end
ub_block_end:

/*
 * Reprogram the controller's timing, from somewhere that is not SDRAM.
 *
 * Outside the measured block on purpose: everything above is placed where it
 * is and the numbers move when that changes, so this goes after the end
 * marker and is copied to internal RAM on its own rather than by the block
 * offset every loop shares.
 *
 * The rules are Grifo's SuspendCode's: no stack, no calls, no globals, and
 * no SDRAM access between the write and the settling.  Interrupts go off
 * first because the vector table and every handler behind it live in SDRAM,
 * and come back on only if they were on.  %r6 is the configuration register,
 * %r7 the refresh register, in that order because that is the order
 * II.4.1.5 initialises them in.
 */

	.p2align 5
	.global	ub_retime
	.type	ub_retime,@function
ub_retime:
	ld.w	%r4,%psr
	and	%r4,0x10
	psrclr	0x4
	xld.w	%r5,0x301604
	ld.w	[%r5],%r6
	xld.w	%r5,0x301608
	ld.w	[%r5],%r7
	/* Settle, out of a counter rather than a run of nops: a nop on this
	   part assembles to a zero halfword, and sixty-four of them in a row
	   is indistinguishable from a guest that has jumped into erased
	   memory -- which the emulator stops on, rightly. */
	xld.w	%r8,64
	ld.w	%r9,1
2:
	sub	%r8,%r9
	cmp	%r8,0
	jrne	2b
	cmp	%r4,0
	jreq	1f
	psrset	0x4
1:
	ret
	.global	ub_retime_end
ub_retime_end:
