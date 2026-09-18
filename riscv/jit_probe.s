; jit_probe.s - what translated code would cost, if there were a translator.
;
; The interpreter's budget is known: about seventy cycles a guest instruction
; on silicon, and the measurements in README.md say roughly two thirds of that
; is decode and dispatch -- work a translator does once instead of every time.
; What is not known is what the other end looks like, because the model is
; least trustworthy exactly there: SDRAM-resident code is the regime
; emulator/README.md still charges about ten percent too much for, and code
; that walks two streams is the one it charges twenty to thirty percent too
; much for.  Translated code is both at once.  So this is a question for the
; hardware, and these are the sequences to ask it with.
;
; Each template is what a translator would emit for a block of guest
; instructions, hand-written and honest about the whole cost: the guest
; register file lives in memory and is addressed with an `ext` prefix, and a
; guest load or store checks its address against both ends of guest RAM, just
; as rv32_hot.s does.  The pairs are the point:
;
;   alu_reg  / alu_mem     the same guest ALU work with the values in C33
;                          registers, and with every one of them fetched from
;                          the register file and stored back
;   ld_free  / ld_check    the same guest load with the bound test hoisted out
;                          of the loop, and paid for on every access
;   copy_reg / copy_mem    the memcpy word loop that is the second hottest
;                          block of a Linux boot, translated well and naively
;
; A template is position-independent -- relative branches, no absolute
; references, nothing but its two arguments -- so the driver can copy it into
; SDRAM or into A0 RAM and time the same bytes in both places.  That is the
; other half of the question: a code cache in internal RAM is about twice as
; cheap to fetch from, and there is only room for a few kilobytes of it.
;
;   void jp_<name>(uint32_t passes, struct jit_ctx *ctx);
;
; %r6 is the pass count and %r7 the context.  %r0..%r3 are callee-saved and
; every template saves them, because it uses them: %r0 is the guest register
; file, %r2 and %r3 the ends of guest RAM, %r8 the host-minus-guest offset an
; address is converted with.  They are the registers rv32_hot.s keeps live for
; exactly the same reasons.

	.set	CTX_REGS, 0
	.set	CTX_SRC, 4
	.set	CTX_DST, 8
	.set	CTX_END, 12
	.set	CTX_RAM, 16
	.set	CTX_RAMEND, 20
	.set	CTX_ADJ, 24
	.set	CTX_TABLE, 28

; Guest register numbers, as byte offsets into the register file.  The block
; being translated is the kernel's memcpy: a1 the source, t6 the destination,
; a3 the end, a4 the word in flight.
	.set	X_A1, 11 * 4
	.set	X_A3, 13 * 4
	.set	X_A4, 14 * 4
	.set	X_T6, 31 * 4

	.section .text,"ax"
	.align	1

; Load the context into the registers every template wants.
	.macro	PROLOGUE
	pushn	%r3
	ld.w	%r0,%r7
	ext	CTX_REGS
	ld.w	%r0,[%r7]
	ext	CTX_RAM
	ld.w	%r2,[%r7]
	ext	CTX_RAMEND
	ld.w	%r3,[%r7]
	ext	CTX_ADJ
	ld.w	%r8,[%r7]
	.endm

	.macro	EPILOGUE
	popn	%r3
	ret
	.endm

; ---------------------------------------------------------------- alu_reg --
;
; Twenty guest instructions of ChaCha20's quarter round -- four adds, four
; xors and four rotates, each rotate three guest instructions because rv32 has
; none -- with the four state words held in C33 registers across the whole
; block.  Twenty-four C33 instructions for twenty guest ones: the extra four
; are the copy a two-operand shift needs and the compiler would need too.
;
; This is the ceiling.  A Linux boot spends 11.9% of its instructions inside
; one 1902-instruction straight-line block of exactly this shape, so it is
; also a real number and not just a limit.

	.macro	QUARTER
	add	%r4,%r5
	xor	%r9,%r4
	ld.w	%r10,%r9
	sll	%r10,16
	srl	%r9,16
	or	%r9,%r10
	add	%r11,%r9
	xor	%r5,%r11
	ld.w	%r10,%r5
	sll	%r10,12
	srl	%r5,20
	or	%r5,%r10
	add	%r4,%r5
	xor	%r9,%r4
	ld.w	%r10,%r9
	sll	%r10,8
	srl	%r9,24
	or	%r9,%r10
	add	%r11,%r9
	xor	%r5,%r11
	ld.w	%r10,%r5
	sll	%r10,7
	srl	%r5,25
	or	%r5,%r10
	.endm

	.globl	jp_alu_reg
