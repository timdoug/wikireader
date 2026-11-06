// SPDX-License-Identifier: GPL-2.0
#include <linux/kallsyms.h>
#include <linux/kernel.h>
#include <linux/panic.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/sched/task_stack.h>

#include <asm/ptrace.h>
#include <asm/sections.h>
#include <asm/traps.h>

/* /proc/sys/debug/exception-trace: report programs' unhandled faults. */
int show_unhandled_signals = 1;

static const char *const c33_vector_names[16] = {
	[0] = "reset",
	[2] = "ext exception",
	[3] = "undefined instruction",
	[6] = "misaligned access",
	[7] = "NMI",
	[11] = "illegal interrupt",
	[12] = "int 0",
	[13] = "int 1",
	[14] = "int 2",
	[15] = "int 3",
};

const char *c33_vector_name(unsigned int vector)
{
	if (vector < ARRAY_SIZE(c33_vector_names) && c33_vector_names[vector])
		return c33_vector_names[vector];
	return "reserved";
}

/* The kernel's text, including what head.S copied into A0 RAM. */
static bool c33_kernel_text(unsigned long addr)
{
	return __kernel_text_address(addr) ||
	       (addr >= (unsigned long)__a0_text_start &&
		addr < (unsigned long)__a0_text_end);
}

void c33_show_regs(const struct pt_regs *regs, const char *loglvl)
{
	int i;

	printk("%sPC: %08lx %pS\n", loglvl, regs->pc, (void *)regs->pc);
	printk("%sSP: %08lx PSR: %08lx vector: %lu (%s) %s mode\n", loglvl,
	       regs->sp, regs->psr, regs->vector,
	       c33_vector_name(regs->vector),
	       user_mode(regs) ? "user" : "kernel");
	for (i = 0; i < 16; i += 4)
		printk("%sr%-2d %08lx %08lx %08lx %08lx\n", loglvl, i,
		       regs->r[i], regs->r[i + 1], regs->r[i + 2],
		       regs->r[i + 3]);
	printk("%salr %08lx ahr %08lx\n", loglvl, regs->alr, regs->ahr);
}

void show_regs(struct pt_regs *regs)
{
	show_regs_print_info(KERN_DEFAULT);
	c33_show_regs(regs, KERN_DEFAULT);
	if (!user_mode(regs))
		show_stack(NULL, (unsigned long *)regs->sp, KERN_DEFAULT);
}

/*
 * There is no unwinder: every word on the stack that is an address in the
 * kernel's text is listed, as m68k and nios2 do.  Return addresses are
 * among them, and so are stale words from calls that have returned.
 */
void show_stack(struct task_struct *task, unsigned long *sp,
		const char *loglvl)
{
	unsigned long base, *end;

	if (!task)
		task = current;
	if (!sp)
		sp = task == current ? (unsigned long *)current_stack_pointer :
				       (unsigned long *)task->thread.ksp;

	base = (unsigned long)task_stack_page(task);
	if ((unsigned long)sp >= base && (unsigned long)sp < base + THREAD_SIZE)
		end = (unsigned long *)(base + THREAD_SIZE);
	else
		end = (unsigned long *)ALIGN((unsigned long)sp, THREAD_SIZE);

	printk("%sCall Trace:\n", loglvl);
	for (; sp < end; sp++)
		if (c33_kernel_text(*sp))
			printk("%s [<%08lx>] %pS\n", loglvl, *sp, (void *)*sp);
}

/*
 * An exception in the kernel's own code.  Without an MMU nothing bounds
 * what the faulting code may already have overwritten, so the report is
 * followed by a panic rather than by killing the task.
 */
void __noreturn c33_die(unsigned int vector, struct pt_regs *regs)
{
	console_verbose();
	bust_spinlocks(1);
	pr_emerg("C33 exception %u (%s) in the kernel\n", vector,
		 c33_vector_name(vector));
	show_regs(regs);
	bust_spinlocks(0);
	/* The trace above is the fault's; TAINT_DIE keeps panic() from adding one. */
	add_taint(TAINT_DIE, LOCKDEP_NOW_UNRELIABLE);
	panic("Fatal C33 exception");
}
