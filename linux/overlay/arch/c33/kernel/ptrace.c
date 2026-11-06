// SPDX-License-Identifier: GPL-2.0
#include <linux/elf.h>
#include <linux/ptrace.h>
#include <linux/regset.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/uaccess.h>

#define C33_SAME_WORD(field)						\
	static_assert(offsetof(struct pt_regs, field) ==		\
		      offsetof(struct user_pt_regs, field))
C33_SAME_WORD(r);
C33_SAME_WORD(alr);
C33_SAME_WORD(ahr);
C33_SAME_WORD(sp);
C33_SAME_WORD(vector);
C33_SAME_WORD(orig_r4);
C33_SAME_WORD(psr);
C33_SAME_WORD(pc);
static_assert(offsetof(struct pt_regs, from_user) ==
	      offsetof(struct user_pt_regs, reserved));
static_assert(sizeof(struct pt_regs) == sizeof(struct user_pt_regs));
static_assert(sizeof(struct user_pt_regs) == sizeof(elf_gregset_t));

static int c33_gpr_get(struct task_struct *target,
		       const struct user_regset *regset, struct membuf to)
{
	return membuf_write(&to, task_pt_regs(target),
			    sizeof(struct user_pt_regs));
}

/*
 * The frame's vector and from_user words are the entry path's own: with
 * from_user cleared, the exit path would return the tracee to userspace as
 * a kernel frame.  A tracer's values for them are dropped, so that the
 * whole set read can be written back, and the PSR is limited as sigreturn
 * limits it.
 */
static int c33_gpr_set(struct task_struct *target,
		       const struct user_regset *regset, unsigned int pos,
		       unsigned int count, const void *kbuf,
		       const void __user *ubuf)
{
	struct pt_regs *regs = task_pt_regs(target);
	struct user_pt_regs new;
	int ret;

	memcpy(&new, regs, sizeof(new));
	ret = user_regset_copyin(&pos, &count, &kbuf, &ubuf, &new, 0,
				 sizeof(new));
	if (ret)
		return ret;

	memcpy(regs->r, new.r, sizeof(regs->r));
	regs->alr = new.alr;
	regs->ahr = new.ahr;
	regs->sp = new.sp;
	regs->orig_r4 = new.orig_r4;
	regs->psr = c33_user_psr(new.psr);
	regs->pc = new.pc;
	return 0;
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
	unsigned long tls;

	switch (request) {
	case PTRACE_GET_THREAD_AREA:
		if (addr != 0)
			return -EINVAL;
		return put_user(child->thread.tls, (unsigned long __user *)data);
	case PTRACE_SET_THREAD_AREA:
		if (addr != 0)
			return -EINVAL;
		if (get_user(tls, (unsigned long __user *)data))
			return -EFAULT;
		child->thread.tls = tls;
		return 0;
	}
	return ptrace_request(child, request, addr, data);
}
