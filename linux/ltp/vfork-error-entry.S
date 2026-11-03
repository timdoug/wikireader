	.text
	.align 1
	.global wr_checked_syscall_ret
	.type wr_checked_syscall_ret,@function
wr_checked_syscall_ret:
	ld.w %r7,%sp
	and %r7,15
	xjp wr_checked_syscall_ret_body
	.size wr_checked_syscall_ret,.-wr_checked_syscall_ret
