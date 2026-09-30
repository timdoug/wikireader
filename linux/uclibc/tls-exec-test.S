/* SPDX-License-Identifier: GPL-2.0-only */
/* Observe exec's TP reset before any libc or loader initialization. */
#include <asm/tls.h>
#include <asm/unistd.h>

	.text
	.global _start
_start:
	xld.w %r4,C33_TLS_SLOT_ADDRESS
	ld.w %r4,[%r4]
	cmp %r4,0
	jrne fail
	xld.w %r4,__NR_c33_get_tls
	int 0
	cmp %r4,0
	jrne fail
	ld.w %r6,0
	jp exit
fail:
	ld.w %r6,1
exit:
	xld.w %r4,__NR_exit
	int 0
	.section .note.GNU-stack,"",@progbits
