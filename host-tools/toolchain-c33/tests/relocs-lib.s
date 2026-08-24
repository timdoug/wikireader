	.text
	.global extfunc
extfunc:
	ret
	.data
	.long 0x11111111
	.long 0x22222222
	.global extdata
extdata:
	.long 0xdeadbeef
