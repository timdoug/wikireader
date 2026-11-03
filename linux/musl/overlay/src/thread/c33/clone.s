# __clone(fn, stack, flags, arg, ptid, tls, ctid): fn, stack and flags
# arrive in %r6-%r8; as a variadic function's last named argument, arg is
# on the stack, followed by ptid, tls and ctid.  The kernel takes flags,
# stack, ptid, ctid and tls in %r6-%r10 and gives the child a copy of
# every register, so fn, a descriptor, and arg cross in %r0 and %r1.
	.text
	.global __clone
	.hidden __clone
	.type __clone,@function
	.align 1
__clone:
	pushn	%r1
	ld.w	%r0,%r6
	xld.w	%r1,[%sp+12]
	ld.w	%r6,%r8
	ld.w	%r4,%r7
	and	%r4,-16
	ld.w	%r7,%r4
	xld.w	%r8,[%sp+16]
	xld.w	%r10,[%sp+20]
	xld.w	%r9,[%sp+24]
	xld.w	%r4,220
	int	0
	cmp	%r4,0
	jreq	1f
	popn	%r1
	ret

1:	ld.w	%r6,%r1
	xld.w	%r15,[%r0+4]
	ld.w	%r0,[%r0]
	call	%r0
	ld.w	%r6,%r4
	xld.w	%r4,93
	int	0
2:	jp	2b
	.size __clone,.-__clone
