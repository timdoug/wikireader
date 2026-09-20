/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/linkage.h>
#include <generated/asm-offsets.h>

SYM_FUNC_START(__c33_switch_to)
	pushn %r3
	ld.w %r10,%sp
	xld.w [%r6+TASK_THREAD+THREAD_KSP],%r10
	ld.w %r4,%r6
	xld.w %r10,c33_current_task
	xld.w [%r10],%r7
	xld.w %r10,[%r7+TASK_THREAD+THREAD_KSP]
	ld.w %sp,%r10
	popn %r3
	ret
SYM_FUNC_END(__c33_switch_to)

SYM_FUNC_START(ret_from_kernel_thread)
	ld.w %r6,%r4
	xcall schedule_tail
	ld.w %r4,[%sp]
	xld.w %r6,[%sp+4]
	call %r4
	ld.w %r6,%sp
	xjp c33_restore
SYM_FUNC_END(ret_from_kernel_thread)

SYM_FUNC_START(ret_from_fork)
	ld.w %r6,%r4
	xcall schedule_tail
	ld.w %r6,%sp
	xjp c33_restore
SYM_FUNC_END(ret_from_fork)
