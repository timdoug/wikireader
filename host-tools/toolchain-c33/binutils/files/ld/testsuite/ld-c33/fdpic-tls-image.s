# An address in the TLS image and one in .data, in an FDPIC executable.
	.text
	.global	_start
_start:
	ret
	.section .rodata
str:	.asciz	"s1"
	.section .tdata,"awT",@progbits
	.align	2
tp:	.long	str
	.data
	.align	2
dp:	.long	str
