	.text
	.global _start
_start:
	xld.w	%r4, 0x12345678
	ld.w	%r5, %r4
	xcall	target
	add	%r6, %r15
	psrclr	0x4
	jp	%r4
target:
	ret