jp_alu_reg:
	PROLOGUE
	ld.w	%r4,%r2			; four state words, any values
	ld.w	%r5,%r3
	ld.w	%r9,%r8
	ld.w	%r11,%r6
1:	QUARTER
	QUARTER
	QUARTER
	sub	%r6,0x1
	jrne	1b
	EPILOGUE
	.globl	jp_alu_reg_end
jp_alu_reg_end:

; ---------------------------------------------------------------- alu_mem --
;
; The same work with no register allocation at all: every operand comes out of
; the guest register file and every result goes back.  A three-operand guest
; instruction is seven words this way and a shift-immediate five, against one
; word each above.  This is what the simplest possible translator emits, and
; the gap between the two is what writing a register allocator is worth.

	.macro	G_OP op, rd, rs1, rs2		; x[rd] = x[rs1] op x[rs2]
	ext	\rs1
	ld.w	%r4,[%r0]
	ext	\rs2
	ld.w	%r5,[%r0]
	\op	%r4,%r5
	ext	\rd
	ld.w	[%r0],%r4
	.endm

	.macro	G_SHIFT op, rd, rs1, imm	; x[rd] = x[rs1] op imm
	ext	\rs1
	ld.w	%r4,[%r0]
	\op	%r4,\imm
	ext	\rd
	ld.w	[%r0],%r4
	.endm

	.globl	jp_alu_mem
jp_alu_mem:
	PROLOGUE
1:	G_OP	add, X_A1, X_A1, X_A3		; a += b
	G_OP	xor, X_A4, X_A4, X_A1		; d ^= a
	G_SHIFT	sll, X_A3, X_A4, 16		; the rotate, three instructions
	G_SHIFT	srl, X_A4, X_A4, 16
	G_OP	or,  X_A4, X_A4, X_A3
	G_OP	add, X_T6, X_T6, X_A4		; c += d
	G_OP	xor, X_A3, X_A3, X_T6		; b ^= c
	G_SHIFT	sll, X_A4, X_A3, 12
	sub	%r6,0x1
	jrne	1b
	EPILOGUE
	.globl	jp_alu_mem_end
jp_alu_mem_end:

; ----------------------------------------------------------------- loads ---
;
; A guest load, translated twice.  Both fetch the base register from the
; register file, add the immediate, convert the guest address to a host one
; and store the result back.  ld_check also does what rv32_hot.s does on every
; access: test the host address against both ends of guest RAM and give up if
; it is a device.  Four instructions of eight, which is why it is worth
; knowing whether a translator can hoist them.

; The four loads of a pass read consecutive words and the base register is
; advanced at the end of it, so the stream walks as a real loop's does rather
; than standing on one open row -- which is the difference the emulator's
; model is known to get wrong, and the reason this is being asked of silicon.

	.macro	G_LOAD check, offset
	ext	X_A1
	ld.w	%r4,[%r0]		; x[a1]
	.if	\offset
	add	%r4,\offset		; the load's immediate
	.endif
	add	%r4,%r8			; host address
	.if	\check
	cmp	%r4,%r2
	jrult	9f
	cmp	%r4,%r3
	jruge	9f
	.endif
	ld.w	%r5,[%r4]
	ext	X_A4
	ld.w	[%r0],%r5
	.endm

	.macro	G_STEP				; addi a1,a1,16
	ext	X_A1
	ld.w	%r4,[%r0]
	add	%r4,0x10
	ext	X_A1
	ld.w	[%r0],%r4
	.endm

	.globl	jp_ld_free
jp_ld_free:
	PROLOGUE
1:	G_LOAD	0, 0
	G_LOAD	0, 4
	G_LOAD	0, 8
	G_LOAD	0, 12
	G_STEP
	sub	%r6,0x1
	jrne	1b
	EPILOGUE
9:	EPILOGUE
	.globl	jp_ld_free_end
jp_ld_free_end:

	.globl	jp_ld_check
