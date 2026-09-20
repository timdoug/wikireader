// SPDX-License-Identifier: GPL-2.0
#include <linux/errno.h>
#include <linux/resume_user_mode.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/signal.h>
#include <linux/uaccess.h>

#include <asm/ptrace.h>
#include <asm/syscalls.h>
#include <asm/ucontext.h>

struct c33_rt_sigframe {
	unsigned long return_address;
	struct siginfo info;
	struct ucontext uc;
};

extern void c33_rt_sigreturn_trampoline(void);

static int c33_restore_sigcontext(struct pt_regs *regs,
				  struct sigcontext __user *context)
{
	unsigned long psr;
	int error = 0;

	current->restart_block.fn = do_no_restart_syscall;
	error |= __copy_from_user(regs->r, context->r, sizeof(regs->r));
	error |= __get_user(regs->alr, &context->alr);
	error |= __get_user(regs->ahr, &context->ahr);
	error |= __get_user(regs->sp, &context->sp);
	error |= __get_user(psr, &context->psr);
	error |= __get_user(regs->pc, &context->pc);
	regs->psr = psr & 0x1f;
	regs->orig_r4 = -1;
	regs->reserved = 1;
	return error;
}

asmlinkage long c33_sys_rt_sigreturn(void)
{
	struct pt_regs *regs = current_pt_regs();
	struct c33_rt_sigframe __user *frame;
	sigset_t set;

	frame = (void __user *)(regs->sp -
			offsetof(struct c33_rt_sigframe, info));
	if (((unsigned long)frame & 15) || !access_ok(frame, sizeof(*frame)))
		goto badframe;
	if (__copy_from_user(&set, &frame->uc.uc_sigmask, sizeof(set)))
		goto badframe;
	set_current_blocked(&set);
	if (c33_restore_sigcontext(regs, &frame->uc.uc_mcontext))
		goto badframe;
	if (restore_altstack(&frame->uc.uc_stack))
		goto badframe;
	return regs->r[4];

badframe:
	force_sig(SIGSEGV);
	return 0;
}

static int c33_save_sigcontext(struct sigcontext __user *context,
			       struct pt_regs *regs)
{
	int error = 0;

	error |= __copy_to_user(context->r, regs->r, sizeof(regs->r));
	error |= __put_user(regs->alr, &context->alr);
	error |= __put_user(regs->ahr, &context->ahr);
	error |= __put_user(regs->sp, &context->sp);
	error |= __put_user(regs->psr, &context->psr);
	error |= __put_user(regs->pc, &context->pc);
	return error;
}

static void __user *c33_get_sigframe(struct ksignal *ksig,
				     struct pt_regs *regs, size_t size)
{
	unsigned long sp = sigsp(regs->sp, ksig);

	return (void __user *)((sp - size) & ~15UL);
}

static int c33_setup_rt_frame(struct ksignal *ksig, sigset_t *set,
			      struct pt_regs *regs)
{
	struct c33_rt_sigframe __user *frame;
	int error = 0;

	frame = c33_get_sigframe(ksig, regs, sizeof(*frame));
	if (!access_ok(frame, sizeof(*frame)))
		return -EFAULT;

	error |= __put_user((unsigned long)c33_rt_sigreturn_trampoline,
			    &frame->return_address);
	if (ksig->ka.sa.sa_flags & SA_SIGINFO)
		error |= copy_siginfo_to_user(&frame->info, &ksig->info);
	error |= __put_user(0, &frame->uc.uc_flags);
	error |= __put_user(NULL, &frame->uc.uc_link);
	error |= __save_altstack(&frame->uc.uc_stack, regs->sp);
	error |= c33_save_sigcontext(&frame->uc.uc_mcontext, regs);
	error |= __copy_to_user(&frame->uc.uc_sigmask, set, sizeof(*set));
	if (error)
		return -EFAULT;

	regs->sp = (unsigned long)frame;
	regs->r[6] = ksig->sig;
	regs->r[7] = (unsigned long)&frame->info;
	regs->r[8] = (unsigned long)&frame->uc;
	regs->pc = (unsigned long)ksig->ka.sa.sa_handler;
	return 0;
}

static void c33_handle_signal(struct ksignal *ksig, struct pt_regs *regs)
{
	int error;

	rseq_signal_deliver(ksig, regs);
	error = c33_setup_rt_frame(ksig, sigmask_to_save(), regs);
	signal_setup_done(error, ksig, 0);
}

static void c33_restart_syscall(struct pt_regs *regs,
				struct k_sigaction *action, bool handler)
{
	switch (regs->r[4]) {
	case -ERESTART_RESTARTBLOCK:
	case -ERESTARTNOHAND:
		if (handler) {
			regs->r[4] = -EINTR;
			return;
		}
		break;
	case -ERESTARTSYS:
		if (handler && !(action->sa.sa_flags & SA_RESTART)) {
			regs->r[4] = -EINTR;
			return;
		}
		break;
	case -ERESTARTNOINTR:
		break;
	default:
		return;
	}

	regs->r[4] = regs->orig_r4;
	regs->pc -= 2;
}

static void c33_do_signal(struct pt_regs *regs, int in_syscall)
{
	struct ksignal ksig;

	if (get_signal(&ksig)) {
		if (in_syscall)
			c33_restart_syscall(regs, &ksig.ka, true);
		c33_handle_signal(&ksig, regs);
		return;
	}
	if (in_syscall)
		c33_restart_syscall(regs, NULL, false);
	restore_saved_sigmask();
}

void c33_do_notify_resume(struct pt_regs *regs, int in_syscall)
{
	unsigned long flags;

	if (!user_mode(regs))
		return;

	while ((flags = read_thread_flags()) & _TIF_WORK_MASK) {
		local_irq_enable();
		if (flags & _TIF_NEED_RESCHED)
			schedule();
		else if (flags & (_TIF_SIGPENDING | _TIF_NOTIFY_SIGNAL)) {
			c33_do_signal(regs, in_syscall);
			in_syscall = 0;
		} else if (flags & _TIF_NOTIFY_RESUME) {
			resume_user_mode_work(regs);
		}
		local_irq_disable();
	}
}
