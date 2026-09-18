; rv32_jit.s - the runtime a translated block is entered from and leaves by.
;
;   uint32_t rv32_jit_enter(rv32_t *s, uint32_t budget, void *code);
;
; Sets up the registers every translated block assumes, jumps into one, and
; returns how many guest instructions were retired before the code cache
; handed control back.  rv32_jit.h lists the register assignment; the short
; version is that it is rv32_hot.s's, for the same reasons, plus two registers
; holding the host range a store has to stay outside.
;
; This is small on purpose.  The interpreter needed all of A0 RAM because its
; dispatch loop cannot fit the C33's 27-byte fetch window at any alignment; a
; translated block can, and jit_probe.s measured a well-translated loop running
; *faster* from SDRAM than from internal RAM.  So the code cache is in SDRAM
; and what lives in the fast memory is only this: the entry, the lookup an
; indirect guest jump takes, and the three ways a block gives up.
;
; None of the three carries a guest pc.  They are reached with `call`, which on
; this core pushes the return address on the stack, and rv32_jit_fault() turns
; that address back into the guest instruction that asked.  Three words at
; every check instead of the eight a block would otherwise spend keeping s->pc
; up to date, on a path that is taken once in seventy-eight instructions.

	.set	OFF_PC, 128
	.set	OFF_RAM, 132
	.set	OFF_RAM_SIZE, 136

	.set	JOFF_MAP, 0			; rv32_jit.h's RV32_JOFF_*
	.set	JOFF_WATCH_LO, 4
	.set	JOFF_WATCH_HI, 8
	.set	JOFF_BACK, 12
	.set	JIT_MASK, 32767			; rv32_jit.h's RV32_JIT_MASK

; This is in SDRAM, not A0 RAM: rv32_hot.s needs every byte of the fast memory
; and is still what interprets everything the translator has not earned its way
; into.  What that costs is one entry and one exit per two hundred guest
; instructions, and one call per device register.
	.section .text,"ax"
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
	add	%r3,%r2				; ram_end
	ld.w	%r8,0x1				; RV_RAM_BASE, without an ext pair:
	sll	%r8,31				; subtracting it and adding it are
	add	%r8,%r2				; the same modulo 2^32
	xld.w	%r4,rv32_jit
	ext	JOFF_WATCH_LO
	ld.w	%r9,[%r4]
	ext	JOFF_WATCH_HI
	ld.w	%r10,[%r4]
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
; whatever the links do.

	.globl	rv32_jit_stub_indirect
rv32_jit_stub_indirect:				; %r13 = the guest pc wanted
	xld.w	%r5,rv32_jit
	ld.w	%r5,[%r5]			; the map
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

	.globl	rv32_jit_stub_decline
rv32_jit_stub_decline:				; an instruction, or an address
	ld.w	%r13,0x0
	xjp	.Ljit_ask

	.globl	rv32_jit_stub_store
rv32_jit_stub_store:				; a store inside the watched range
	ld.w	%r13,0x1
	ld.w	%r14,%r4			; where it went
	xjp	.Ljit_ask

	.globl	rv32_jit_stub_budget
rv32_jit_stub_budget:				; the batch is spent
	ld.w	%r13,0x2
	xjp	.Ljit_ask

; The return address on the stack says which guest instruction asked.  It is
; read before the slot is reused for the batch counter, which has to survive
; the call and is not one of the four registers that would.
.Ljit_ask:
	ld.w	%r7,[%sp+0x0]			; arg1: the host return address
	ld.w	[%sp+0x0],%r6
	ld.w	%r8,%r13			; arg2: why
	ld.w	%r9,%r14			; arg3: the address, for a store
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
; The C interpreter truncates nothing on a device access -- rv32.c hands the
; whole word back on a load and the whole register out on a store -- so the
; width does not appear here either, and one stub covers all of them.

	.globl	rv32_jit_stub_dev_load
rv32_jit_stub_dev_load:				; %r4 host address -> %r5 value
	sub	%sp,0x4
	ld.w	[%sp+0x0],%r6
	ld.w	[%sp+0x1],%r8
	ld.w	[%sp+0x2],%r9
	ld.w	[%sp+0x3],%r10
	ld.w	%r7,%r4
	sub	%r7,%r8				; arg1: the guest address
	ld.w	%r6,%r0				; arg0: s
	xcall	rv32_mmio_load
	ld.w	%r5,%r4
	ld.w	%r6,[%sp+0x0]
	ld.w	%r8,[%sp+0x1]
	ld.w	%r9,[%sp+0x2]
	ld.w	%r10,[%sp+0x3]
	add	%sp,0x4
	ret

; The marker and SYSCON are still C's: one reads the retired count, which is
; not published until this returns, and the other stops the machine, which is
; a thing only the interpreter's caller can do.
	.globl	rv32_jit_stub_dev_store
rv32_jit_stub_dev_store:			; %r4 address, %r5 value
	sub	%sp,0x4
	ld.w	[%sp+0x0],%r6
	ld.w	[%sp+0x1],%r8
	ld.w	[%sp+0x2],%r9
	ld.w	[%sp+0x3],%r10
	ld.w	%r7,%r4
	sub	%r7,%r8				; arg1: the guest address
	ld.w	%r8,%r5				; arg2: the value
	ld.w	%r6,%r0				; arg0: s
	xcall	rv32_mmio_store_hot
	ld.w	%r13,%r4			; nonzero: this one is C's to do
	ld.w	%r6,[%sp+0x0]
	ld.w	%r8,[%sp+0x1]
	ld.w	%r9,[%sp+0x2]
	ld.w	%r10,[%sp+0x3]
	add	%sp,0x4
	cmp	%r13,0x0
	jrne	1f
	ret
1:	ld.w	%r13,0x0			; RV32_JIT_DECLINE, and the return
	xjp	.Ljit_ask			; address is still where it needs
