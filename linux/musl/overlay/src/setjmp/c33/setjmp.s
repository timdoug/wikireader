# %r0-%r3, the caller's stack pointer (CALL pushed the return address) and
# the return address.  %r15 is not saved: a caller restores its own after
# setjmp returns, either time.
	.text
	.global ___setjmp
	.hidden ___setjmp
	.global __setjmp
	.global _setjmp
	.global setjmp
	.type __setjmp,@function
	.type _setjmp,@function
	.type setjmp,@function
	.align 1
___setjmp:
__setjmp:
_setjmp:
setjmp:
	ld.w	[%r6],%r0
	xld.w	[%r6+4],%r1
	xld.w	[%r6+8],%r2
	xld.w	[%r6+12],%r3
	ld.w	%r4,%sp
	add	%r4,4
	xld.w	[%r6+16],%r4
	ld.w	%r4,[%sp]
	xld.w	[%r6+20],%r4
	ld.w	%r4,0
	ret
	.size setjmp,.-setjmp
