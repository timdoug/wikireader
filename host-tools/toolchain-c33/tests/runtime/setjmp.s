/* setjmp/longjmp for the C33 DejaGnu board.
 *
 * Nothing in the firmware has an equivalent, so unlike the allocator this
 * is written for the occasion.  It only has to preserve what the C33 ABI
 * says a callee preserves: %r0-%r3, %sp, and the return address.
 *
 * There is no link register.  call pushes the return address (core manual
 * 2.4.4) and ret pops it, so on entry to setjmp the word at [%sp+0] *is*
 * the return address, and %sp itself is the caller's %sp minus 4.  Saving
 * both and restoring both is all longjmp needs: it writes the saved return
 * address back over its own and lets ret take it.
 *
 * jmp_buf layout, 6 words:
 *   0  %r0   4  %r1   8  %r2   12  %r3   16  %sp   20  return address
 */

	.section .text,"ax"

	.global setjmp
	.global _setjmp
setjmp:
_setjmp:
	xld.w	[%r6+0], %r0
	xld.w	[%r6+4], %r1
	xld.w	[%r6+8], %r2
	xld.w	[%r6+12], %r3
	ld.w	%r4, %sp
	xld.w	[%r6+16], %r4		/* %sp as it is now: points at our
					   own return address */
	ld.w	%r4, [%sp+0]
	xld.w	[%r6+20], %r4		/* where our caller wants to resume */
	ld.w	%r4, 0			/* the direct call returns 0 */
	ret

	.global longjmp
	.global _longjmp
longjmp:				/* %r6 = jmp_buf, %r7 = value */
_longjmp:
	xld.w	%r0, [%r6+0]
	xld.w	%r1, [%r6+4]
	xld.w	%r2, [%r6+8]
	xld.w	%r3, [%r6+12]
	xld.w	%r4, [%r6+20]		/* setjmp's return address ... */
	xld.w	%r5, [%r6+16]
	ld.w	%sp, %r5		/* ... and setjmp's stack */
	ld.w	[%sp+0], %r4		/* ret will pop this */

	/* longjmp(buf, 0) must still make setjmp return 1.  */
	ld.w	%r4, %r7
	cmp	%r4, 0
	jrne	1f
	ld.w	%r4, 1
1:
	ret