jp_ld_check:
	PROLOGUE
1:	G_LOAD	1, 0
	G_LOAD	1, 4
	G_LOAD	1, 8
	G_LOAD	1, 12
	G_STEP
	sub	%r6,0x1
	jrne	1b
	EPILOGUE
9:	EPILOGUE
	.globl	jp_ld_check_end
jp_ld_check_end:

; ------------------------------------------------- what the translator emits --
;
; The first translator on silicon (2026-09-18) ran rvbench at 23.4 cycles a
; guest instruction against the model's 13.3, and the alu kernel -- no memory
; traffic at all -- at 20.6 against 10.5, while alu_reg above agrees with
; the model to the cycle.  So these are the emitted loops, byte for byte,
; and the same loops with one thing changed at a time: the prefixed
; branches made short, the batch check taken out.  Whichever variant the
; device prices differently from the model is the construct the model has
; wrong.
;
; alu_jit is rvbench's alu loop as the translator lays it out: the batch
; check at a 16-byte-aligned head, two copies of the ten-instruction body,
; the first leaving by a prefixed forward branch and the second by a
; prefixed backward one.  %r11 is the guest's counter, so a pass is one
; copy; %r6 is a batch that never runs out.

	.macro	ALU_BODY
	sub	%r6,0xa
	ld.w	%r1,%r12
	xor	%r1,%r9
	add	%r7,%r1
	ld.w	%r1,%r7
	add	%r1,%r10
	xor	%r9,%r1
	ld.w	%r14,%r9
	sll	%r14,0x1
	ld.w	%r1,%r7
	srl	%r1,0x3
	sub	%r11,0x1
	add	%r10,%r14
	add	%r12,%r1
	ld.w	%r5,0x0
	cmp	%r11,%r5
	.endm

	.macro	ALU_SETUP
	PROLOGUE
	ld.w	%r11,%r6		; the guest's counter
	xld.w	%r6,0x7fffffff		; a batch that never runs out
	ld.w	%r1,%r2			; the state words, any values
	ld.w	%r7,%r3
	ld.w	%r9,%r8
	ld.w	%r10,%r7
	ld.w	%r12,%r0
	ld.w	%r14,%r2
	.endm

	.balign	16
	.globl	jp_alu_jit
jp_alu_jit:
	ALU_SETUP
	.balign	16, 0
1:	cmp	%r6,0x0
	ext	0x0
	jrle	9f
	ALU_BODY
	ext	0x0
	jreq	9f
	ALU_BODY
	ext	-1
	jrne	1b
9:	EPILOGUE
	.globl	jp_alu_jit_end
jp_alu_jit_end:

; The same with the three branches short: does a prefixed branch cost more
; than its extra word?
	.balign	16
	.globl	jp_alu_short
jp_alu_short:
	ALU_SETUP
	.balign	16, 0
1:	cmp	%r6,0x0
	jrle	9f
	ALU_BODY
	jreq	9f
	ALU_BODY
	jrne	1b
9:	EPILOGUE
	.globl	jp_alu_short_end
jp_alu_short_end:

; And without the batch at all: the body twice and a short branch back,
; which is alu_reg's shape with this loop's instructions.
	.balign	16
	.globl	jp_alu_plain
jp_alu_plain:
	ALU_SETUP
	.balign	16, 0
1:	ALU_BODY
	jreq	9f
	ALU_BODY
	jrne	1b
9:	EPILOGUE
	.globl	jp_alu_plain_end
jp_alu_plain_end:

; rvbench's load loop as laid out to fit the fetch window: twenty bytes on a
; 16-byte line, the batch charged and checked by one subtraction and a
; prefixed branch, the back edge short.  The device ran the kernel at 17.5
; cycles a guest instruction against the model's 11.1, which is exactly a
; refetch of the two lines every pass -- as if it were not resident.  a1
; walks the stream; a3 is its end; the sum goes in %r9.

	.macro	LD_SETUP
	PROLOGUE
	xld.w	%r6,0x7fffffff
	ext	X_A1
	ld.w	%r1,[%r0]
	ext	X_A3
	ld.w	%r10,[%r0]
	ld.w	%r9,0x0
	.endm

	.macro	LD_DONE
	ext	X_A1
	ld.w	[%r0],%r1
	EPILOGUE
	.endm

	.balign	16
	.globl	jp_ld_res
