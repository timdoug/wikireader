/* m68k entry point and vfork-style spawn for the freestanding reproducers. */
#include <asm/unistd.h>

	.text
	.globl	_start
_start:
	/* self_relocate(load base, _DYNAMIC), both PC-relative */
	lea	_DYNAMIC(%pc), %a0
	move.l	%a0, -(%sp)
	lea	__ehdr_start(%pc), %a0
	move.l	%a0, -(%sp)
	lea	self_relocate(%pc), %a0
	jsr	(%a0)
	addq.l	#8, %sp
	move.l	%sp, %d0
	move.l	%d0, -(%sp)		/* start_main(argc, argv..., envp...) */
	lea	start_main(%pc), %a0
	jsr	(%a0)
1:	moveq	#__NR_exit, %d0
	moveq	#0, %d1
	trap	#0
	bra.s	1b

/*
 * long spawn(int (*fn)(void), void *stack_top)
 *
 * clone(CLONE_VM | CLONE_VFORK | SIGCHLD) onto a separate stack. The child
 * runs fn() there and exits with fn()'s return value; the parent resumes
 * after the child exits or execs, with the child's pid or an errno.
 */
	.globl	spawn
spawn:
	lea	-16(%sp), %sp
	movem.l	%d2-%d5, (%sp)
	move.l	20(%sp), %a1		/* fn */
	move.l	#0x4111, %d1		/* flags */
	move.l	24(%sp), %d2		/* stack_top */
	moveq	#0, %d3
	moveq	#0, %d4
	moveq	#0, %d5
	move.l	#__NR_clone, %d0
	trap	#0
	tst.l	%d0
	bne.s	2f
	jsr	(%a1)
	move.l	%d0, %d1
1:	moveq	#__NR_exit, %d0
	trap	#0
	bra.s	1b
2:	movem.l	(%sp), %d2-%d5
	lea	16(%sp), %sp
	rts
