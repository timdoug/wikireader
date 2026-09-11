/*
 * A guest that asks the watchdog to reset it, for test-wdt-reset.
 *
 * This is what grifo's System_reboot() does, reduced to the registers that
 * matter: unlock the watchdog, give it a short timeout, enable it with the
 * reset output, and halt.  The chip restarts, so the emulator should too --
 * and should run this program again from its entry point rather than stopping
 * the way it did before resets were modelled.
 *
 * Each boot writes one byte to UART0's transmit register, which the emulator
 * prints, so the number of boots is visible in the output without the guest
 * having to keep any state across a reset.  It could not: reset clears RAM.
 *
 * Bare metal on purpose.  No grifo, no libc, no stack: just stores, so that
 * the test depends on nothing but the assembler.
 */

	.equ	UART0_TXD,	0x00300b00
	.equ	CMU_PROTECT,	0x00301b24
	.equ	CMU_GATEDCLK1,	0x00301b04
	.equ	CMU_UNLOCK,	0x96
	.equ	WDT_CKE,	0x200		/* GATEDCLK1 bit 9 */

	.equ	WD_WP,		0x00300660
	.equ	WD_EN,		0x00300662
	.equ	WD_COMP,	0x00300664
	.equ	WD_CNTL,	0x0030066c
	.equ	WP_UNLOCK,	0x96
	.equ	WDRESEN,	0x1		/* written to CNTL, clears count */
	.equ	RUNSTP,		0x10
	.equ	RESEN,		0x1

	/* Short enough that the test does not wait for grifo's twenty
	 * seconds, long enough to be an interval rather than an edge. */
	.equ	TIMEOUT,	20000

	.text
	.global	_start
_start:
	/* One byte per boot. */
	xld.w	%r4, UART0_TXD
	xld.w	%r5, 0x42		/* 'B' */
	ld.b	[%r4], %r5

	/* The counter only runs while its clock is ungated, and the gate is
	 * behind the CMU's write protection. */
	xld.w	%r4, CMU_PROTECT
	xld.w	%r5, CMU_UNLOCK
	ld.w	[%r4], %r5
	xld.w	%r4, CMU_GATEDCLK1
	ld.w	%r5, [%r4]
	xld.w	%r6, WDT_CKE
	or	%r5, %r6
	ld.w	[%r4], %r5
	xld.w	%r4, CMU_PROTECT
	xld.w	%r5, 0
	ld.w	[%r4], %r5

	/* Arm it: reset output, no NMI, counting from zero. */
	xld.w	%r4, WD_WP
	xld.w	%r5, WP_UNLOCK
	ld.h	[%r4], %r5
	xld.w	%r4, WD_COMP
	xld.w	%r5, TIMEOUT
	ld.w	[%r4], %r5
	xld.w	%r4, WD_CNTL
	xld.w	%r5, WDRESEN
	ld.h	[%r4], %r5
	xld.w	%r4, WD_EN
	xld.w	%r5, RUNSTP | RESEN
	ld.h	[%r4], %r5
	xld.w	%r4, WD_WP
	xld.w	%r5, 0
	ld.h	[%r4], %r5

	/* Wait for it, exactly as System_reboot() does. */
1:	halt
	jp	1b
