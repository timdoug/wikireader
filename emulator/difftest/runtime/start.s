	.text
	.global	_start
_start:
	xld.w	%r4, __stack_top
	ld.w	%sp, %r4
	xld.w	%r15, __dp		/* gcc addresses globals off %r15 */

	/* zero .bss */
	xld.w	%r6, __bss_start
	xld.w	%r7, __bss_end
	ld.w	%r8, 0
1:
	cmp	%r6, %r7
	jrge	2f
	ld.b	[%r6], %r8
	add	%r6, 1
	jp	1b
2:
	xcall	main
	halt
