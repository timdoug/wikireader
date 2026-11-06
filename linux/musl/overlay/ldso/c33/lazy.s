# __c33_lazy_resolve: entered by jump from a module's .plt head, which a
# lazy stub calls with the relocation's offset in %r13, with %r15 ld.so's.
# [%sp] is the return into the stub, which names the module; [%sp+4] the
# return to the original caller, whose arguments are untouched in
# %r6-%r12 and on the stack.  Bind the slot, then go where it points with
# the stack as the caller left it.
	.cfi_sections .debug_frame
	.text
	.global __c33_lazy_resolve
	.hidden __c33_lazy_resolve
	.hidden __c33_lazy_bind
	.type __c33_lazy_resolve,@function
	.align 1
__c33_lazy_resolve:
	.cfi_startproc
	pushn	%r12
	.cfi_adjust_cfa_offset 52
	.cfi_rel_offset %r0, 0
	.cfi_rel_offset %r1, 4
	.cfi_rel_offset %r2, 8
	.cfi_rel_offset %r3, 12
	.cfi_rel_offset %r4, 16
	.cfi_rel_offset %r5, 20
	.cfi_rel_offset %r6, 24
	.cfi_rel_offset %r7, 28
	.cfi_rel_offset %r8, 32
	.cfi_rel_offset %r9, 36
	.cfi_rel_offset %r10, 40
	.cfi_rel_offset %r11, 44
	.cfi_rel_offset %r12, 48
	# The call pushed two return addresses: 8 modulo 16, then 52 bytes
	# here and 4 more leave the argument base aligned.
	sub	%sp,1
	.cfi_adjust_cfa_offset 4
	xld.w	%r6,[%sp+56]
	ld.w	%r7,%r13
	xcall	__c33_lazy_bind
	ld.w	%r13,%r4
	add	%sp,1
	.cfi_adjust_cfa_offset -4
	popn	%r12
	.cfi_adjust_cfa_offset -52
	.cfi_restore %r0
	.cfi_restore %r1
	.cfi_restore %r2
	.cfi_restore %r3
	.cfi_restore %r4
	.cfi_restore %r5
	.cfi_restore %r6
	.cfi_restore %r7
	.cfi_restore %r8
	.cfi_restore %r9
	.cfi_restore %r10
	.cfi_restore %r11
	.cfi_restore %r12
	# The stub's return address.  From here the stub's frame is gone and
	# [%sp] is the original caller's return: unwind straight to it.
	add	%sp,1
	.cfi_def_cfa_offset 4
	xld.w	%r15,[%r13+4]
	ld.w	%r14,[%r13]
	jp	%r14
	.cfi_endproc
	.size __c33_lazy_resolve,.-__c33_lazy_resolve
