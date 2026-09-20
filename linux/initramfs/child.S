/* SPDX-License-Identifier: GPL-2.0 */
	.section .text.start,"ax"
	.global _start
	.type _start,@function
_start:
	call child_main
	ld.w %r6,%r4
	xld.w %r4,93
	int 0
1:
	jp 1b
	.size _start,.-_start

	.text
	.global c33_write
	.type c33_write,@function
c33_write:
	xld.w %r4,64
	int 0
	ret
	.size c33_write,.-c33_write
