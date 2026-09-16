/* start.S - entry for the bare-metal rv32ima benchmark image.
 *
 * The machine arrives here in M-mode with nothing set up, exactly as the
 * interpreter's reset leaves it: a0 is the hart id and a1 the device tree,
 * both ignored here.
 */

	.section .text.start, "ax"
	.globl _start
_start:
	.option push
	.option norelax
	la	gp, __global_pointer$
	.option pop
	la	sp, __stack_top

	/* Clear .bss; the host loads only the file image. */
	la	t0, __bss_start
	la	t1, __bss_end
1:	bgeu	t0, t1, 2f
	sw	zero, 0(t0)
	addi	t0, t0, 4
	j	1b
2:
	/* A trap here is a bug in the benchmark or the interpreter; park in a
	   loop the host can see as a hang rather than silently looping. */
	la	t0, trap_park
	csrw	mtvec, t0

	call	main

	/* main returned: ask the machine to power off. */
	li	t0, 0x11100000
	li	t1, 0x5555
	sw	t1, 0(t0)
3:	j	3b

	.align 2
trap_park:
	li	t0, 0x10000000
	li	t1, '!'
	sw	t1, 0(t0)
4:	j	4b
