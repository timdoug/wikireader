	.text
	.global _start
_start:
	xcall	extfunc
	xjp	extfunc
	xld.w	%r4, extdata
	call	near_target
near_target:
	ret
