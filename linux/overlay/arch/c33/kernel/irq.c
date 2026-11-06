// SPDX-License-Identifier: GPL-2.0
#include <linux/entry-common.h>
#include <linux/interrupt.h>
#include <linux/hardirq.h>
#include <linux/irq-entry-common.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdesc.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/sched/signal.h>
#include <linux/syscalls.h>

#include <asm/irq.h>
#include <asm/irq_regs.h>
#include <asm/ptrace.h>
#include <asm/syscall.h>
#include <asm/traps.h>
#include <asm/wikireader.h>

#define C33_SYSCALL_VECTOR    12	/* int 0; int N is vector 12 + N */
#define C33_BREAKPOINT_VECTOR 15	/* int 3 */
#define C33_FIRST_IRQ_VECTOR  16	/* the ITC's interrupts, to 255 */

unsigned long c33_boot_ttbr;
int c33_grifo_booted;
asmlinkage struct pt_regs *c33_handle_irq(unsigned int vector,
					  struct pt_regs *regs);

void __init init_IRQ(void)
{
	/* Keep a resident Grifo's trap table for poweroff and reboot. */
	__asm__ volatile ("ld.w %0, %%ttbr" : "=r" (c33_boot_ttbr));
	c33_grifo_booted = c33_boot_ttbr == C33_GRIFO_TTBR;
	pr_info("C33 boot: %s (incoming TTBR %08lx)\n",
		c33_grifo_booted ? "Grifo application" : "standalone",
		c33_boot_ttbr);

	/* The controller quiets every cause before the vector table goes live. */
	irqchip_init();

	__asm__ volatile ("ld.w %%ttbr,%0" : : "r" (c33_vector_table)
			  : "memory");
}

/* Which of the process's mappings holds ADDR, as file+offset.  */
static void c33_fault_where(const char *what, unsigned long addr)
{
	struct mm_struct *mm = current->mm;
	struct vm_area_struct *vma;

	if (!mm || !mmap_read_trylock(mm))
		return;
	vma = find_vma(mm, addr);
	if (vma && vma->vm_start <= addr && vma->vm_file)
		pr_info("  %s %08lx: %pD+%lx\n", what, addr, vma->vm_file,
			addr - vma->vm_start + (vma->vm_pgoff << PAGE_SHIFT));
	else if (vma && vma->vm_start <= addr)
		pr_info("  %s %08lx: anonymous %08lx-%08lx\n", what, addr,
			vma->vm_start, vma->vm_end);
	mmap_read_unlock(mm);
}

static void c33_user_fault(unsigned int vector, struct pt_regs *regs)
{
	void __user *pc = (void __user *)regs->pc;
	unsigned long ret;
	int sig, code;

	/*
	 * int 3 is the breakpoint debuggers write.  The trap leaves the PC
	 * after it; report the breakpoint's own address, as ARM does.
	 */
	if (vector == C33_BREAKPOINT_VECTOR) {
		regs->pc -= 2;
		force_sig_fault(SIGTRAP, TRAP_BRKPT, (void __user *)regs->pc);
		return;
	}

	/*
	 * The core's exceptions (S1C33E07 manual 6.3): a third ext prefix (2),
	 * an undefined instruction (3) and a misaligned halfword or word
	 * access (6).  In a program they are its own bug, and it gets the
	 * signal a larger machine would send.
	 */
	switch (vector) {
	case 2:
	case 3:
		sig = SIGILL;
		code = ILL_ILLOPC;
		break;
	case 6:
		sig = SIGBUS;
		code = BUS_ADRALN;
		break;
	default:
		sig = SIGSEGV;
		code = SEGV_MAPERR;
		break;
	}

	/* A fault the program handles itself is not reported. */
	if (show_unhandled_signals && unhandled_signal(current, sig) &&
	    printk_ratelimit()) {
		pr_info("%s[%d]: C33 exception %u (%s): unhandled signal %d code %d\n",
			current->comm, task_pid_nr(current), vector,
			c33_vector_name(vector), sig, code);
		c33_show_regs(regs, KERN_INFO);
		c33_fault_where("pc", regs->pc);
		c33_fault_where("r4", regs->r[4]);
		c33_fault_where("r15", regs->r[15]);
		if (!get_user(ret, (unsigned long __user *)regs->sp))
			c33_fault_where("[sp]", ret);
	}
	force_sig_fault(sig, code, pc);
}

asmlinkage struct pt_regs *c33_handle_irq(unsigned int vector,
					  struct pt_regs *regs)
{
	struct pt_regs *old_regs = set_irq_regs(regs);
	struct pt_regs *saved_task_regs = current->thread.regs;
	irqentry_state_t state;
	long nr;

	if (vector == C33_SYSCALL_VECTOR) {
		struct pt_regs *old_task_regs = current->thread.regs;

		/* Generic clone and exec code must see this live syscall frame. */
		current->thread.regs = regs;
		regs->orig_r4 = regs->r[4];
		/*
		 * Tracing, seccomp, and audit get their say here, and may
		 * rewrite the number or ask for the call to be skipped.
		 */
		nr = regs->orig_r4;
		regs->r[4] = -ENOSYS;
		/* The arguments are read from the frame: asm/syscall_wrapper.h. */
		if (syscall_enter_from_user_mode_randomize_stack(regs, &nr) &&
		    nr >= 0 && nr < NR_syscalls)
			regs->r[4] = c33_sys_call_table[nr](regs);
		syscall_exit_to_user_mode(regs);
		regs->orig_r4 = -1L;
		current->thread.regs = old_task_regs;
		set_irq_regs(old_regs);
		return regs;
	}

	/* Only a syscall frame carries a number; see arch_do_signal_or_restart(). */
	regs->orig_r4 = -1L;
	/*
	 * Signals and core notes must see the interrupted user frame, rather
	 * than the frame left by its most recent syscall.
	 */
	if (user_mode(regs))
		current->thread.regs = regs;

	if (vector < C33_FIRST_IRQ_VECTOR && user_mode(regs)) {
		/* A program's fault is the program's: it gets a signal. */
		state = irqentry_enter(regs);
		c33_user_fault(vector, regs);
		irqentry_exit(regs, state);
		current->thread.regs = saved_task_regs;
		set_irq_regs(old_regs);
		return regs;
	}

	if (vector < C33_FIRST_IRQ_VECTOR)
		c33_die(vector, regs);

	state = irqentry_enter(regs);
	irq_enter_rcu();
	handle_arch_irq(regs);
	irq_exit_rcu();
	irqentry_exit(regs, state);
	current->thread.regs = saved_task_regs;

	set_irq_regs(old_regs);
	return regs;
}
