	.text
	.align 1
	.global wr_checked_errno_entry
	.type wr_checked_errno_entry,@function
wr_checked_errno_entry:
	ld.w %r6,%sp
	and %r6,15
	xjp wr_checked_errno_body
	.size wr_checked_errno_entry,.-wr_checked_errno_entry
