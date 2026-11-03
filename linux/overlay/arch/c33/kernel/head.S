/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/linkage.h>
#include <linux/init.h>
#include <asm/iram.h>
#include <asm/thread_info.h>
#include <uapi/asm/tls.h>

/* Grifo's picture: 240 pixels in 8 words a line, 208 lines. */
#define C33_BOOT_PICTURE_WORDS	(8 * 208)

/* Copy whole words from \from to \to up to \end, which is past \to. */
.macro	copy_words from, to, end, t
	cmp	\to,\end
	jruge	.Lcopied\@
.Lcopy\@:
	ld.w	\t,[\from]+
	ld.w	[\to]+,\t
	cmp	\to,\end
	jrult	.Lcopy\@
.Lcopied\@:
.endm

	.section .head.text,"ax"
	.global _start
	.type _start,@function
_start:
	psrclr 4
	xld.w %r4,init_stack + THREAD_SIZE
	ld.w %sp,%r4
	xld.w %r15,__dp
	ld.w %r4,0
	xld.w %r5,C33_TLS_SLOT_ADDRESS
	ld.w [%r5],%r4
	ld.w %psr,%r4

	/*
	 * A0 RAM (asm/iram.h).  Grifo leaves the panel scanning its picture
	 * from IVRAM: note where, for the return to it, and if that is IVRAM
	 * move the picture to SDRAM, until the display driver takes it over.
	 * %r6 and %r7 hold Grifo's argc and argv for c33_start.
	 */
	xld.w	%r4,C33_LCDC_MADD
	ld.w	%r5,[%r4]
	xld.w	%r8,c33_boot_madd
	ld.w	[%r8],%r5
	xld.w	%r8,C33_LCDC_MLADD
	ld.w	%r8,[%r8]
	xld.w	%r9,c33_boot_mladd
	ld.w	[%r9],%r8
	xld.w	%r8,C33_IVRAM_BASE
	ld.w	%r9,%r5
	sub	%r9,%r8
	xld.w	%r8,C33_IVRAM_SIZE
	cmp	%r9,%r8
	jruge	.Lpicture_moved
	xld.w	%r8,c33_boot_picture
	xld.w	%r9,c33_boot_picture + 4 * C33_BOOT_PICTURE_WORDS
	copy_words %r5, %r8, %r9, %r10
	xld.w	%r8,c33_boot_picture
	ld.w	[%r4],%r8
.Lpicture_moved:
	/* Then IVRAM joins area 0, and the hot sections go into place. */
	xld.w	%r4,C33_LCDC_IRAM
	ld.w	%r5,[%r4]
	or	%r5,1
	ld.w	[%r4],%r5
	xld.w	%r4,__a0_text_load
	xld.w	%r5,__a0_text_start
	xld.w	%r8,__a0_text_end
	copy_words %r4, %r5, %r8, %r9
	xld.w	%r4,__a0_data_load
	xld.w	%r5,__a0_data_start
	xld.w	%r8,__a0_data_end
	copy_words %r4, %r5, %r8, %r9

	xcall c33_start
1:
	halt
	jp 1b
	.size _start,.-_start

	.data
	.balign	4
SYM_DATA(c33_boot_madd, .long 0)
SYM_DATA(c33_boot_mladd, .long 0)

	__INITDATA
	.balign	4
SYM_DATA_START(c33_boot_picture)
	.space	4 * C33_BOOT_PICTURE_WORDS
SYM_DATA_END(c33_boot_picture)
