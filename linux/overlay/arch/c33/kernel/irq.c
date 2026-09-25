// SPDX-License-Identifier: GPL-2.0
#include <linux/entry-common.h>
#include <linux/interrupt.h>
#include <linux/hardirq.h>
#include <linux/irq-entry-common.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/s1c33-itc.h>
#include <linux/irqdesc.h>
#include <linux/io.h>
#include <linux/syscalls.h>

#include <asm/irq.h>
#include <asm/irq_regs.h>
#include <asm/ptrace.h>
#include <asm/syscalls.h>
#include <asm/wikireader.h>

#define C33_REG_BASE          0x00300000UL
#define C33_IRQ_ENABLE_FIRST  (C33_REG_BASE + 0x270)
#define C33_IRQ_FLAG_FIRST    (C33_REG_BASE + 0x280)
#define C33_SYSCALL_VECTOR    12

extern unsigned long c33_vector_table[];
extern void *const c33_sys_call_table[];
unsigned long c33_boot_ttbr;
int c33_grifo_booted;
asmlinkage struct pt_regs *c33_handle_irq(unsigned int vector,
					  struct pt_regs *regs);

typedef long (*c33_syscall_fn_t)(unsigned long, unsigned long,
				 unsigned long, unsigned long,
				 unsigned long, unsigned long);

void __init init_IRQ(void)
{
	int sources;

	/* Keep a resident Grifo's trap table for poweroff and reboot. */
	__asm__ volatile ("ld.w %0, %%ttbr" : "=r" (c33_boot_ttbr));
	c33_grifo_booted = c33_boot_ttbr == C33_GRIFO_TTBR;
	pr_info("C33 boot: %s (incoming TTBR %08lx)\n",
		c33_grifo_booted ? "Grifo application" : "standalone",
		c33_boot_ttbr);

	/* The controller quiets every cause before the vector table goes live. */
	sources = s1c33_itc_init();
	if (sources < 0)
		panic("C33 IRQ: interrupt controller failed: %d", sources);

	__asm__ volatile ("ld.w %%ttbr,%0" : : "r" (c33_vector_table)
			  : "memory");
	pr_info("C33 IRQ: registered %d interrupt sources\n", sources);
	c33_lcd_checkpoint(2);
}

asmlinkage struct pt_regs *c33_handle_irq(unsigned int vector,
					  struct pt_regs *regs)
{
	struct pt_regs *old_regs = set_irq_regs(regs);
	irqentry_state_t state;
	long nr;
	int i;

	if (vector == C33_SYSCALL_VECTOR) {
		struct pt_regs *old_task_regs = current->thread.regs;

		/* Generic clone and exec code must see this live syscall frame. */
		current->thread.regs = regs;
		regs->orig_r4 = regs->r[4];
		/*
		 * Tracing, seccomp, and audit get their say here, and may
		 * rewrite the number or ask for the call to be skipped.
		 */
		nr = syscall_enter_from_user_mode(regs, regs->r[4]);
		local_irq_enable();
		if (nr >= 0 && nr < NR_syscalls) {
			c33_syscall_fn_t fn =
				(c33_syscall_fn_t)c33_sys_call_table[nr];

			pr_info_once("C33: entered userspace syscall path\n");
			regs->r[4] = fn(regs->r[6], regs->r[7], regs->r[8],
					regs->r[9], regs->r[10], regs->r[11]);
		} else if (nr != -1L) {
			regs->r[4] = -ENOSYS;
		}
		syscall_exit_to_user_mode(regs);
		regs->orig_r4 = -1L;
		current->thread.regs = old_task_regs;
		set_irq_regs(old_regs);
		return regs;
	}

	/* Only a syscall frame carries a number; see arch_do_signal_or_restart(). */
	regs->orig_r4 = -1L;

	if (!s1c33_itc_is_source(vector)) {
		int reg;

		c33_lcd_fault(vector);
		pr_emerg("C33 exception %u: pc=%08lx sp=%08lx psr=%08lx\n",
			 vector, regs->pc, regs->sp, regs->psr);
		/* Which cause was actually asserted is the whole question. */
		for (reg = 0; reg < 16; reg += 8) {
			pr_emerg("itc flags %d: %8ph\n", reg,
				 (void *)(C33_IRQ_FLAG_FIRST + reg));
			pr_emerg("itc enable %d: %8ph\n", reg,
				 (void *)(C33_IRQ_ENABLE_FIRST + reg));
		}
		for (i = 0; i < 16; i += 4)
			pr_emerg("r%d=%08lx r%d=%08lx r%d=%08lx r%d=%08lx\n",
				i, regs->r[i], i + 1, regs->r[i + 1],
				i + 2, regs->r[i + 2], i + 3,
				regs->r[i + 3]);
		panic("unhandled C33 exception");
	}

	state = irqentry_enter(regs);
	irq_enter_rcu();
	s1c33_itc_handle(vector);
	irq_exit_rcu();
	irqentry_exit(regs, state);

	set_irq_regs(old_regs);
	return regs;
}
