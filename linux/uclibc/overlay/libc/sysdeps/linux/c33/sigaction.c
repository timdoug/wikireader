/* C33's kernel signal ABI has no userspace restorer field.
 * Keep the established libc structure and marshal its kernel representation.
 * Licensed under the LGPL v2.1, see COPYING.LIB. */
#include <signal.h>
#include <stddef.h>
#include <sys/syscall.h>

struct c33_kernel_sigaction {
	__sighandler_t handler;
	unsigned long flags;
	sigset_t mask;
};

int __libc_sigaction(int sig, const struct sigaction *act,
		    struct sigaction *oact)
{
	struct c33_kernel_sigaction kact, koact;
	int result;
	typedef char check_sigset_size[sizeof(sigset_t) == _NSIG / 8 ? 1 : -1];
	(void)sizeof(check_sigset_size);
	if (act) {
		kact.handler = act->sa_handler;
		kact.flags = act->sa_flags;
		kact.mask = act->sa_mask;
	}
	result = INLINE_SYSCALL(rt_sigaction, 4, sig,
			       act ? &kact : NULL, oact ? &koact : NULL,
			       sizeof(sigset_t));
	if (oact && result == 0) {
		oact->sa_handler = koact.handler;
		oact->sa_flags = koact.flags;
		oact->sa_restorer = NULL;
		oact->sa_mask = koact.mask;
	}
	return result;
}

#ifndef __UCLIBC_HAS_THREADS__
strong_alias(__libc_sigaction, sigaction)
libc_hidden_def(sigaction)
#else
weak_alias(__libc_sigaction, sigaction)
libc_hidden_weak(sigaction)
#endif
