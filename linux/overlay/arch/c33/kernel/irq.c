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
#include <linux/mm.h>
#include <linux/sched/signal.h>
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

/*
 * A system call's six argument words arrive in %r6-%r11.  The C33 ABI
 * passes a function's first four words in %r6-%r9 and the rest on the
 * stack, except that a 64-bit argument starting at the fourth word runs on
 * into %r10: pread64's and pwrite64's offset.  So the table's functions are
 * called with words four and five as one 64-bit argument, which puts %r10
 * in %r10, and then with words five and six again, as the stack arguments
 * every other call reads them from.
 */
typedef long (*c33_syscall_fn_t)(unsigned long, unsigned long,
				 unsigned long, unsigned long long,
				 unsigned long, unsigned long);

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

/*
 * The core's exceptions (S1C33E07 manual 6.3): a third ext prefix (2), an
 * undefined instruction (3) and a misaligned halfword or word access (6).
 * In a program they are its own bug, and it gets the signal a larger machine
 * would send.
 */
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

	pr_info_ratelimited("%s[%d]: C33 exception %u at pc %08lx sp %08lx r15 %08lx\n",
			    current->comm, task_pid_nr(current), vector,
			    regs->pc, regs->sp, regs->r[15]);
	pr_info("  r0-r3 %08lx %08lx %08lx %08lx r4 %08lx r12-r14 %08lx %08lx %08lx\n",
		regs->r[0], regs->r[1], regs->r[2], regs->r[3], regs->r[4],
		regs->r[12], regs->r[13], regs->r[14]);
	c33_fault_where("pc", regs->pc);
	c33_fault_where("r4", regs->r[4]);
	c33_fault_where("r15", regs->r[15]);
	if (!get_user(ret, (unsigned long __user *)regs->sp))
		c33_fault_where("[sp]", ret);
	switch (vector) {
	case 2:
	case 3:
		force_sig_fault(SIGILL, ILL_ILLOPC, pc);
		break;
	case 6:
		force_sig_fault(SIGBUS, BUS_ADRALN, pc);
		break;
	default:
		force_sig_fault(SIGSEGV, SEGV_MAPERR, pc);
		break;
	}
}

asmlinkage struct pt_regs *c33_handle_irq(unsigned int vector,
					  struct pt_regs *regs)
{
	struct pt_regs *old_regs = set_irq_regs(regs);
	struct pt_regs *saved_task_regs = current->thread.regs;
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
					((unsigned long long)regs->r[10] << 32)
					| regs->r[9],
					regs->r[10], regs->r[11]);
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
	/* Signals and core notes must see the interrupted user frame, rather
	 * than the frame left by its most recent syscall. */
	if (user_mode(regs))
		current->thread.regs = regs;

	if (!s1c33_itc_is_source(vector) && user_mode(regs)) {
		/* A program's fault is the program's: it gets a signal. */
		state = irqentry_enter(regs);
		c33_user_fault(vector, regs);
		irqentry_exit(regs, state);
		current->thread.regs = saved_task_regs;
		set_irq_regs(old_regs);
		return regs;
	}

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
	current->thread.regs = saved_task_regs;

	set_irq_regs(old_regs);
	return regs;
}
