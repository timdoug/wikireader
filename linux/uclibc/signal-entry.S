/* SPDX-License-Identifier: GPL-2.0-only */
/* Capture handler-entry SP before a compiler prologue changes it.  Tail
 * jumps retain the kernel-supplied return address and module data pointer.
 * The extra argument is the entry SP residue modulo the call alignment. */

	.text
	.balign 2
	.global c33_signal_handler_entry
	.type c33_signal_handler_entry, @function
	.hidden c33_signal_handler_body
c33_signal_handler_entry:
	ld.w %r7,%sp
	and %r7,15
	xjp c33_signal_handler_body
	.size c33_signal_handler_entry, .-c33_signal_handler_entry

	.global c33_signal_alt_entry
	.type c33_signal_alt_entry, @function
	.hidden c33_signal_alt_body
c33_signal_alt_entry:
	ld.w %r9,%sp
	and %r9,15
	xjp c33_signal_alt_body
	.size c33_signal_alt_entry, .-c33_signal_alt_entry

	.section .note.GNU-stack,"",@progbits
