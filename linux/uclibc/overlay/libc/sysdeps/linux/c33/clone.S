/* clone for C33's stack-based CALL/RET convention.
 *
 * int clone(int (*fn)(void *arg), void *child_stack, int flags, void *arg,
 *           pid_t *ptid, void *tls, pid_t *ctid);
 *
 * fn, child_stack, flags and arg arrive in %r6-%r9; ptid, tls and ctid on
 * the stack above the return address.  The kernel takes flags, newsp,
 * ptid, ctid and tls in %r6-%r10 (the asm-generic order, without
 * CLONE_BACKWARDS) and gives the child a copy of every register, so fn and
 * arg ride across in callee-saved %r0 and %r1, and the child finds its data
 * segment in the %r15 it inherits.
 */
#include <features.h>
#define _ERRNO_H
#include <bits/errno.h>
#include <sys/syscall.h>

	.text
	.align 1
	.global __clone
	.type __clone,@function
__clone:
	/* No NULL function or stack. */
	ld.w %r4,-EINVAL
	cmp %r6,0
	jreq .Lerror
	cmp %r7,0
	jreq .Lerror

	pushn %r1
	ld.w %r0,%r6
	ld.w %r1,%r9
	ld.w %r6,%r8
	/* After pushn the caller's stack arguments start at [%sp+12]. */
	xld.w %r8,[%sp+12]	/* ptid */
	xld.w %r10,[%sp+16]	/* tls */
	xld.w %r9,[%sp+20]	/* ctid */
	xld.w %r4,__NR_clone
	int 0
	cmp %r4,0
	jreq .Lchild
	popn %r1

	/* Linux returns -errno. */
	xld.w %r5,0xfffff000
	cmp %r4,%r5
	jrugt .Lerror
	ret

.Lerror:
	pushn %r0
	ld.w %r0,%r4
	not %r0,%r0
	add %r0,1
	xcall __errno_location
	ld.w [%r4],%r0
	popn %r0
	ld.w %r4,-1
	ret

.Lchild:
	/* On the new stack, with the parent's registers. */
	ld.w %r6,%r1
	call %r0
	ld.w %r6,%r4
	xld.w %r4,__NR_exit
	int 0
1:
	jp 1b
	.size __clone,.-__clone

	.weak clone
	.type clone,@function
	.set clone,__clone
