# __syscall_cp_asm(&cancel, nr, u, v, w, x, y, z): a cancellation point.
# A cancel signal taken between __cp_begin and __cp_end, or one that
# restarts the call, which backs the PC up to the int, sends the thread to
# __cp_cancel.  The handler runs in libc, so %r15 is already libc's there.
	.text
	.global __cp_begin
	.hidden __cp_begin
	.global __cp_end
	.hidden __cp_end
	.global __cp_cancel
	.hidden __cp_cancel
	.hidden __cancel
	.global __syscall_cp_asm
	.hidden __syscall_cp_asm
	.type __syscall_cp_asm,@function
	.align 1
__syscall_cp_asm:
__cp_begin:
	ld.w	%r4,[%r6]
	cmp	%r4,0
	jrne	__cp_cancel
	ld.w	%r4,%r7
	ld.w	%r6,%r8
	ld.w	%r7,%r9
	xld.w	%r8,[%sp+4]
	xld.w	%r9,[%sp+8]
	xld.w	%r10,[%sp+12]
	xld.w	%r11,[%sp+16]
	int	0
__cp_end:
	ret
__cp_cancel:
	xjp	__cancel
	.size __syscall_cp_asm,.-__syscall_cp_asm
