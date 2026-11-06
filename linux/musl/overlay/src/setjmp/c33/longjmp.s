	.cfi_sections .debug_frame
	.text
	.global _longjmp
	.global longjmp
	.type _longjmp,@function
	.type longjmp,@function
	.align 1
_longjmp:
longjmp:
	.cfi_startproc
	ld.w	%r4,%r7
	cmp	%r4,0
	jrne	1f
	ld.w	%r4,1
1:	ld.w	%r0,[%r6]
	xld.w	%r1,[%r6+4]
	xld.w	%r2,[%r6+8]
	xld.w	%r3,[%r6+12]
	xld.w	%r5,[%r6+20]
	xld.w	%r7,[%r6+16]
	ld.w	%sp,%r7
	# setjmp's return, as if from setjmp: its CFA is the stack pointer
	# it saved, and the return address is in %r5.
	.cfi_def_cfa %sp, 0
	.cfi_register 22, %r5
	jp	%r5
	.cfi_endproc
	.size longjmp,.-longjmp
