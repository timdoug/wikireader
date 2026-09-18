; rv32_hot.s - the rv32ima interpreter's hot path, hand-written for the C33.
;
;   uint32_t rv32_hot(rv32_t *s, uint32_t budget);
;
; Runs guest instructions from s->pc and returns how many it retired.  It
; stops early, having retired fewer than `budget`, whenever it meets
; something it does not implement -- a system instruction, an atomic, a
; divide, a device address, a misaligned access, a fetch outside guest RAM.
; s->pc then points at that instruction and the C interpreter in rv32.c
; executes exactly that one before calling back in.  Everything rare is
; therefore still written once, in C, and only the shapes that dominate a
; real workload are here.
;
; Two things make this faster than what the compiler produces, and neither
; is instruction count.  A taken branch costs about six cycles, so what
; matters is how many of them a guest instruction pays.  The dispatch table
; is indexed by the opcode *and* funct3 together -- 1024 entries, which is
; why it lives in the window buffer rather than A0 RAM -- so there is no
; second level of dispatch for the ALU operations, the load and store
; widths or the branch conditions.  And the fetch is threaded: every body
; ends by fetching and dispatching the next instruction itself rather than
; jumping back to a shared loop head.  That leaves one taken branch per
; guest instruction, the dispatch itself.  A0 RAM is zero-wait, so the code
; the threading duplicates costs nothing to fetch.
;
; Registers, for the whole loop:
;   %r0  s, which is also the guest register file: x[n] is at [%r0 + n*4]
;   %r1  p, a host pointer to the guest instruction being executed
;   %r2  ram, the host base of guest RAM
;   %r3  ram_end, the host end of guest RAM
;   %r6  instructions left in the batch
;   %r7  the dispatch table
;   %r8  ram - RV_RAM_BASE, so a guest address converts with one add
;   %r5  ir, the instruction word
;   %r12 x[rs1], %r10 rd*4, %r11 the value to write back
;   %r4, %r9, %r13, %r14  scratch
;
; The fetch bound is only checked against the top of RAM.  p rises by four
; on its own, so only a control transfer can put it below the bottom, and
; each one checks for itself.

	.set	OFF_PC, 128
	.set	OFF_RAM, 132
	.set	OFF_RAM_SIZE, 136
	.set	OFF_RESERVE, 140

	.section .fastcode,"ax"
	.align	1
	.globl	rv32_hot

; Fetch the next guest instruction and jump to its body.  Ends every body,
; so the only taken branch on the path is the indirect jump at the end.
; The two guards are short forward branches to a trampoline placed after
; that jump, which fall-through cannot reach.
	.macro	DISPATCH
	sub	%r6,0x1
	jreq	8f			; batch spent; p is already the next pc
	cmp	%r1,%r3
	jruge	8f			; ran off the top of guest RAM
	ld.w	%r5,[%r1]
	ld.w	%r9,%r5			; x[rs1], wanted by all but lui and jal
	sll	%r9,12
	srl	%r9,27
	sll	%r9,2
	add	%r9,%r0
	ld.w	%r12,[%r9]
	ld.w	%r10,%r5		; rd * 4
	sll	%r10,20
	srl	%r10,27
	sll	%r10,2
	ld.w	%r9,%r5			; (opcode << 3 | funct3) * 4
	sll	%r9,25
	srl	%r9,22
	ld.w	%r13,%r5
	srl	%r13,12
	and	%r13,0x7
	or	%r9,%r13
	sll	%r9,1
	add	%r9,%r7
	ld.uh	%r9,[%r9]		; a halfword: every body is in A0 RAM
	jp.d	%r9
	add	%r1,0x4			; p now points at the next instruction
8:	xjp	.Lleave
	.endm

; Write %r11 to x[rd] unless rd is zero.
	.macro	WRITEBACK
	cmp	%r10,0x0
	jreq	9f
	add	%r10,%r0
	ld.w	[%r10],%r11
9:
	.endm

; ---------------------------------------------------------------- entry --

