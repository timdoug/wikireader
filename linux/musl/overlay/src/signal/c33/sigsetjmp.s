# sigsetjmp(env, savemask): with a mask to save, setjmp returns here, once
# or twice, and __sigsetjmp_tail saves or restores the mask.  Our return
# address, which a later call by the caller overwrites, goes in __fl; the
# caller's %r0, which carries env through setjmp, in __ss[2]; and libc's
# %r15, which longjmp's caller may not have, in __ss[3].  The kernel's
# mask takes __ss[0] and __ss[1].
	.text
	.global sigsetjmp
	.global __sigsetjmp
	.type sigsetjmp,@function
	.type __sigsetjmp,@function
	.hidden ___setjmp
	.hidden __sigsetjmp_tail
	.align 1
sigsetjmp:
__sigsetjmp:
	cmp	%r7,0
	xjreq	___setjmp
	pop	%r4
	xld.w	[%r6+24],%r4
	xld.w	[%r6+36],%r0
	xld.w	[%r6+40],%r15
	ld.w	%r0,%r6
	xcall	___setjmp
	ld.w	%r7,%r4
	ld.w	%r6,%r0
	xld.w	%r15,[%r6+40]
	xld.w	%r0,[%r6+36]
	xld.w	%r4,[%r6+24]
	push	%r4
	xjp	__sigsetjmp_tail
	.size sigsetjmp,.-sigsetjmp
