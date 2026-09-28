/* Startup code for C33 FDPIC executables.
 *
 * The kernel, or ld.so for a dynamic executable, enters _start with the
 * executable's load map in %r6, the interpreter's in %r7 (zero for a static
 * executable), and in %r9 the descriptor of ld.so's _dl_fini, or zero.
 * [%sp] holds argc, then argv and the environment.  Nothing in this
 * module has been relocated and %r15 is not set: __self_reloc (crtreloc.c)
 * relocates the words listed in .rofixup and returns our __dp, relocated,
 * which is %r15.  It is handed the list's link-time bounds, which it maps
 * through the load map: the text has no PC-relative way to reach them.
 *
 * Then __uClibc_main (main, argc, argv, _init, _fini, rtld_fini,
 * stack_end), with descriptors for the functions.
 */
#include <features.h>

#ifndef __FDPIC__
#error "crt1.S is FDPIC startup code"
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
	ld.w %r0,%sp		/* argc, and the stack's end */
	ld.w %r1,%r9		/* rtld_fini */
	xld.w %r7,__ROFIXUP_LIST__
	xld.w %r8,__ROFIXUP_END__
	xcall __self_reloc
	ld.w %r15,%r4

	/* Arguments five to seven go in the caller's stack area, 16-byte
	   aligned.  */
	ld.w %r4,%r0
	and %r4,-16
	sub %r4,16
	ld.w %sp,%r4
	ext doff_hi(.Lfini)
	ext doff_lo(.Lfini)
	ld.w %r4,[%r15]
	xld.w [%sp],%r4
	xld.w [%sp+4],%r1
	xld.w [%sp+8],%r0

	ext doff_hi(.Lmain)
	ext doff_lo(.Lmain)
	ld.w %r6,[%r15]
	xld.w %r7,[%r0]
	ld.w %r8,%r0
	add %r8,4
	ext doff_hi(.Linit)
	ext doff_lo(.Linit)
	ld.w %r9,[%r15]
	xcall __uClibc_main

	/* __uClibc_main does not return. */
1:
	jp 1b

	.size _start,.-_start

	.data
	.global __data_start
__data_start:
	.4byte 0
	.align 2
.Lmain:
	.long funcdesc(main)
.Linit:
	.long funcdesc(_init)
.Lfini:
	.long funcdesc(_fini)
	.weak data_start
	.set data_start,__data_start
