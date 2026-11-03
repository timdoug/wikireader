	.text
	.global dlsym
	.hidden __dlsym
	.type dlsym,@function
	.align 1
dlsym:
	ld.w	%r8,[%sp]
	xjp	__dlsym
	.size dlsym,.-dlsym
