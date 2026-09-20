#include <jmpbuf-offsets.h>

	.text
	.align 1
	.global setjmp
	.global __GI_setjmp
	.hidden __GI_setjmp
	.type setjmp,@function
setjmp:
__GI_setjmp:
	ld.w %r7,1
	xjp __sigsetjmp
	.size setjmp,.-setjmp

	.global _setjmp
	.global __GI__setjmp
	.hidden __GI__setjmp
	.type _setjmp,@function
_setjmp:
__GI__setjmp:
	ld.w %r7,0
	xjp __sigsetjmp
	.size _setjmp,.-_setjmp

	.global __sigsetjmp
	.global __GI___sigsetjmp
	.hidden __GI___sigsetjmp
	.type __sigsetjmp,@function
__sigsetjmp:
__GI___sigsetjmp:
	xld.w [%r6+JB_R0*4],%r0
	xld.w [%r6+JB_R1*4],%r1
	xld.w [%r6+JB_R2*4],%r2
	xld.w [%r6+JB_R3*4],%r3
	ld.w %r4,%sp
	add %r4,1
	xld.w [%r6+JB_SP*4],%r4
	xld.w %r4,[%sp]
	xld.w [%r6+JB_PC*4],%r4
	xjp __sigjmp_save
	.size __sigsetjmp,.-__sigsetjmp
