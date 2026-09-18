; rv32_jitrt.s - the runtime a translated block is entered from and leaves by.
;
;   uint32_t rv32_jit_enter(rv32_t *s, uint32_t budget, void *code);
;
; Sets up the registers every translated block assumes, jumps into one, and
; returns how many guest instructions were retired before the code cache
; handed control back.  rv32_jit.h lists the register assignment; the short
; version is that it is rv32_hot.s's, for the same reasons, plus the base and
; the size of guest RAM for the access check, and seven registers that hold
; guest registers for the length of a region.
;
; This is small on purpose.  The interpreter needed all of A0 RAM because its
; dispatch loop cannot fit the C33's 27-byte fetch window at any alignment; a
; translated block can, and jit_probe.s measured a well-translated loop running
; *faster* from SDRAM than from internal RAM.  So the code cache is in SDRAM
; and what lives in the fast memory is only this: the entry, the lookup an
; indirect guest jump takes, the one way a block gives up, and the two device
; helpers that come back.

	.set	OFF_PC, 128
	.set	OFF_RAM, 132
	.set	OFF_RAM_SIZE, 136

	.set	JOFF_MAP, 0			; rv32_jit.h's RV32_JOFF_*
	.set	JOFF_BACK, 4
	.set	JOFF_SPILL, 8
	.set	JOFF_CSRVAL, 36
	.set	JIT_MASK, 32767			; rv32_jit.h's RV32_JIT_MASK

; This lives in the LCD window buffer, beside the interpreter's dispatch table:
; A0 RAM is rv32_hot.s's, and it is still what interprets everything the
; translator has not earned its way into.  Internal RAM matters here more than
; anywhere, because every stub is entered by a jump and left by another, and
; landing a jump in SDRAM code costs twenty to thirty cycles where landing one
; here costs six.  Measured: the divides got slower, not faster, when their
; saves were halved but their wrapper was in SDRAM.
	.section .ivram_code,"ax"
	.align	1

; ---------------------------------------------------------------- entry --

	.globl	rv32_jit_enter
rv32_jit_enter:
	pushn	%r3				; %r0..%r3 are callee-saved
	sub	%sp,0x1
	ld.w	[%sp+0x0],%r7			; the batch size, for the return
	ld.w	%r13,%r8			; where to start
	ld.w	%r0,%r6
	ld.w	%r6,%r7
	ext	OFF_RAM
	ld.w	%r2,[%r0]			; ram
	ext	OFF_RAM_SIZE
	ld.w	%r3,[%r0]
	srl	%r3,2				; ram_size / 4: what a word access's
						; rotated offset is compared against
	ld.w	%r8,0x1				; RV_RAM_BASE, without an ext pair:
	sll	%r8,31				; subtracting it and adding it are
	add	%r8,%r2				; the same modulo 2^32
	jp	%r13

; Store the guest pc back and return the number retired.  The count can exceed
; the batch by up to one block: a block that has started always runs whole, so
; that what it charged and what it executed are the same number.
.Ljit_leave:					; %r13 = the guest pc
	ext	OFF_PC
	ld.w	[%r0],%r13
	ld.w	%r4,[%sp+0x0]
	sub	%r4,%r6
	add	%sp,0x1
	popn	%r3
	ret

; ------------------------------------------------------- an indirect jump --
;
; The map is a direct-mapped cache of guest pc to translated code, two words a
; slot.  A miss is not an error: the block may exist and have been thrown out
; of the map by a collision, or may never have been translated, and C settles
; which.  One guest instruction in 37 is a jalr, so this is on the hot path
; whatever the links do.  A block has written every allocated register back
; before it gets here, so the six are free to clobber -- what it jumps to is a
; cold entry, which loads its own.

	.globl	rv32_jit_stub_indirect