jp_ld_res:
	LD_SETUP
	.balign	16, 0
1:	sub	%r6,0x4
	ext	0x0
	jrlt	9f
	ld.w	%r4,%r1
	add	%r4,%r8
	ld.w	%r11,[%r4]
	add	%r1,0x4
	add	%r9,%r11
	cmp	%r1,%r10
	jrne	1b
9:	LD_DONE
	.globl	jp_ld_res_end
jp_ld_res_end:

; The same without the batch: sixteen bytes, one short branch.
	.balign	16
	.globl	jp_ld_res_nb
jp_ld_res_nb:
	LD_SETUP
	.balign	16, 0
1:	ld.w	%r4,%r1
	add	%r4,%r8
	ld.w	%r11,[%r4]
	add	%r1,0x4
	add	%r9,%r11
	cmp	%r1,%r10
	jrne	1b
9:	LD_DONE
	.globl	jp_ld_res_nb_end
jp_ld_res_nb_end:

; And with the back edge prefixed, as an unrolled loop's is: twenty-two
; bytes, still inside the window by the rule.
	.balign	16
	.globl	jp_ld_res_far
jp_ld_res_far:
	LD_SETUP
	.balign	16, 0
1:	sub	%r6,0x4
	ext	0x0
	jrlt	9f
	ld.w	%r4,%r1
	add	%r4,%r8
	ld.w	%r11,[%r4]
	add	%r1,0x4
	add	%r9,%r11
	cmp	%r1,%r10
	ext	-1
	jrne	1b
9:	LD_DONE
	.globl	jp_ld_res_far_end
jp_ld_res_far_end:

; The store kernel's loop as laid out to be resident -- sw a5,0(a4); addi
; a5,1; addi a4,4; bne a5,a2 -- which the device ran at 15.3 cycles a guest
; instruction against the model's 8.0, twice the resident load loop above
; whose only difference is the store.  a1 walks the stream, a3 is its end.
	.balign	16
	.globl	jp_st_res
jp_st_res:
	LD_SETUP
	.balign	16, 0
1:	sub	%r6,0x4
	ext	0x0
	jrlt	9f
	ld.w	%r4,%r1
	add	%r4,%r8
	ld.w	[%r4],%r9
	add	%r9,0x1
	add	%r1,0x4
	cmp	%r1,%r10
	jrne	1b
9:	LD_DONE
	.globl	jp_st_res_end
jp_st_res_end:

	.balign	16
	.globl	jp_st_res_nb
jp_st_res_nb:
	LD_SETUP
	.balign	16, 0
1:	ld.w	%r4,%r1
	add	%r4,%r8
	ld.w	[%r4],%r9
	add	%r9,0x1
	add	%r1,0x4
	cmp	%r1,%r10
	jrne	1b
9:	LD_DONE
	.globl	jp_st_res_nb_end
jp_st_res_nb_end:

; The sieve's memset -- sb a3,0(a5); addi a5,1; bne a5,a4 -- a byte a pass,
; the pass count setting the end.
	.balign	16
	.globl	jp_sb_res
jp_sb_res:
	PROLOGUE
	ext	X_A1
	ld.w	%r1,[%r0]
	ld.w	%r10,%r1
	add	%r10,%r6		; the end: one byte a pass
	xld.w	%r6,0x7fffffff
	ld.w	%r9,0x1
	.balign	16, 0
1:	sub	%r6,0x3
	ext	0x0
	jrlt	9f
	ld.w	%r4,%r1
	add	%r4,%r8
	ld.b	[%r4],%r9
	add	%r1,0x1
	cmp	%r1,%r10
	jrne	1b
9:	EPILOGUE
	.globl	jp_sb_res_end
jp_sb_res_end:

; The sieve's inner loop -- sb zero,0(a4); add a5,a5,a3; add a4,a4,a3; bge
; a2,a5 -- striding by three, the device's worst kernel at 3.2x the model.
	.balign	16
	.globl	jp_sieve_res