rv32_hot:
	pushn	%r3			; %r0..%r3 are callee-saved
	sub	%sp,0x2
	ld.w	[%sp+0x0],%r7		; the batch size, for the return value
	ld.w	%r0,%r6
	ld.w	%r6,%r7
	ext	OFF_RAM
	ld.w	%r2,[%r0]		; ram
	ext	OFF_RAM_SIZE
	ld.w	%r3,[%r0]
	add	%r3,%r2			; ram_end
	ld.w	%r8,0x1			; RV_RAM_BASE, without an ext pair:
	sll	%r8,31			; subtracting it and adding it are the
	add	%r8,%r2			; same modulo 2^32, so %r8 = ram_adj
	ext	OFF_PC
	ld.w	%r1,[%r0]
	add	%r1,%r8			; p = ram_adj + s->pc
	cmp	%r1,%r2
	jrult	.Lleave			; the entry pc is outside guest RAM
	xld.w	%r7,.Ltable
	add	%r6,0x1			; DISPATCH decrements before each fetch,
	DISPATCH			; so bias the count only once past here

; Store p back as a guest address and return the number retired.
.Lleave:
	ld.w	%r4,%r1
	sub	%r4,%r8			; guest pc
	ext	OFF_PC
	ld.w	[%r0],%r4
	ld.w	%r4,[%sp+0x0]
	sub	%r4,%r6			; retired = batch - left
	add	%sp,0x2
	popn	%r3
	ret

; The instruction at p is not implemented here.  It has not run and has not
; been counted; %r6 has not been decremented for it either, because that
; happens in the next DISPATCH.
.Ldecline_far:
	sub	%r1,0x4			; undo the delay-slot advance
	xjp	.Lleave

; ------------------------------------------------------------- opcodes ---

.Llui:					; 0x37, every funct3
	ld.w	%r11,%r5
	srl	%r11,12
	sll	%r11,12
	WRITEBACK
	DISPATCH

.Lauipc:				; 0x17, every funct3
	ld.w	%r11,%r5
	srl	%r11,12
	sll	%r11,12
	ld.w	%r9,%r1
	sub	%r9,%r8			; guest pc of the next instruction
	sub	%r9,0x4			; ...so of this one
	add	%r11,%r9
	WRITEBACK
	DISPATCH

.Ljal:					; 0x6f, every funct3
	ld.w	%r11,%r1
	sub	%r11,%r8		; link = pc + 4, which p already is
	ld.w	%r14,%r1
	sub	%r14,0x4		; this instruction, if the target is bad
	ld.w	%r9,%r5			; imm[20|10:1|11|19:12]
	sra	%r9,31
	sll	%r9,20			; sign, from bit 20 up
	ld.w	%r13,%r5
	sll	%r13,1
	srl	%r13,22
	sll	%r13,1			; imm[10:1]
	or	%r9,%r13
	ld.w	%r13,%r5
	sll	%r13,11
	srl	%r13,31
	sll	%r13,11			; imm[11]
	or	%r9,%r13
	ld.w	%r13,%r5
	sll	%r13,12
	srl	%r13,24
	sll	%r13,12			; imm[19:12]
	or	%r9,%r13
	ld.w	%r1,%r14
	add	%r1,%r9
	xjp	.Lcheck_target

.Ljalr:					; 0x67, funct3 0
	ld.w	%r11,%r1
	sub	%r11,%r8		; link
	ld.w	%r14,%r1
	sub	%r14,0x4		; this instruction, if the target is bad
	ld.w	%r9,%r5
	sra	%r9,20			; imm
	add	%r9,%r12		; x[rs1] + imm
	srl	%r9,1
	sll	%r9,1			; clear bit 0
	ld.w	%r1,%r9
	add	%r1,%r8
	; fall through

; A computed target must land inside guest RAM and on a word boundary.
; Anything else goes back to C, so that traps are built in one place.
.Lcheck_target:
	cmp	%r1,%r2
	jrult	.Lbad_target
	cmp	%r1,%r3
	jruge	.Lbad_target
	ld.w	%r9,%r1
	and	%r9,0x3
	jrne	.Lbad_target
	WRITEBACK
	DISPATCH
