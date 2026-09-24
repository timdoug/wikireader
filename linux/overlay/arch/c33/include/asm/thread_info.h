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
	unsigned long syscall_work;
	int preempt_count;
	__u32 cpu;
};

#define INIT_THREAD_INFO(tsk) { \
	.preempt_count = INIT_PREEMPT_COUNT, \
}

#endif

/*
 * Syscall tracing, seccomp, and audit are SYSCALL_WORK_* bits owned by the
 * generic entry code, so only the flags its exit loop looks at live here.
 */
#define TIF_NOTIFY_RESUME 0
#define TIF_SIGPENDING 1
#define TIF_NEED_RESCHED 2
#define TIF_NOTIFY_SIGNAL 3
#define TIF_MEMDIE 4
#define TIF_RESTORE_SIGMASK 5

#define _TIF_NOTIFY_SIGNAL (1 << TIF_NOTIFY_SIGNAL)
#define _TIF_NOTIFY_RESUME (1 << TIF_NOTIFY_RESUME)
#define _TIF_SIGPENDING (1 << TIF_SIGPENDING)
#define _TIF_NEED_RESCHED (1 << TIF_NEED_RESCHED)
#define _TIF_MEMDIE (1 << TIF_MEMDIE)
#define _TIF_RESTORE_SIGMASK (1 << TIF_RESTORE_SIGMASK)

/* No uprobes here, and the generic exit loop wants the flag to exist. */
#define _TIF_UPROBE 0
#endif
