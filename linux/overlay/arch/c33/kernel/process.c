// SPDX-License-Identifier: GPL-2.0
#include <linux/elfcore.h>
#include <linux/kernel.h>
#include <linux/reboot.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/string.h>

#include <asm/processor.h>

struct task_struct *c33_current_task = &init_task;

asmlinkage struct pt_regs *c33_exception_enter(struct pt_regs *regs);
asmlinkage struct pt_regs *c33_exception_exit(struct pt_regs *regs);

asmlinkage struct pt_regs *c33_exception_enter(struct pt_regs *regs)
{
	struct pt_regs *kernel_regs;

	if (current->thread.in_kernel) {
		regs->reserved = 0;
		return regs;
	}

	kernel_regs = task_pt_regs(current);
	*kernel_regs = *regs;
	kernel_regs->reserved = 1;
	current->thread.in_kernel = 1;
	current->thread.regs = kernel_regs;
	return kernel_regs;
}

asmlinkage struct pt_regs *c33_exception_exit(struct pt_regs *regs)
{
	if (user_mode(regs))
		current->thread.in_kernel = 0;
	return regs;
}

void arch_cpu_idle(void);

void arch_cpu_idle(void)
{
	__asm__ volatile ("halt");
}

void machine_restart(char *command)
{
	for (;;)
		__asm__ volatile ("halt");
}

void machine_halt(void)
{
	for (;;)
		__asm__ volatile ("halt");
}

void machine_power_off(void)
{
	machine_halt();
}

void show_regs(struct pt_regs *regs)
{
	pr_info("PC: %08lx SP: %08lx PSR: %08lx vector: %lu\n",
		regs->pc, regs->sp, regs->psr, regs->vector);
}

void show_stack(struct task_struct *task, unsigned long *stack,
		const char *loglvl)
{
	unsigned long *end;
	int words = 0;

	if (!stack) {
		if (task && task != current)
			stack = (unsigned long *)task->thread.ksp;
		else
			stack = (unsigned long *)current_stack_pointer;
	}

	end = (unsigned long *)(((unsigned long)stack + THREAD_SIZE - 1) &
				 ~(THREAD_SIZE - 1));
	printk("%sStack:", loglvl);
	while (stack < end && words++ < 32)
		printk(" %08lx", *stack++);
	printk("\n");
}

void flush_thread(void)
{
}

void start_thread(struct pt_regs *regs, unsigned long pc, unsigned long sp)
{
	memset(regs, 0, sizeof(*regs));
	regs->pc = pc;
	regs->sp = sp;
	regs->psr = 1 << 4;
	regs->orig_r4 = -1;
	regs->reserved = 1;
	current->thread.usp = sp;
}

unsigned long __get_wchan(struct task_struct *p)
{
	return 0;
}

int copy_thread(struct task_struct *p, const struct kernel_clone_args *args)
{
	struct pt_regs *childregs = task_pt_regs(p);
	unsigned long *switch_sp = (unsigned long *)childregs - 5;
	extern void ret_from_fork(void);
	extern void ret_from_kernel_thread(void);

	memset(switch_sp, 0, 4 * sizeof(*switch_sp) + sizeof(*childregs));
	p->thread.regs = childregs;
	p->thread.ksp = (unsigned long)switch_sp;
	p->thread.in_kernel = 1;

	if (unlikely(args->fn)) {
		/* POPN restores r0-r3, then RET leaves SP on these two words. */
		switch_sp[4] = (unsigned long)ret_from_kernel_thread;
		childregs->r[0] = (unsigned long)args->fn;
		childregs->r[1] = (unsigned long)args->fn_arg;
		p->thread.usp = 0;
		return 0;
	}

	*childregs = *current_pt_regs();
	childregs->r[4] = 0;
	childregs->orig_r4 = -1;
	if (args->stack)
		childregs->sp = args->stack;
	p->thread.usp = childregs->sp;
	switch_sp[4] = (unsigned long)ret_from_fork;
	return 0;
}

int elf_core_copy_task_fpregs(struct task_struct *t, elf_fpregset_t *fpu)
{
	return 0;
}