rv32_jit_stub_indirect:				; %r13 = the guest pc wanted
	xld.w	%r5,rv32_jit_map
	ld.w	%r4,%r13
	srl	%r4,2
	xand	%r4,JIT_MASK
	sll	%r4,3
	add	%r4,%r5
	ld.w	%r14,[%r4]			; the pc that slot answers to
	cmp	%r14,%r13
	jrne	1f
	ext	4
	ld.w	%r4,[%r4]			; ...and where its code is
	jp	%r4
1:	xjp	.Ljit_leave

; ------------------------------------------------------- giving up --------
;
; A block that cannot go on -- an address it cannot take, a batch that is
; spent -- arrives here with %r5 naming the
; block, the instruction within it and the reason, and with its seven allocated
; registers still holding guest registers.  They are spilled where C can see
; them and rv32_jit_fault() writes them back by the block's map, so the block
; itself spends nothing on the way out: three words to load %r5 and a jump.

	.globl	rv32_jit_stub_fault
rv32_jit_stub_fault:				; %r5 = code
	xld.w	%r13,rv32_jit
	ext	JOFF_SPILL
	ld.w	[%r13],%r1
	ext	JOFF_SPILL+4
	ld.w	[%r13],%r7
	ext	JOFF_SPILL+8
	ld.w	[%r13],%r9
	ext	JOFF_SPILL+12
	ld.w	[%r13],%r10
	ext	JOFF_SPILL+16
	ld.w	[%r13],%r11
	ext	JOFF_SPILL+20
	ld.w	[%r13],%r12
	ext	JOFF_SPILL+24
	ld.w	[%r13],%r14
	sub	%sp,0x1
	ld.w	[%sp+0x0],%r6			; the batch counter has to survive
	ld.w	%r7,%r5				; arg1: what and where
	ld.w	%r6,%r0				; arg0: s
	xcall	rv32_jit_fault
	ld.w	%r13,%r4			; where the guest resumes
	ld.w	%r6,[%sp+0x0]
	add	%sp,0x1
	xld.w	%r5,rv32_jit			; and what the block over-charged
	ext	JOFF_BACK
	ld.w	%r5,[%r5]
	add	%r6,%r5
	xjp	.Ljit_leave

; ------------------------------------------------------- device registers --
;
; An address outside guest RAM is a device register, and a Linux boot makes one
; every 110 instructions -- two thirds of everything this would otherwise hand
; back to C.  Handing one back costs a return, one instruction interpreted from
; SDRAM, a lookup and a re-entry, which measured about four thousand cycles;
; doing it here is a call and a return.  These are the only stubs that go back
; into the code cache, and they do it with `ret`: the address the block's `call`
; pushed is still on the stack.
;
; What the block has live and C is free to clobber goes on the stack around
; the call: the batch counter and the six allocated registers that are not
; %r1.  The stack is at the top of SDRAM, so every word of it is a bus cycle
; and this is chosen over pushn, which would be fifteen words each way for
; two instructions.  %r8 is not saved: it is a function of %r2, which C
; keeps, and three words rebuild it.
;
; Both halves are subroutines rather than macros, so that the five stubs fit
; beside the interpreter's table: a call here lands in internal RAM and costs
; six cycles, not thirty.  Each leaves its own return address on the stack
; and goes back through a register it has just saved or is about to restore,
; so the saved words sit at a fixed offset and %r13 -- which three stubs
; arrive with an argument in -- survives the save.

.Lsave:						; call: [sp] = the return address
	sub	%sp,0x7
	ld.w	[%sp+0x0],%r6
	ld.w	[%sp+0x1],%r7
	ld.w	[%sp+0x2],%r9
	ld.w	[%sp+0x3],%r10
	ld.w	[%sp+0x4],%r11
	ld.w	[%sp+0x5],%r12
	ld.w	[%sp+0x6],%r14
	ld.w	%r6,[%sp+0x7]
	jp	%r6				; leaving eight words

