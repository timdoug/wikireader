// SPDX-License-Identifier: GPL-2.0
#include <linux/elfcore.h>
#include <linux/kernel.h>
#include <linux/mm_types.h>
#include <linux/reboot.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/string.h>
#include <linux/syscalls.h>

#include <asm/iram.h>
#include <asm/processor.h>
#include <asm/switch_to.h>
#include <asm/wikireader.h>

#define C33_GRIFO_EXIT_POWER_OFF 1
#define C33_GRIFO_EXIT_REBOOT    2
#define C33_GRIFO_SYSCALL_EXIT   16
#define C33_STRINGIFY_(value)    #value
#define C33_STRINGIFY(value)     C33_STRINGIFY_(value)

struct task_struct *c33_current_task = &init_task;

/*
 * C33 has no hardware TLS register.  On this uniprocessor NOMMU machine,
 * userspace can read this word at its fixed address and obtain its TLS base
 * without a syscall or consuming one of the four callee-saved general registers.
 * The switch publishes it with interrupts disabled, alongside current.
 */
/* Defined at the fixed ABI address by vmlinux.lds.S, outside the allocator. */

SYSCALL_DEFINE1(c33_set_tls, unsigned long, tls)
{
	unsigned long flags;

	local_irq_save(flags);
	current->thread.tls = tls;
	WRITE_ONCE(c33_current_tls, tls);
	local_irq_restore(flags);
	return 0;
}

asmlinkage struct pt_regs *c33_exception_enter(struct pt_regs *regs);
asmlinkage struct pt_regs *c33_exception_exit(struct pt_regs *regs);

asmlinkage struct pt_regs *c33_exception_enter(struct pt_regs *regs)
{
	struct pt_regs *kernel_regs;
	unsigned long stack_lo;

	if (current->thread.in_kernel) {
		regs->from_user = 0;
		return regs;
	}

	/*
	 * The core pushes an exception's frame on the user stack, and this
	 * entry builds pt_regs there before moving to the kernel stack: 108
	 * bytes below the interrupted SP.  Without an MMU nothing stops that
	 * below the stack's allocation, where it lands in whatever the
	 * process or another one keeps next to it.  Say so, rather than let
	 * the damage surface later as someone else's crash.
	 */
	/*
	 * An alternate signal stack can lie below the ordinary stack. Check
	 * against its own allocation rather than diagnosing a valid handler
	 * syscall or interrupt as an ordinary-stack overflow.
	 */
	stack_lo = on_sig_stack(regs->sp) ? current->sas_ss_sp :
		current->thread.stack_lo;
	if (unlikely(regs->sp - 108 < stack_lo)) {
		pr_err("%s[%d]: stack overflow: sp %08lx, stack from %08lx, pc %08lx\n",
		       current->comm, task_pid_nr(current), regs->sp,
		       stack_lo, regs->pc);
		current->thread.stack_lo = 0;
		force_sig(SIGKILL);
	}

	kernel_regs = task_pt_regs(current);
	*kernel_regs = *regs;
	kernel_regs->from_user = 1;
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

/*
 * Grifo draws wherever the LCD controller's main window points, and keeps
 * its picture in IVRAM: hand IVRAM back to the controller and the window
 * back to where Grifo had it (head.S).  From here no code in IVRAM may run,
 * so this runs from SDRAM with interrupts off; nothing from process.c is
 * placed in A0 RAM.
 */
static __noreturn void c33_grifo_exit(unsigned long code)
{
	volatile u32 *iram = (u32 *)C33_LCDC_IRAM;

	__asm__ volatile ("psrclr 4" : : : "memory");
	*iram &= ~1;
	*(volatile u32 *)C33_LCDC_MLADD = c33_boot_mladd;
	*(volatile u32 *)C33_LCDC_MADD = c33_boot_madd;
	__asm__ volatile ("psrclr 4\n\t"
			  "ld.w %%r6, %0\n\t"
			  "ld.w %%ttbr, %1\n\t"
			  "int 1\n\t"
			  ".short " C33_STRINGIFY(C33_GRIFO_SYSCALL_EXIT)
			  : : "r" (code), "r" (c33_boot_ttbr)
			  : "%r6", "memory");
	for (;;)
		__asm__ volatile ("halt");
}

void machine_restart(char *command)
{
	if (c33_grifo_booted)
		c33_grifo_exit(C33_GRIFO_EXIT_REBOOT);
	/* Without the launcher to return to, the watchdog resets the chip. */
	do_kernel_restart(command);
	for (;;)
		__asm__ volatile ("halt");
}

void machine_halt(void)
{
	for (;;)
		__asm__ volatile ("halt");
}

/*
 * Power is Grifo's to cut: the launcher, back in control, turns the unit
 * off.  As a sys-off handler, so that the kernel knows it can power off and
 * poweroff does not settle for a halt.
 */
static int c33_grifo_power_off(struct sys_off_data *data)
{
	c33_grifo_exit(C33_GRIFO_EXIT_POWER_OFF);
}

static int __init c33_power_off_init(void)
{
	if (c33_grifo_booted)
		register_sys_off_handler(SYS_OFF_MODE_POWER_OFF,
					 SYS_OFF_PRIO_FIRMWARE,
					 c33_grifo_power_off, NULL);
	return 0;
}
arch_initcall(c33_power_off_init);

void machine_power_off(void)
{
	do_kernel_power_off();
	machine_halt();
}

void flush_thread(void)
{
	unsigned long flags;

	local_irq_save(flags);
	current->thread.tls = 0;
	WRITE_ONCE(c33_current_tls, 0);
	local_irq_restore(flags);
}

void start_thread(struct pt_regs *regs, unsigned long pc, unsigned long sp)
{
	unsigned long r6 = regs->r[6], r7 = regs->r[7], r8 = regs->r[8];
	unsigned long nr = regs->orig_r4;

	memset(regs, 0, sizeof(*regs));
	regs->pc = pc;
	regs->sp = sp;
	/*
	 * Every program is FDPIC ELF: binfmt_elf_fdpic allocates the stack
	 * with an empty brk area at its bottom, at start_brk, and leaves the
	 * load maps from ELF_FDPIC_PLAT_INIT in %r6-%r8.
	 */
	current->thread.stack_lo = current->mm->start_brk;
	regs->r[6] = r6;
	regs->r[7] = r7;
	regs->r[8] = r8;
	regs->psr = C33_PSR_IE;
	/*
	 * Still execve's: its PTRACE_EVENT_EXEC and syscall-exit stops report
	 * it, as on other architectures.  The syscall path clears it after the
	 * exit stop; r4 is 0, so no restart can match.
	 */
	regs->orig_r4 = nr;
	regs->from_user = 1;
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

	memset(switch_sp, 0, 4 * sizeof(*switch_sp) + sizeof(*childregs));
	p->thread.regs = childregs;
	p->thread.ksp = (unsigned long)switch_sp;
	p->thread.in_kernel = 1;
	p->thread.psr = 0;
	if (args->flags & CLONE_SETTLS)
		p->thread.tls = args->tls;
	/* A thread's stack is wherever its creator put it. */
	if (args->stack && (args->flags & CLONE_VM) && !(args->flags & CLONE_VFORK))
		p->thread.stack_lo = 0;

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
