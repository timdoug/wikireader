#include <jmpbuf-offsets.h>

	.text
	.align 1
	.global __longjmp
	.global __GI___longjmp
	.hidden __GI___longjmp
	.type __longjmp,@function
__longjmp:
__GI___longjmp:
	ld.w %r4,%r7
	cmp %r4,0
	jrne 1f
	ld.w %r4,1
1:
	xld.w %r0,[%r6+JB_R0*4]
	xld.w %r1,[%r6+JB_R1*4]
	xld.w %r2,[%r6+JB_R2*4]
	xld.w %r3,[%r6+JB_R3*4]
	xld.w %r7,[%r6+JB_SP*4]
	ld.w %sp,%r7
	xld.w %r7,[%r6+JB_PC*4]
	jp %r7
	.size __longjmp,.-__longjmp