jp_sieve_res:
	PROLOGUE
	ext	X_A1
	ld.w	%r9,[%r0]		; a4, the pointer
	ld.w	%r11,0x0		; a5, the index
	ld.w	%r12,0x3		; a3, the stride
	ld.w	%r10,%r6		; a2: three a pass, less one
	add	%r10,%r6
	add	%r10,%r6
	sub	%r10,0x1
	xld.w	%r6,0x7fffffff
	.balign	16, 0
1:	sub	%r6,0x4
	ext	0x0
	jrlt	9f
	ld.w	%r5,0x0
	ld.w	%r4,%r9
	add	%r4,%r8
	ld.b	[%r4],%r5
	add	%r11,%r12
	add	%r9,%r12
	cmp	%r10,%r11
	jrge	1b
9:	EPILOGUE
	.globl	jp_sieve_res_end
jp_sieve_res_end:

; -------------------------------------------------------------- the copy ---
;
; The kernel's memcpy word loop, which is 4.2% of a Linux boot on its own:
;
;	lw   a4,0(a1) ; addi a1,a1,4 ; sw a4,0(t6) ; addi t6,t6,4
;	bltu a1,a3,-16
;
; Five guest instructions.  Translated with the three pointers in C33
; registers it is six C33 instructions and twelve bytes -- which is inside the
; 27-byte window a loop has to fit in to be fetched once instead of every
; pass, so in SDRAM this should be the one template that does not pay for its
; own fetch at all.  The bound test is the loop's own: a1 has been proven
; below a3, and a3 was checked on entry.

	.globl	jp_copy_reg
jp_copy_reg:
	PROLOGUE
	ext	CTX_SRC
	ld.w	%r1,[%r7]
	ext	CTX_DST
	ld.w	%r9,[%r7]
	ext	CTX_END
	ld.w	%r10,[%r7]
1:	ld.w	%r4,[%r1]
	add	%r1,0x4
	ld.w	[%r9],%r4
	add	%r9,0x4
	cmp	%r1,%r10
	jrult	1b
	EPILOGUE
	.globl	jp_copy_reg_end
jp_copy_reg_end:

; The same five guest instructions with nothing held in a register and every
; access checked: thirty-three words where the other is six.  Both are
; translations of the same block, and the device says what the difference
; between a good translator and a crude one is worth on the code that matters
; most.

	.globl	jp_copy_mem
jp_copy_mem:
	PROLOGUE
1:	ext	X_A1			; lw a4,0(a1)
	ld.w	%r4,[%r0]
	add	%r4,%r8
	cmp	%r4,%r2
	jrult	9f
	cmp	%r4,%r3
	jruge	9f
	ld.w	%r5,[%r4]
	ext	X_A4
	ld.w	[%r0],%r5
	ext	X_A1			; addi a1,a1,4
	ld.w	%r4,[%r0]
	add	%r4,0x4
	ext	X_A1
	ld.w	[%r0],%r4
	ext	X_T6			; sw a4,0(t6)
	ld.w	%r4,[%r0]
	add	%r4,%r8
	cmp	%r4,%r2
	jrult	9f
	cmp	%r4,%r3
	jruge	9f
	ext	X_A4
	ld.w	%r5,[%r0]
	ld.w	[%r4],%r5
	ext	X_T6			; addi t6,t6,4
	ld.w	%r4,[%r0]
	add	%r4,0x4
	ext	X_T6
	ld.w	[%r0],%r4
	ext	X_A1			; bltu a1,a3
	ld.w	%r4,[%r0]
	ext	X_A3
	ld.w	%r5,[%r0]
	cmp	%r4,%r5
	jrult	1b
	EPILOGUE
9:	EPILOGUE
	.globl	jp_copy_mem_end
jp_copy_mem_end:

