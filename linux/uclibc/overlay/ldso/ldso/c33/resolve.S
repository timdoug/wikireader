/* C33 FDPIC lazy binding.
 *
 * Licensed under the LGPL v2.1, see the file COPYING.LIB in this tarball.
 *
 * A module's .plt head jumps here with ld.so's %r15, the entry's offset in
 * the module's .rela.plt in %r13, the return address of the stub that
 * called the head at [%sp] -- it names the module -- and the caller's own
 * return address at [%sp+4].  The caller's arguments are in %r5-%r12 and
 * on the stack above that.  _dl_linux_resolver fills in the descriptor and
 * returns it; the call then goes on as the .plt entry would have made it.
 */

	.text
	.align	1
	.global	_dl_linux_resolve
	.hidden	_dl_linux_resolve
	.type	_dl_linux_resolve,@function
_dl_linux_resolve:
	pop	%r4
	/* Eleven words keep the call below on a 16-byte boundary.  */
	sub	%sp,11
	xld.w	[%sp],%r5
	xld.w	[%sp+4],%r6
	xld.w	[%sp+8],%r7
	xld.w	[%sp+12],%r8
	xld.w	[%sp+16],%r9
	xld.w	[%sp+20],%r10
	xld.w	[%sp+24],%r11
	xld.w	[%sp+28],%r12
	ld.w	%r6,%r4
	ld.w	%r7,%r13
	xcall	_dl_linux_resolver
	ld.w	%r13,%r4
	xld.w	%r5,[%sp]
	xld.w	%r6,[%sp+4]
	xld.w	%r7,[%sp+8]
	xld.w	%r8,[%sp+12]
	xld.w	%r9,[%sp+16]
	xld.w	%r10,[%sp+20]
	xld.w	%r11,[%sp+24]
	xld.w	%r12,[%sp+28]
	add	%sp,11
	ld.w	%r14,[%r13]
	xld.w	%r15,[%r13+4]
	jp	%r14
	.size	_dl_linux_resolve,.-_dl_linux_resolve
