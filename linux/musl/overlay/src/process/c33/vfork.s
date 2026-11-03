# The child shares the parent's stack, so the return address CALL left
# there would be popped and overwritten by the child while the parent
# sleeps.  It goes across clone in %r12, and each returns with its own.
	.text
	.global vfork
	.type vfork,@function
	.hidden __syscall_ret
	.align 1
vfork:
	pop	%r12
	xld.w	%r6,0x4111
	ld.w	%r7,0
	ld.w	%r8,0
	ld.w	%r9,0
	ld.w	%r10,0
	xld.w	%r4,220
	int	0
	push	%r12
	ld.w	%r6,%r4
	xjp	__syscall_ret
	.size vfork,.-vfork
