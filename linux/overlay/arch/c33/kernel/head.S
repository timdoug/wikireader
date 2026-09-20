/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/linkage.h>
#include <asm/thread_info.h>

	.section .head.text,"ax"
	.global _start
	.type _start,@function
_start:
	psrclr 4
	xld.w %r4,init_stack + THREAD_SIZE
	ld.w %sp,%r4
	xld.w %r15,__dp
	ld.w %r4,0
	ld.w %psr,%r4
	xcall c33_start
1:
	halt
	jp 1b
	.size _start,.-_start
