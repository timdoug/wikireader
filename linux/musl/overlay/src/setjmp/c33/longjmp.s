	.text
	.global _longjmp
	.global longjmp
	.type _longjmp,@function
	.type longjmp,@function
	.align 1
_longjmp:
longjmp:
	ld.w	%r4,%r7
	cmp	%r4,0
	jrne	1f
	ld.w	%r4,1
1:	ld.w	%r0,[%r6]
	xld.w	%r1,[%r6+4]
	xld.w	%r2,[%r6+8]
	xld.w	%r3,[%r6+12]
	xld.w	%r7,[%r6+16]
	ld.w	%sp,%r7
	xld.w	%r7,[%r6+20]
	jp	%r7
	.size longjmp,.-longjmp
