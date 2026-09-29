/* SPDX-License-Identifier: GPL-2.0 */
/*
 * A static FDPIC program with no C library.  The kernel enters with the
 * load map in %r6 and nothing relocated; as crt1 does, relocate the words
 * .rofixup lists (crtreloc.o) and take __dp, relocated, for %r15.
 */
	.text
	.global _start
	.type _start,@function
_start:
	xld.w %r7,__ROFIXUP_LIST__
	xld.w %r8,__ROFIXUP_END__
	xcall __self_reloc
	ld.w %r15,%r4
	xcall root_main
.Lhang:
	jp .Lhang
	.size _start,.-_start

	.text
/*
 * long c33_syscall(long a1, long a2, long a3, long a4, long a5, long nr)
 *
 * The first four arguments are already in %r6-%r9, where the kernel wants
 * them; the fifth goes to %r10 and the number to %r4.  CALL pushed the
 * return address, so the stacked arguments start at [%sp+4].
 */
	.global c33_syscall
	.type c33_syscall,@function
c33_syscall:
	xld.w %r10,[%sp+4]
	xld.w %r4,[%sp+8]
	int 0
	ret
	.size c33_syscall,.-c33_syscall
