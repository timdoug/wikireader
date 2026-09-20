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
