; rv32_div.s - the guest's divides, which the C33 PE has no instruction for.
;
;   uint32_t rv32_divop(uint32_t funct3, uint32_t a, uint32_t b);
;
; and the same body entered from translated code as rv32_jit_stub_divop, with
; funct3 in %r13, the operands in %r4 and %r5 and the answer in %r4, keeping
; every other register.  funct3 is the guest's: 4 div, 5 divu, 6 rem, 7 remu,
; so bit 0 says unsigned and bit 1 says remainder; the C entry also takes 2,
; mulhsu, which the interpreter hands here and the translator does inline.
;
; This replaced libgcc's __udivmodsi4, which this same internal RAM held and
; which still cost 1,600 cycles a divide: a bit a pass through two loops,
; each pass a taken branch, all 32 bits every time.  This runs only the
; quotient's bits.  The divisor is shifted up under the dividend in five
; halvings -- by 16 if it still fits, then 8, 4, 2, 1 -- which is the exact
; count of quotient bits, and that count picks where to enter one straight
; line of 32 identical steps.  A step is six words and one branch that is
; taken half the time.  Signs are stripped first and put back at the end
; from two bits kept beside funct3; RISC-V's answers for a zero divisor and
; for the one overflowing signed case fall out of the same path with one
; special entry apiece.
;
; Two of the block's registers are needed as scratch and parked in the
; multiplier's result registers rather than on the stack, which is at the top
; of SDRAM and costs ten cycles a word to read back.

	.section .ivram_code,"ax"
	.align	1

	.globl	rv32_divop
rv32_divop:				; C: %r6 funct3, %r7 a, %r8 b -> %r4
	cmp	%r6,0x2
	xjreq	.Lmulhsu
	ld.w	%r13,%r6
	ld.w	%r4,%r7
	ld.w	%r5,%r8

	.globl	rv32_jit_stub_divop
rv32_jit_stub_divop:			; %r13 funct3, %r4 a, %r5 b -> %r4
	ld.w	%alr,%r7
	ld.w	%ahr,%r9
	ld.w	%r9,%r13
	and	%r9,0x1
	jrne	.Lunsigned
	cmp	%r4,0x0
	jrge	1f
	not	%r4,%r4
	add	%r4,0x1
	xor	%r13,0x18		; a negative: quotient and remainder both
1:	cmp	%r5,0x0
	jrge	.Lunsigned
	not	%r5,%r5
	add	%r5,0x1
	xor	%r13,0x8		; b negative: the quotient's sign flips
.Lunsigned:
	cmp	%r5,0x0
	xjreq	.Lzero
	cmp	%r4,%r5
	xjrult	.Lsmall

	; The divisor under the dividend: the largest k with (b << k) <= a,
	; found greedily, and steps = k + 1 quotient bits to produce.
	ld.w	%r9,0x1
	ld.w	%r7,%r4
	srl	%r7,16
	cmp	%r5,%r7
	jrugt	1f
	sll	%r5,16
	add	%r9,16
1:	ld.w	%r7,%r4
	srl	%r7,8
	cmp	%r5,%r7
	jrugt	1f
	sll	%r5,8
	add	%r9,8
1:	ld.w	%r7,%r4
	srl	%r7,4
	cmp	%r5,%r7
	jrugt	1f
	sll	%r5,4
	add	%r9,4
1:	ld.w	%r7,%r4
	srl	%r7,2
	cmp	%r5,%r7
	jrugt	1f
	sll	%r5,2
	add	%r9,2
1:	ld.w	%r7,%r4
	srl	%r7,1
	cmp	%r5,%r7
	jrugt	1f
	sll	%r5,1
	add	%r9,1
1:
	; Skip the first 32 - steps of the line: twelve bytes a step.
	xld.w	%r7,32
	sub	%r7,%r9
	ld.w	%r9,%r7
	sll	%r9,3
	sll	%r7,2
	add	%r9,%r7
	xld.w	%r7,.Lbody
	add	%r9,%r7
	ld.w	%r7,0x0			; the quotient
	jp	%r9

.Lbody:
	.rept	32
	add	%r7,%r7
	cmp	%r4,%r5
	jrult	1f
	sub	%r4,%r5
	or	%r7,0x1
1:	srl	%r5,1
	.endr

.Lresult:				; %r4 remainder, %r7 quotient
	ld.w	%r9,%r13
	and	%r9,0x2
	jrne	1f
	ld.w	%r4,%r7
	ld.w	%r9,%r13
	and	%r9,0x8
	jreq	.Ldone
	not	%r4,%r4
	add	%r4,0x1
	jp	.Ldone
1:	ld.w	%r9,%r13
	and	%r9,0x10
	jreq	.Ldone
	not	%r4,%r4
	add	%r4,0x1
.Ldone:
	ld.w	%r7,%alr
	ld.w	%r9,%ahr
	ret

.Lzero:					; b == 0: all ones, and a for the remainder
	ld.w	%r7,0x0
	not	%r7,%r7
	xand	%r13,0xfffffff7		; the quotient is not signed
	jp	.Lresult
.Lsmall:				; a < b: nothing to divide
	ld.w	%r7,0x0
	jp	.Lresult

.Lmulhsu:				; C only: the high word, signed by unsigned
	cmp	%r7,0x0
	mltu.w	%r7,%r8
	ld.w	%r4,%ahr
	jrge	1f
	sub	%r4,%r8
1:	ret