.Lbad_target:
	ld.w	%r1,%r14		; the instruction that jumped
	xjp	.Lleave

; ---- branches.  The immediate is built only when the branch is taken.

	.macro	BRANCH cond
	ld.w	%r9,%r5			; x[rs2]
	sll	%r9,7
	srl	%r9,27
	sll	%r9,2
	add	%r9,%r0
	ld.w	%r13,[%r9]
	cmp	%r12,%r13
	\cond	7f
	DISPATCH			; not taken
7:	xjp	.Ltaken
	.endm

.Lbeq:	BRANCH jreq
.Lbne:	BRANCH jrne
.Lblt:	BRANCH jrlt
.Lbge:	BRANCH jrge
.Lbltu:	BRANCH jrult
.Lbgeu:	BRANCH jruge

.Ltaken:				; imm[12|10:5|4:1|11]
	ld.w	%r14,%r1
	sub	%r14,0x4		; this instruction, if the target is bad
	ld.w	%r9,%r5
	sra	%r9,31
	sll	%r9,12			; sign, from bit 12 up
	ld.w	%r13,%r5
	srl	%r13,7
	and	%r13,0x1e		; imm[4:1]
	or	%r9,%r13
	ld.w	%r13,%r5
	sll	%r13,1
	srl	%r13,26
	sll	%r13,5			; imm[10:5]
	or	%r9,%r13
	ld.w	%r13,%r5
	sll	%r13,24
	srl	%r13,31
	sll	%r13,11			; imm[11]
	or	%r9,%r13
	ld.w	%r1,%r14
	add	%r1,%r9
	ld.w	%r10,0x0		; a branch writes no register
	xjp	.Lcheck_target

; ---- loads.  %r9 becomes the host pointer; anything outside RAM is a
; device access and belongs to C.

	.macro	LOADADDR
	ld.w	%r9,%r5
	sra	%r9,20
	add	%r9,%r12
	add	%r9,%r8
	cmp	%r9,%r2
	jrult	6f
	cmp	%r9,%r3
	jrult	5f
6:	xjp	.Lmmio_load
5:
	.endm

.Llb:	LOADADDR
	ld.b	%r11,[%r9]
	WRITEBACK
	DISPATCH

.Llbu:	LOADADDR
	ld.ub	%r11,[%r9]
	WRITEBACK
	DISPATCH

.Llh:	LOADADDR
	ld.w	%r13,%r9
	and	%r13,0x1
	jrne	4f
	ld.h	%r11,[%r9]
	WRITEBACK
	DISPATCH
4:	xjp	.Ldecline_far

.Llhu:	LOADADDR
	ld.w	%r13,%r9
	and	%r13,0x1
	jrne	4f
	ld.uh	%r11,[%r9]
	WRITEBACK
	DISPATCH
4:	xjp	.Ldecline_far

.Llw:	LOADADDR
	ld.w	%r13,%r9
	and	%r13,0x3
	jrne	4f
	ld.w	%r11,[%r9]
	WRITEBACK
	DISPATCH
4:	xjp	.Ldecline_far

; ---- stores.  Same, plus the value and the reservation.

	.macro	STOREADDR
	ld.w	%r9,%r5			; imm[11:5]
	sra	%r9,25
	sll	%r9,5
	ld.w	%r13,%r5		; imm[4:0]
	sll	%r13,20
	srl	%r13,27
	or	%r9,%r13
	add	%r9,%r12
	add	%r9,%r8
	cmp	%r9,%r2
	jrult	6f
	cmp	%r9,%r3
	jrult	5f
6:	xjp	.Lmmio_store
5:	ld.w	%r13,%r5		; x[rs2], the value
	sll	%r13,7
	srl	%r13,27
	sll	%r13,2
	add	%r13,%r0
	ld.w	%r11,[%r13]
	.endm

.Lsb:	STOREADDR
	ld.b	[%r9],%r11
	xjp	.Lreserve_check

