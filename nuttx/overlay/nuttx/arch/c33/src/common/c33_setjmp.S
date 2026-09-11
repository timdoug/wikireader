/****************************************************************************
 * arch/c33/src/common/c33_setjmp.S
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/* Saving and resuming a call context.  The C33 has no architectural help
 * here: CALL pushes the return address on the stack, so the continuation is
 * read back from [%sp] and the saved stack pointer is the caller's, which is
 * %sp plus that one word.
 *
 * The register set is the ABI's callee-saved four.  Anything the caller
 * wanted kept across the call that setjmp() returns from a second time is,
 * by the same convention, already in these or on the stack.
 */

	.text

/****************************************************************************
 * Name: setjmp
 *
 * Description:
 *   Save the current call context in env and return 0.
 *
 * Input Parameters:
 *   env - %r6, the jmp_buf to save into
 *
 * Returned Value:
 *   0 when called directly, and whatever longjmp() was given when resumed.
 *
 ****************************************************************************/

	.global	setjmp
	.type	setjmp,@function
setjmp:
	xld.w	[%r6+0],%r0
	xld.w	[%r6+4],%r1
	xld.w	[%r6+8],%r2
	xld.w	[%r6+12],%r3
	/* The caller's %sp is this frame's, past the pushed return address. */
	ld.w	%r4,%sp
	xadd	%r4,4
	xld.w	[%r6+16],%r4
	xld.w	%r4,[%sp+0]
	xld.w	[%r6+20],%r4
	ld.w	%r4,0
	ret
	.size	setjmp,.-setjmp

/****************************************************************************
 * Name: longjmp
 *
 * Description:
 *   Resume the context saved in env, making setjmp() return val.
 *
 * Input Parameters:
 *   env - %r6, the jmp_buf to resume
 *   val - %r7, setjmp()'s second return value; 0 is turned into 1, as
 *         POSIX requires, since 0 means "returned directly"
 *
 * Returned Value:
 *   Does not return.
 *
 ****************************************************************************/

	.global	longjmp
	.type	longjmp,@function
longjmp:
	ld.w	%r4,%r7
	cmp	%r4,0
	jrne	1f
	ld.w	%r4,1
1:
	xld.w	%r0,[%r6+0]
	xld.w	%r1,[%r6+4]
	xld.w	%r2,[%r6+8]
	xld.w	%r3,[%r6+12]
	xld.w	%r7,[%r6+16]
	ld.w	%sp,%r7
	xld.w	%r7,[%r6+20]
	/* %r4 holds the value setjmp() is to return; jump past its "ld.w
	 * %r4,0" by resuming at the saved return address directly.
	 */
	jp	%r7
	.size	longjmp,.-longjmp