; ------------------------------------------------------------ block exits --
;
; A translator does not emit one long run, it emits basic blocks -- 8.16 guest
; instructions of them on a Linux boot -- and every one of them ends by
; getting to the next.  There are two ways to do that and the budget needs
; both: a direct jump patched into the block once its successor has been
; translated, and a lookup, which is what an indirect guest jump has to do
; because its target is not known until it runs.  One guest instruction in 37
; is a `jalr`, so the lookup is on the hot path whatever the linker does.
;
; Eight blocks of eight instructions each, on a 48-byte stride and entered in
; the order 0 3 6 1 4 7 2 5, so no jump lands on the instruction after it and
; the fetch stream is broken exactly as a code cache breaks it.  `exit_none`
; is the same eight bodies run straight through: the difference between it and
; the other two, over eight exits, is what an exit costs.
;
; These are 420 bytes and A0 RAM has 316 left, so they are measured in SDRAM
; only -- which is where a code cache would live anyway, on the evidence of
; copy_reg above.

	.set	SLOT, 56		; jit_probe.h's JP_SLOT, and checked there
	.set	TAG0, 0x1000		; block i answers to TAG0 + i*4, so that
					; (tag >> 2) & 7 is i and the hash is exact

	.macro	BODY			; eight guest instructions, in registers
	add	%r4,%r5
	xor	%r10,%r4
	add	%r11,%r10
	xor	%r5,%r11
	add	%r4,%r10
	xor	%r11,%r5
	add	%r10,%r11
	xor	%r4,%r5
	.endm

	.globl	jp_exit_none
jp_exit_none:
	PROLOGUE
1:	.rept	8
	BODY
	.endr
	sub	%r6,0x1
	jrne	1b
	ld.w	%r4,%r6			; zero, if every pass ran
	EPILOGUE
	.globl	jp_exit_none_end
jp_exit_none_end:

; The direct chain.  `xjp` carries its displacement in two `ext` prefixes and
; is relative, so the template survives being copied anywhere -- which is also
; what makes it the honest form to measure: a code cache is too big for the
; short jump to reach across.

	.macro	LINKED from, to
.Lk\from:
	BODY
	xjp	.Lk\to
	.space	SLOT - (. - .Lk\from)
	.endm

	.globl	jp_exit_link
jp_exit_link:
	PROLOGUE
	xjp	.Lk0
	.align	2
	LINKED	0, 3
	LINKED	1, 4
	LINKED	2, 5
	LINKED	3, 6
	LINKED	4, 7
.Lk5:					; the chain closes here, with the count
	BODY
	sub	%r6,0x1
	jreq	9f
	xjp	.Lk0
9:	ld.w	%r4,%r6			; zero, if every pass ran
	EPILOGUE
	.space	SLOT - (. - .Lk5)
	LINKED	6, 1
	LINKED	7, 2
	.globl	jp_exit_link_end
jp_exit_link_end:

; The lookup.  %r14 holds the table the driver fills in after copying, which
; is the one thing here a translator would also have to do: the targets are
; addresses in the buffer and are not known until the code is somewhere.
; Eight entries of tag and target; the tag is compared because a real table
; collides and this one must pay for the compare even though it does not.

	.macro	LOOKUP from, to
.Lh\from:
	BODY
	xld.w	%r13,TAG0 + \to * 4	; the guest pc being jumped to
	ld.w	%r9,%r13
	srl	%r9,2
	and	%r9,0x7
	sll	%r9,3
	add	%r9,%r14
	ld.w	%r12,[%r9]		; the tag that entry answers to
	cmp	%r12,%r13
	jrne	7f
	ext	4
	ld.w	%r9,[%r9]		; ...and where its code is
	jp	%r9
	; Only the tag compare reaches here, and only if the driver filled the
	; table wrongly.  It costs bytes and never a cycle, and without it a
	; miss would slide through the padding into the next block's body and
	; come back as a plausible number.
7:	xjp	.Lh_out
	.space	SLOT - (. - .Lh\from)
	.endm

	.globl	jp_exit_hash
jp_exit_hash:
	PROLOGUE
	ext	CTX_TABLE
	ld.w	%r14,[%r7]
	xjp	.Lh0
	.align	2
	.globl	jp_exit_hash_blocks
jp_exit_hash_blocks:
	LOOKUP	0, 3
	LOOKUP	1, 4
	LOOKUP	2, 5
	LOOKUP	3, 6
	LOOKUP	4, 7
.Lh5:					; the chain closes here, with the count
	BODY
	sub	%r6,0x1
	jreq	.Lh_out
	xjp	.Lh0
.Lh_out:				; zero only if no lookup ever missed
	ld.w	%r4,%r6
	EPILOGUE
	.space	SLOT - (. - .Lh5)
	LOOKUP	6, 1
	LOOKUP	7, 2
	.globl	jp_exit_hash_end
jp_exit_hash_end:
