/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/linkage.h>

	.text

	.global c33_timer_vector
	.type c33_timer_vector,@function
c33_timer_vector:
	push %r4
	xld.w %r4,38
	xjp c33_exception
	.size c33_timer_vector,.-c33_timer_vector

	.global c33_fault_vector
	.type c33_fault_vector,@function
c33_fault_vector:
	push %r4
	xld.w %r4,6
	xjp c33_exception
	.size c33_fault_vector,.-c33_fault_vector

	.global c33_syscall_vector
	.type c33_syscall_vector,@function
c33_syscall_vector:
	push %r4
	xld.w %r4,12
	xjp c33_exception
	.size c33_syscall_vector,.-c33_syscall_vector

	.global c33_unhandled_vector
	.type c33_unhandled_vector,@function
c33_unhandled_vector:
1:
	halt
	jp 1b
	.size c33_unhandled_vector,.-c33_unhandled_vector

	.type c33_exception,@function
c33_exception:
	/*
	 * The core has pushed PSR and PC, and the vector stub has pushed r4.
	 * Reserving another 21 words makes a 96-byte pt_regs frame whose final
	 * two words are the hardware return frame consumed by RETI.
	 */
	sub %sp,21
	xld.w [%sp+0],%r0
	xld.w [%sp+4],%r1
	xld.w [%sp+8],%r2
	xld.w [%sp+12],%r3
	xld.w [%sp+20],%r5
	xld.w [%sp+24],%r6
	xld.w [%sp+28],%r7
	xld.w [%sp+32],%r8
	xld.w [%sp+36],%r9
	xld.w [%sp+40],%r10
	xld.w [%sp+44],%r11
	xld.w [%sp+48],%r12
	xld.w [%sp+52],%r13
	xld.w [%sp+56],%r14
	xld.w [%sp+60],%r15
	xld.w [%sp+76],%r4
	xld.w %r5,[%sp+84]
	xld.w [%sp+16],%r5
	ld.w %r5,%alr
	xld.w [%sp+64],%r5
	ld.w %r5,%ahr
	xld.w [%sp+68],%r5
	ld.w %r5,%sp
	xadd %r5,96
	xld.w [%sp+72],%r5

	/* C33 ABI: vector in r6, pt_regs pointer in r7. */
	ld.w %r6,%r4
	ld.w %r7,%sp
	ld.w %r5,%sp
	xand %r5,0xfffffff0
	ld.w %sp,%r5
	xld.w %r15,__dp
	xcall c33_handle_irq
	ld.w %r6,%r4

	.global c33_restore
	.type c33_restore,@function
c33_restore:
	psrclr 4
	ld.w %sp,%r6
	xld.w %r5,[%sp+64]
	ld.w %alr,%r5
	xld.w %r5,[%sp+68]
	ld.w %ahr,%r5
	xld.w %r0,[%sp+0]
	xld.w %r1,[%sp+4]
	xld.w %r2,[%sp+8]
	xld.w %r3,[%sp+12]
	xld.w %r4,[%sp+16]
	xld.w %r5,[%sp+20]
	xld.w %r6,[%sp+24]
	xld.w %r7,[%sp+28]
	xld.w %r8,[%sp+32]
	xld.w %r9,[%sp+36]
	xld.w %r10,[%sp+40]
	xld.w %r11,[%sp+44]
	xld.w %r12,[%sp+48]
	xld.w %r13,[%sp+52]
	xld.w %r14,[%sp+56]
	xld.w %r15,[%sp+60]
	add %sp,22
	reti
	.size c33_restore,.-c33_restore
	.size c33_exception,.-c33_exception

	.section .vectors,"a"
	.balign 1024
	.global c33_vector_table
c33_vector_table:
	.rept 6
	.long c33_unhandled_vector
	.endr
	.long c33_fault_vector
	.rept 5
	.long c33_unhandled_vector
	.endr
	.long c33_syscall_vector
	.rept 25
	.long c33_unhandled_vector
	.endr
	.long c33_timer_vector
	.rept 217
	.long c33_unhandled_vector
	.endr