.Lsh:	STOREADDR
	ld.w	%r13,%r9
	and	%r13,0x1
	jrne	4f
	ld.h	[%r9],%r11
	xjp	.Lreserve_check
4:	xjp	.Ldecline_far

.Lsw:	STOREADDR
	ld.w	%r13,%r9
	and	%r13,0x3
	jrne	4f
	ld.w	[%r9],%r11
	xjp	.Lreserve_check
4:	xjp	.Ldecline_far

; A store into the reserved word breaks the reservation.  Comparing in host
; pointer space is the same comparison.
.Lreserve_check:
	ext	OFF_RESERVE
	ld.w	%r13,[%r0]
	add	%r13,%r8
	ld.w	%r14,%r9
	srl	%r14,2
	sll	%r14,2
	cmp	%r13,%r14
	jrne	3f
	ld.w	%r13,0x0
	not	%r13,%r13		; ~0, no reservation held
	ext	OFF_RESERVE
	ld.w	[%r0],%r13
3:	ld.w	%r10,0x0		; a store writes no register
	DISPATCH

; fence.i goes to C rather than through here, because the translator has to
; hear about it: it is the one point at which guest code is allowed to have
; changed underneath a translation.

; ---- arithmetic.  The immediate forms and the register forms have
; separate entries, so neither tests which it is.

	.macro	IMM
	ld.w	%r13,%r5
	sra	%r13,20
	.endm

	.macro	REG
	ld.w	%r13,%r5
	sll	%r13,7
	srl	%r13,27
	sll	%r13,2
	add	%r13,%r0
	ld.w	%r13,[%r13]
	.endm

	.macro	DONE op
	ld.w	%r11,%r12
	\op	%r11,%r13
	WRITEBACK
	DISPATCH
	.endm

.Laddi:	IMM
	DONE add
.Lori:	IMM
	DONE or
.Lxori:	IMM
	DONE xor
.Landi:	IMM
	DONE and

.Lslli:	IMM
	and	%r13,0x1f
	DONE sll

.Lsrxi:	IMM
	and	%r13,0x1f
	ld.w	%r11,%r12
	ld.w	%r4,%r5
	sll	%r4,1
	srl	%r4,31
	jrne	2f
	srl	%r11,%r13
	WRITEBACK
	DISPATCH
2:	sra	%r11,%r13
	WRITEBACK
	DISPATCH

.Lslti:	IMM
	ld.w	%r11,0x0
	cmp	%r12,%r13
	jrge	1f
	ld.w	%r11,0x1
1:	WRITEBACK
	DISPATCH

.Lsltiu: IMM
	ld.w	%r11,0x0
	cmp	%r12,%r13
	jruge	1f
	ld.w	%r11,0x1
1:	WRITEBACK
	DISPATCH

; The register forms: bit 25 selects the M extension and bit 30 the
; subtract or the arithmetic shift.
	.macro	NOMUL
	ld.w	%r4,%r5
	sll	%r4,6
	srl	%r4,31
	jrne	6f
	.endm

.Laddsub: NOMUL
	REG
	ld.w	%r11,%r12
	ld.w	%r4,%r5
	sll	%r4,1
	srl	%r4,31
	jrne	2f
	add	%r11,%r13
	WRITEBACK
	DISPATCH
2:	sub	%r11,%r13
	WRITEBACK
	DISPATCH
6:	REG				; mul, the one M operation worth having
	mltu.w	%r12,%r13
	ld.w	%r11,%alr
	WRITEBACK
	DISPATCH

.Lor:	NOMUL
	REG
	DONE or
6:	REG
	xjp	.Lmdiv
.Lxor:	NOMUL
	REG
	DONE xor
6:	REG
	xjp	.Lmdiv
.Land:	NOMUL
	REG
	DONE and
6:	REG
	xjp	.Lmdiv
.Lsll:	NOMUL
	REG
	and	%r13,0x1f
	DONE sll
