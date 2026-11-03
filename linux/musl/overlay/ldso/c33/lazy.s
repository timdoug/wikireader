# __c33_lazy_resolve: entered by jump from a module's .plt head, which a
# lazy stub calls with the relocation's offset in %r13, with %r15 ld.so's.
# [%sp] is the return into the stub, which names the module; [%sp+4] the
# return to the original caller, whose arguments are untouched in
# %r6-%r12 and on the stack.  Bind the slot, then go where it points with
# the stack as the caller left it.
	.text
	.global __c33_lazy_resolve
	.hidden __c33_lazy_resolve
	.hidden __c33_lazy_bind
	.type __c33_lazy_resolve,@function
	.align 1
__c33_lazy_resolve:
	pushn	%r12
	# The call pushed two return addresses: 8 modulo 16, then 52 bytes
	# here and 4 more leave the argument base aligned.
	sub	%sp,1
	xld.w	%r6,[%sp+56]
	ld.w	%r7,%r13
	xcall	__c33_lazy_bind
	ld.w	%r13,%r4
	add	%sp,1
	popn	%r12
	# The stub's return address.
	add	%sp,1
	xld.w	%r15,[%r13+4]
	ld.w	%r14,[%r13]
	jp	%r14
	.size __c33_lazy_resolve,.-__c33_lazy_resolve
