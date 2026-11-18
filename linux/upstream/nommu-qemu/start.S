/* Entry point and vfork-style spawn for the freestanding reproducers. */

	.text
	.globl	_start
_start:
	mv	a0, sp			/* argc, argv..., envp... */
	andi	sp, sp, -16
	call	start_main
1:	li	a7, 93			/* exit */
	ecall
	j	1b

/*
 * long spawn(int (*fn)(void), void *stack_top)
 *
 * clone(CLONE_VM | CLONE_VFORK | SIGCHLD) onto a separate stack. The child
 * runs fn() there and exits with fn()'s return value; the parent resumes after the
 * child exits or execs, with the child's pid or an errno.
 */
	.globl	spawn
spawn:
	mv	t0, a0
	li	a0, 0x4111
	li	a2, 0
	li	a3, 0
	li	a4, 0
	li	a7, 220			/* clone */
	ecall
	bnez	a0, 2f
	jalr	t0
1:	li	a7, 93			/* exit */
	ecall
	j	1b
2:	ret
