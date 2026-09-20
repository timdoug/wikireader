/* SPDX-License-Identifier: GPL-2.0 */
	.section .text.start,"ax"
	.global _start
	.type _start,@function
_start:
	call init_main
.Lhang:
	jp .Lhang
	.size _start,.-_start

	.text
	.global c33_fcntl
	.type c33_fcntl,@function
c33_fcntl:
	xld.w %r4,25
	int 0
	ret
	.size c33_fcntl,.-c33_fcntl

	.global c33_read
	.type c33_read,@function
c33_read:
	xld.w %r4,63
	int 0
	ret
	.size c33_read,.-c33_read

	.global c33_write
	.type c33_write,@function
c33_write:
	xld.w %r4,64
	int 0
	ret
	.size c33_write,.-c33_write

	.global c33_getpid
	.type c33_getpid,@function
c33_getpid:
	xld.w %r4,172
	int 0
	ret
	.size c33_getpid,.-c33_getpid

	.global c33_clone
	.type c33_clone,@function
c33_clone:
	xld.w %r4,220
	int 0
	cmp %r4,0
	jrne 1f
	xjp clone_child
1:
	ret
	.size c33_clone,.-c33_clone

	.global c33_execve
	.type c33_execve,@function
c33_execve:
	xld.w %r4,221
	int 0
	ret
	.size c33_execve,.-c33_execve

	.global c33_wait4
	.type c33_wait4,@function
c33_wait4:
	xld.w %r4,260
	int 0
	ret
	.size c33_wait4,.-c33_wait4

	.global c33_exit
	.type c33_exit,@function
c33_exit:
	xld.w %r4,93
	int 0
1:
	jp 1b
	.size c33_exit,.-c33_exit