6:	REG				; mulh: the multiply already produced it
	mlt.w	%r12,%r13
	ld.w	%r11,%ahr
	WRITEBACK
	DISPATCH

.Lsrx:	NOMUL
	REG
	and	%r13,0x1f
	ld.w	%r11,%r12
	ld.w	%r4,%r5
	sll	%r4,1
	srl	%r4,31
	jrne	2f
	srl	%r11,%r13
	WRITEBACK
	DISPATCH
2:	sra	%r11,%r13
	WRITEBACK
	DISPATCH
6:	REG
	xjp	.Lmdiv

.Lslt:	NOMUL
	REG
	ld.w	%r11,0x0
	cmp	%r12,%r13
	jrge	1f
	ld.w	%r11,0x1
1:	WRITEBACK
	DISPATCH
6:	REG
	xjp	.Lmdiv

.Lsltu:	NOMUL
	REG
	ld.w	%r11,0x0
	cmp	%r12,%r13
	jruge	1f
	ld.w	%r11,0x1
1:	WRITEBACK
	DISPATCH
6:	REG				; mulhu
	mltu.w	%r12,%r13
	ld.w	%r11,%ahr
	WRITEBACK
	DISPATCH

; The divides and mulhsu, handed to C with a plain call: %r0..%r3 survive
; it, so p, s and the RAM bounds do not need saving -- only the loop's
; caller-saved registers do.
.Lmdiv:
	sub	%sp,0x5
	ld.w	[%sp+0x0],%r5
	ld.w	[%sp+0x1],%r6
	ld.w	[%sp+0x2],%r7
	ld.w	[%sp+0x3],%r8
	ld.w	[%sp+0x4],%r10
	ld.w	%r6,%r5
	srl	%r6,12
	and	%r6,0x7			; funct3
	ld.w	%r7,%r12
	ld.w	%r8,%r13
	xcall	rv32_divop
	ld.w	%r11,%r4
	ld.w	%r5,[%sp+0x0]
	ld.w	%r6,[%sp+0x1]
	ld.w	%r7,[%sp+0x2]
	ld.w	%r8,[%sp+0x3]
	ld.w	%r10,[%sp+0x4]
	add	%sp,0x5
	WRITEBACK
	DISPATCH

; ---- devices.  An access outside guest RAM is a device register, and a Linux
; boot makes one every 121 instructions -- two thirds of everything this path
; declines, against four times in a whole run of the benchmark.  Handing one
; back costs about four hundred cycles in the C interpreter, so they are done
; from here instead, the way the divides are, with the helpers in the window
; buffer.  What is still declined is what C has to see for itself: a
; misaligned access, because that raises a trap, and a store to the marker or
; to SYSCON, because those read the retired count and stop the machine.
;
; The width's mask comes out of funct3 rather than out of a separate stub per
; body: this is 0.8% of the instruction stream and six instructions here are
; cheaper than six copies of the stub in A0 RAM.

	.macro	DEVICE_ADDR			; %r4 = the guest address, aligned
	ld.w	%r4,%r5
	srl	%r4,12
	and	%r4,0x3			; log2 of the width: b 0, h 1, w 2
	ld.w	%r13,0x1
	sll	%r13,%r4
	sub	%r13,0x1
	ld.w	%r4,%r9
	sub	%r4,%r8			; the guest address the helper wants
	and	%r13,%r4
	jreq	1f
	xjp	.Ldecline_far		; misaligned: C raises the trap
1:
	.endm

	.macro	SAVE_CALLER
	sub	%sp,0x4
	ld.w	[%sp+0x0],%r6
	ld.w	[%sp+0x1],%r7
	ld.w	[%sp+0x2],%r8
	ld.w	[%sp+0x3],%r10
	.endm

	.macro	RESTORE_CALLER
	ld.w	%r6,[%sp+0x0]
	ld.w	%r7,[%sp+0x1]
	ld.w	%r8,[%sp+0x2]
	ld.w	%r10,[%sp+0x3]
	add	%sp,0x4
	.endm

