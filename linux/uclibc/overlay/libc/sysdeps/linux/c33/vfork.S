/* vfork for C33's stack-based CALL/RET convention.  */
#include <features.h>

	.text
	.align 1
	.global __vfork
	.hidden __vfork
	.type __vfork,@function
__vfork:
	/*
	 * CALL left our return address on the shared user stack.  A vfork child
	 * would pop it and then overwrite it with its first nested CALL while
	 * the parent sleeps.  Keep it in a cloned register across clone(2), and
	 * let each task put its own copy back immediately before returning.
	 */
	pop %r12
	xld.w %r6,0x4111	/* CLONE_VM | CLONE_VFORK | SIGCHLD */
	ld.w %r7,0
	ld.w %r8,0
	ld.w %r9,0
	ld.w %r10,0
	ld.w %r11,0
	xld.w %r4,220		/* __NR_clone */
	int 0

	/* Translate Linux's -errno return convention. */
	xld.w %r5,0xfffff000
	cmp %r4,%r5
	jrule 1f
	not %r4,%r4
	add %r4,1
	xld.w %r5,errno
	xld.w [%r5],%r4
	xld.w %r4,-1
1:
	push %r12
	ret
	.size __vfork,.-__vfork

	.weak vfork
	.type vfork,@function
	.set vfork,__vfork

	/* uClibc calls the hidden alias from popen() and other libc internals. */
	.global __GI_vfork
	.hidden __GI_vfork
	.type __GI_vfork,@function
	.set __GI_vfork,__vfork
