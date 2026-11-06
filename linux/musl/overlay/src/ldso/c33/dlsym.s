	.cfi_sections .debug_frame
	.text
	.global dlsym
	.hidden __dlsym
	.type dlsym,@function
	.align 1
dlsym:
	.cfi_startproc
	ld.w	%r8,[%sp]
	xjp	__dlsym
	.cfi_endproc
	.size dlsym,.-dlsym
