/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_THREAD_INFO_H
#define _ASM_C33_THREAD_INFO_H

#include <asm/page.h>

#define THREAD_SIZE_ORDER 1
#define THREAD_SIZE (PAGE_SIZE << THREAD_SIZE_ORDER)

#ifndef __ASSEMBLER__
#include <linux/types.h>
struct task_struct;
struct thread_info {
	unsigned long flags;
	int preempt_count;
	__u32 cpu;
};

#define INIT_THREAD_INFO(tsk) { \
	.preempt_count = INIT_PREEMPT_COUNT, \
}

#endif

#define TIF_SYSCALL_TRACE 0
#define TIF_NOTIFY_RESUME 1
#define TIF_SIGPENDING 2
#define TIF_NEED_RESCHED 3
#define TIF_MEMDIE 4
#define TIF_SECCOMP 5
#define TIF_SYSCALL_AUDIT 6
#define TIF_NOTIFY_SIGNAL 7
#define TIF_SINGLESTEP 8
#define TIF_RESTORE_SIGMASK 9
#define TIF_SYSCALL_TRACEPOINT 10

#define _TIF_SYSCALL_TRACE (1 << TIF_SYSCALL_TRACE)
#define _TIF_NOTIFY_SIGNAL (1 << TIF_NOTIFY_SIGNAL)
#define _TIF_NOTIFY_RESUME (1 << TIF_NOTIFY_RESUME)
#define _TIF_SIGPENDING (1 << TIF_SIGPENDING)
#define _TIF_NEED_RESCHED (1 << TIF_NEED_RESCHED)
#define _TIF_SECCOMP (1 << TIF_SECCOMP)
#define _TIF_MEMDIE (1 << TIF_MEMDIE)
#define _TIF_SYSCALL_AUDIT (1 << TIF_SYSCALL_AUDIT)
#define _TIF_SINGLESTEP (1 << TIF_SINGLESTEP)
#define _TIF_RESTORE_SIGMASK (1 << TIF_RESTORE_SIGMASK)
#define _TIF_SYSCALL_TRACEPOINT (1 << TIF_SYSCALL_TRACEPOINT)
#define _TIF_WORK_MASK (_TIF_NOTIFY_SIGNAL | _TIF_NOTIFY_RESUME | \
			_TIF_SIGPENDING | _TIF_NEED_RESCHED)
#endif