.Lrestore:					; call: saved words at [sp+1..7]
	ld.w	%r6,[%sp+0x1]
	ld.w	%r7,[%sp+0x2]
	ld.w	%r9,[%sp+0x3]
	ld.w	%r10,[%sp+0x4]
	ld.w	%r11,[%sp+0x5]
	ld.w	%r12,[%sp+0x6]
	ld.w	%r14,[%sp+0x7]
	ld.w	%r13,[%sp+0x0]
	add	%sp,0x9				; both return addresses too
	ld.w	%r8,0x1				; ram - RV_RAM_BASE, again
	sll	%r8,31
	add	%r8,%r2
	jp	%r13

; The C interpreter truncates nothing on a device access -- rv32.c hands the
; whole word back on a load and the whole register out on a store -- so the
; width does not appear here either, and one stub covers all of them.

	.globl	rv32_jit_stub_dev_load
rv32_jit_stub_dev_load:				; %r4 host address -> %r5 value
	call	.Lsave
	ld.w	%r7,%r4
	sub	%r7,%r8				; arg1: the guest address
	ld.w	%r6,%r0				; arg0: s
	xcall	rv32_mmio_load
	ld.w	%r5,%r4
	call	.Lrestore
	ret

; The marker and SYSCON are still C's: one reads the retired count, which is
; not published until this returns, and the other stops the machine, which is
; a thing only the interpreter's caller can do.  Returns with the flags set by
; a compare of the answer with zero: `jrne` after the call is the decline.
	.globl	rv32_jit_stub_dev_store
rv32_jit_stub_dev_store:			; %r4 address, %r5 value
	call	.Lsave
	ld.w	%r7,%r4
	sub	%r7,%r8				; arg1: the guest address
	ld.w	%r8,%r5				; arg2: the value
	ld.w	%r6,%r0				; arg0: s
	xcall	rv32_mmio_store_hot
	call	.Lrestore
	cmp	%r4,0x0				; nonzero: this one is C's to do
	ret

; ------------------------------------------------- CSRs and atomics ------
;
; Both were declines -- a return to C, the instruction interpreted, a lookup
; and a re-entry, some two thousand cycles -- and a Linux boot does one every
; two hundred instructions: interrupts are masked and unmasked with a CSR
; write, and every lock is an atomic.  Now they are a call.  The CSR helper
; can still decline one, and says so the way the device store does.

	.globl	rv32_jit_stub_csr
rv32_jit_stub_csr:				; %r13 ir, %r4 rs1 -> %r5 old, flags
	call	.Lsave
	ld.w	%r7,%r13			; arg1: the instruction
	ld.w	%r8,%r4				; arg2: x[rs1]
	xld.w	%r9,rv32_jit
	add	%r9,JOFF_CSRVAL			; arg3: where the old value goes
						; (fits the field: a prefix here
						; would sit above it, not in it)
	ld.w	%r6,%r0				; arg0: s
	xcall	rv32_csr_hot
	xld.w	%r5,rv32_jit
	ext	JOFF_CSRVAL
	ld.w	%r5,[%r5]
	call	.Lrestore
	cmp	%r4,0x0				; nonzero: this one is C's to do
	ret

	.globl	rv32_jit_stub_amo
rv32_jit_stub_amo:				; %r13 ir, %r4 host address, %r5 value -> %r5
	call	.Lsave
	ld.w	%r7,%r13			; arg1: the instruction
	ld.w	%r8,%r4				; arg2: the host address
	ld.w	%r9,%r5				; arg3: x[rs2]
	ld.w	%r6,%r0				; arg0: s
	xcall	rv32_amo_hot
	ld.w	%r5,%r4
	call	.Lrestore
	ret

; ------------------------------------------------------- the divides ------
;
; The M operations with no C33 instruction behind them: rv32_divop in rv32.c,
; called the same way and with the same registers kept.  A block wants its
; result in some register of its own, and it is cheaper to move it there
; afterwards than to make the call site the size of this.

	.globl	rv32_jit_stub_divop
rv32_jit_stub_divop:				; %r13 funct3, %r4 a, %r5 b -> %r4
	call	.Lsave
	ld.w	%r6,%r13
	ld.w	%r7,%r4
	ld.w	%r8,%r5
	xcall	rv32_divop
	call	.Lrestore
	ret