.Lmmio_load:				; %r9 host address, %r5 ir, %r10 rd*4
	DEVICE_ADDR
	SAVE_CALLER
	ld.w	%r6,%r0
	ld.w	%r7,%r4
	xcall	rv32_mmio_load
	ld.w	%r11,%r4		; whatever the device read as
	RESTORE_CALLER
	WRITEBACK
	DISPATCH

.Lmmio_store:				; %r9 host address, %r5 ir
	DEVICE_ADDR
	ld.w	%r13,%r5		; x[rs2], the value
	sll	%r13,7
	srl	%r13,27
	sll	%r13,2
	add	%r13,%r0
	ld.w	%r11,[%r13]
	SAVE_CALLER
	ld.w	%r6,%r0
	ld.w	%r7,%r4
	ld.w	%r8,%r11
	xcall	rv32_mmio_store_hot
	ld.w	%r13,%r4		; nonzero: this one is C's to do
	RESTORE_CALLER
	cmp	%r13,0x0
	jreq	2f
	xjp	.Ldecline_far
2:	ld.w	%r10,0x0		; a store writes no register
	DISPATCH

.Lfence:				; nothing is cached or reordered
	ld.w	%r10,0x0
	DISPATCH

; ------------------------------------------------------- dispatch table --
;
; 1024 entries: the 7-bit opcode and funct3 together.  Opcodes whose funct3
; field is part of an immediate (lui, auipc, jal) name the same body eight
; times; encodings whose low two bits are not both set are compressed
; instructions, which this core does not implement, and land on the decline
; entry like any other hole.  An entry is a halfword: every body is in A0
; RAM, below 0x2000, and the 2 KB the other half took is what the divide
; and the translator's runtime live in.

	.section .ivram_code,"ax"
	.align	1

	.macro	HOLE n
	.rept	\n * 8
	.short	.Ldecline_far
	.endr
	.endm

.Ltable:
	HOLE	3			; 0x00 .. 0x02
	.short	.Llb, .Llh, .Llw, .Ldecline_far			; 0x03 load
	.short	.Llbu, .Llhu, .Ldecline_far, .Ldecline_far
	HOLE	11			; 0x04 .. 0x0e
	.short	.Lfence, .Ldecline_far, .Ldecline_far, .Ldecline_far	; 0x0f: fence.i is C's
	.short	.Ldecline_far, .Ldecline_far, .Ldecline_far, .Ldecline_far
	HOLE	3			; 0x10 .. 0x12
	.short	.Laddi, .Lslli, .Lslti, .Lsltiu			; 0x13 op-imm
	.short	.Lxori, .Lsrxi, .Lori, .Landi
	HOLE	3			; 0x14 .. 0x16
	.rept	8						; 0x17 auipc
	.short	.Lauipc
	.endr
	HOLE	11			; 0x18 .. 0x22
	.short	.Lsb, .Lsh, .Lsw, .Ldecline_far			; 0x23 store
	.short	.Ldecline_far, .Ldecline_far, .Ldecline_far, .Ldecline_far
	HOLE	15			; 0x24 .. 0x32
	.short	.Laddsub, .Lsll, .Lslt, .Lsltu			; 0x33 op
	.short	.Lxor, .Lsrx, .Lor, .Land
	HOLE	3			; 0x34 .. 0x36
	.rept	8						; 0x37 lui
	.short	.Llui
	.endr
	HOLE	43			; 0x38 .. 0x62
	.short	.Lbeq, .Lbne, .Ldecline_far, .Ldecline_far	; 0x63 branch
	.short	.Lblt, .Lbge, .Lbltu, .Lbgeu
	HOLE	3			; 0x64 .. 0x66
	.short	.Ljalr, .Ldecline_far, .Ldecline_far, .Ldecline_far	; 0x67
	.short	.Ldecline_far, .Ldecline_far, .Ldecline_far, .Ldecline_far
	HOLE	7			; 0x68 .. 0x6e
	.rept	8						; 0x6f jal
	.short	.Ljal
	.endr
	HOLE	16			; 0x70 .. 0x7f
