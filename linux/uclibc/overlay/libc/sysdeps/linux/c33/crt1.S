/* Startup code for the Epson S1C33.  */
#include <features.h>

/*
 * -msep-data: this text is shared by every process running the program, so
 * it holds no absolute address.  binfmt_flat has pointed %r15 at this
 * process's data segment (__dp); the three function addresses are words
 * there, which the loader relocates per process.
 */
#ifndef __C33_SEP_DATA__
#error "crt1.S is -msep-data startup code"
#endif

	.text
	.align 1
	.global _start
	.type _start,@function
	.type main,@function
	.type __uClibc_main,@function
	.type _init,@function
	.type _fini,@function
#ifndef __UCLIBC_CTOR_DTOR__
	.weak _init
	.weak _fini
#endif

_start:
	/* Preserve the kernel-provided initial stack pointer. */
	ld.w %r0,%sp

	/* Arguments five through seven occupy the caller's stack area. */
	sub %sp,3
	ext doff_hi(.Lfini)
	ext doff_lo(.Lfini)
	ld.w %r4,[%r15]
	xld.w [%sp],%r4
	ld.w %r4,0
	xld.w [%sp+4],%r4
	xld.w [%sp+8],%r0

	/* main, argc, argv, and app_init are passed in r6 through r9. */
	ext doff_hi(.Lmain)
	ext doff_lo(.Lmain)
	ld.w %r6,[%r15]
	xld.w %r7,[%r0]
	/* The C33 port selects BINFMT_FLAT_ARGVP_ENVP_ON_STACK. */
	xld.w %r8,[%r0+4]
	ext doff_hi(.Linit)
	ext doff_lo(.Linit)
	ld.w %r9,[%r15]
	xcall __uClibc_main

	/* __uClibc_main does not return. */
	xcall abort
1:
	jp 1b

	.size _start,.-_start

	.data
	.global __data_start
__data_start:
	.4byte 0
	.align 2
.Lmain:
	.4byte main
.Linit:
	.4byte _init
.Lfini:
	.4byte _fini
	.weak data_start
	.set data_start,__data_start
