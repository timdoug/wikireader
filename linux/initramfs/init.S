/* SPDX-License-Identifier: GPL-2.0 */
	.text
	.global _start
	.type _start,@function
_start:
	/* CALL pushes the address immediately after itself: the message. */
	call 1f
message:
	.ascii "WikiReader native C33 userspace is alive!\n"
message_end:
1:	pop %r7

	/* write(1, message, message_end - message) */
	xld.w %r4,64
	xld.w %r6,1
	xld.w %r8,42
	int 0

	/* fcntl(0, F_SETFL, O_NONBLOCK), then wait for one UART byte. */
	xld.w %r4,25
	ld.w %r6,0
	ld.w %r7,4
	xld.w %r8,0x800
	int 0
	sub %sp,1
.Lread:
	xld.w %r4,63
	ld.w %r6,0
	ld.w %r7,%sp
	ld.w %r8,1
	int 0
	cmp %r4,1
	jrne .Lread

	/* A second message proves UART RX reached a Linux userspace read(). */
	call 2f
rx_message:
	.ascii "UART RX reached Linux userspace.\n"
rx_message_end:
	.balign 2
2:	pop %r7
	xld.w %r4,64
	ld.w %r6,1
	xld.w %r8,33
	int 0

	/* Stay alive while timer interrupts preempt native userspace. */
.Lalive:
	jp .Lalive

	.if (message_end-message) != 42
	.error "update the write length"
	.endif
	.if (rx_message_end-rx_message) != 33
	.error "update the RX message length"
	.endif
	.size _start,.-_start
