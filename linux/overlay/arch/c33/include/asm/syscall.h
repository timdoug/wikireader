/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_SYSCALL_H
#define _ASM_C33_SYSCALL_H

#include <linux/err.h>
#include <linux/sched.h>
#include <uapi/linux/audit.h>

/* int 0 enters the kernel with the number in r4 and arguments in r6-r11. */
static inline int syscall_get_nr(struct task_struct *task,
				 struct pt_regs *regs)
{
	return regs->orig_r4;
}

static inline void syscall_set_nr(struct task_struct *task,
				  struct pt_regs *regs, int nr)
{
	regs->orig_r4 = nr;
}

static inline void syscall_rollback(struct task_struct *task,
				    struct pt_regs *regs)
{
	regs->r[4] = regs->orig_r4;
}

static inline long syscall_get_error(struct task_struct *task,
				     struct pt_regs *regs)
{
	return IS_ERR_VALUE(regs->r[4]) ? regs->r[4] : 0;
}

static inline long syscall_get_return_value(struct task_struct *task,
					    struct pt_regs *regs)
{
	return regs->r[4];
}

static inline void syscall_set_return_value(struct task_struct *task,
					    struct pt_regs *regs,
					    int error, long value)
{
	regs->r[4] = error ?: value;
}

static inline void syscall_get_arguments(struct task_struct *task,
					 struct pt_regs *regs,
					 unsigned long *args)
{
	memcpy(args, &regs->r[6], 6 * sizeof(args[0]));
}

static inline void syscall_set_arguments(struct task_struct *task,
					 struct pt_regs *regs,
					 const unsigned long *args)
{
	memcpy(&regs->r[6], args, 6 * sizeof(args[0]));
}

/* There is no vDSO, so no signal return can come from one. */
static inline bool arch_syscall_is_vdso_sigreturn(struct pt_regs *regs)
{
	return false;
}

static inline int syscall_get_arch(struct task_struct *task)
{
	return 107 | __AUDIT_ARCH_LE; /* EM_SE_C33 */
}

#endif
