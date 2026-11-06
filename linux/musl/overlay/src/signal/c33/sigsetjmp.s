# sigsetjmp(env, savemask): with a mask to save, setjmp returns here, once
# or twice, and __sigsetjmp_tail saves or restores the mask.  Our return
# address, which a later call by the caller overwrites, goes in __fl; the
# caller's %r0, which carries env through setjmp, in __ss[2]; and libc's
# %r15, which longjmp's caller may not have, in __ss[3].  The kernel's
# mask takes __ss[0] and __ss[1].
	.cfi_sections .debug_frame
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
	.cfi_startproc
	cmp	%r7,0
	xjreq	___setjmp
	pop	%r4
	.cfi_adjust_cfa_offset -4
	.cfi_register 22, %r4
	xld.w	[%r6+24],%r4
	xld.w	[%r6+36],%r0
	xld.w	[%r6+40],%r15
	ld.w	%r0,%r6
	# Across setjmp, and after a second return through longjmp, the
	# return address and the caller's %r0 are only in env, at [%r0+24]
	# and [%r0+36]: DW_CFA_expression, DW_OP_breg0.  (%r15 is libc's
	# here; a caller restores its own.)
	.cfi_escape 0x10, 22, 2, 0x70, 24
	.cfi_escape 0x10, 0, 2, 0x70, 36
	xcall	___setjmp
	ld.w	%r7,%r4
	ld.w	%r6,%r0
	xld.w	%r15,[%r6+40]
	# env is in %r6 too: the return address by DW_OP_breg6, before %r0
	# is reloaded.
	.cfi_escape 0x10, 22, 2, 0x76, 24
	xld.w	%r0,[%r6+36]
	.cfi_restore %r0
	xld.w	%r4,[%r6+24]
	.cfi_register 22, %r4
	push	%r4
	.cfi_adjust_cfa_offset 4
	.cfi_restore 22
	xjp	__sigsetjmp_tail
	.cfi_endproc
	.size sigsetjmp,.-sigsetjmp
