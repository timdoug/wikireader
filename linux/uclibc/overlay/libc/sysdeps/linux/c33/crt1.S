/* Startup code for the Epson S1C33.  */
#include <features.h>

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
	xld.w %r4,_fini
	xld.w [%sp],%r4
	ld.w %r4,0
	xld.w [%sp+4],%r4
	xld.w [%sp+8],%r0

	/* main, argc, argv, and app_init are passed in r6 through r9. */
	xld.w %r6,main
	xld.w %r7,[%r0]
	ld.w %r8,%r0
	add %r8,1
	xld.w %r9,_init
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
	.weak data_start
	.set data_start,__data_start
