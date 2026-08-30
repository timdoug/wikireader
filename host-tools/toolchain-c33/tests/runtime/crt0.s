/* Startup and exit path for a testsuite executable under the emulator.
 *
 * Pass/fail comes out of the emulator's register dump: run to the exit
 * point with %r4 == 0 and the test passed.  %r4 == 0xdead means abort().
 *
 * The exit point is a spin, not a HALT.  HALT sets "sleeping", not
 * "halted" -- grifo's suspend path waits in HALT for the touch controller,
 * so the emulator fast-forwards through it rather than stopping.  A
 * breakpoint is the reliable stop, and .text.exit is linked first so that
 * breakpoint is always 0x10000002 and the driver never needs to look a
 * symbol up.
 *
 * Note %r6 and %r4: the first argument arrives in %r6 (ARG0_REGNUM) while
 * %r4 is the return register (RV_REGNUM), so exit's status and main's
 * return value reach here in different places.  Everything funnels through
 * %r6 and exit copies it, or a test that ends in exit(0) reports whatever
 * %r4 happened to hold.
 *
 * Nothing here touches hardware: no clocks, no interrupts, no card.  A
 * test that faults or runs away is reported as that rather than hanging.
 */

	.section .text.c33_test_exit,"ax"
	.global exit
	.global _exit
/* mini-libc declares exit() as __asm__("__stop_progExec__") -- see
   mini-libc/include/stdlib.h -- so every test that includes <stdlib.h>
   links against that name rather than "exit".  It was the single biggest
   reason tests could not be built here.  */
	.global __stop_progExec__
exit:
_exit:
__stop_progExec__:
	ld.w	%r4, %r6		/* status arrives in %r6; report in %r4 */
	.global _exit_done
_exit_done:
	jp	_exit_done		/* driver breakpoints here: 0x10000002 */

	.section .text.start,"ax"
	.global _start
_start:
	xld.w	%r15, __stack_top
	ld.w	%sp, %r15
	xld.w	%r15, __dp
	ld.w	%r4, 0
	ld.w	%psr, %r4		/* interrupts off */

	xcall	_runtime_init		/* hands the heap to grifo's allocator */

	ld.w	%r6, 0			/* argc */
	ld.w	%r7, 0			/* argv */
	xcall	main

	ld.w	%r6, %r4		/* run destructors and preserve status */
	xcall	_runtime_fini
	ld.w	%r6, %r4		/* exit wants the status in %r6 */
	xjp	exit

/* abort() -- a distinctive status, so a failing test is never confused
   with one that merely returned non-zero. */
	.global abort
abort:
	xld.w	%r6, 0xdead
	xjp	exit
