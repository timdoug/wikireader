// SPDX-License-Identifier: GPL-2.0
#include <linux/elf.h>
#include <linux/ptrace.h>
#include <linux/regset.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>

static int c33_gpr_get(struct task_struct *target,
		       const struct user_regset *regset, struct membuf to)
{
	return membuf_write(&to, task_pt_regs(target), sizeof(struct pt_regs));
}

static int c33_gpr_set(struct task_struct *target,
		       const struct user_regset *regset, unsigned int pos,
		       unsigned int count, const void *kbuf,
		       const void __user *ubuf)
{
	return user_regset_copyin(&pos, &count, &kbuf, &ubuf,
				  task_pt_regs(target), 0,
				  sizeof(struct pt_regs));
}

static const struct user_regset c33_regsets[] = {
	{
		USER_REGSET_NOTE_TYPE(PRSTATUS),
		.n = sizeof(struct pt_regs) / sizeof(unsigned long),
		.size = sizeof(unsigned long),
		.align = sizeof(unsigned long),
		.regset_get = c33_gpr_get,
		.set = c33_gpr_set,
	},
};

static const struct user_regset_view c33_user_view = {
	.name = "c33",
	.e_machine = ELF_ARCH,
	.ei_osabi = ELF_OSABI,
	.regsets = c33_regsets,
	.n = ARRAY_SIZE(c33_regsets),
};

const struct user_regset_view *task_user_regset_view(struct task_struct *task)
{
	return &c33_user_view;
}

void ptrace_disable(struct task_struct *child)
{
}

long arch_ptrace(struct task_struct *child, long request, unsigned long addr,
		 unsigned long data)
{
	return ptrace_request(child, request, addr, data);
}
