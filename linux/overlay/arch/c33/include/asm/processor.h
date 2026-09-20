/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_PROCESSOR_H
#define _ASM_C33_PROCESSOR_H

#include <asm/ptrace.h>
#include <asm/page.h>

#define TASK_SIZE 0xffffffffUL
#define TASK_UNMAPPED_BASE 0
#define STACK_TOP TASK_SIZE
#define STACK_TOP_MAX STACK_TOP

struct thread_struct {
	unsigned long ksp;
	unsigned long usp;
	struct pt_regs *regs;
};

#define INIT_THREAD { .ksp = 0, .usp = 0, .regs = NULL }

struct task_struct;
extern void start_thread(struct pt_regs *regs, unsigned long pc,
			 unsigned long sp);
extern unsigned long __get_wchan(struct task_struct *p);

#define task_pt_regs(task) ((struct pt_regs *)(THREAD_SIZE + task_stack_page(task)) - 1)
#define KSTK_EIP(task) ((task)->thread.regs ? (task)->thread.regs->pc : 0)
#define KSTK_ESP(task) ((task)->thread.usp)
#define cpu_relax() barrier()
#endif
