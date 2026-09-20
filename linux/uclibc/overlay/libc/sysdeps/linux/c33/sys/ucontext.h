#ifndef _SYS_UCONTEXT_H
#define _SYS_UCONTEXT_H 1

#include <features.h>
#include <signal.h>
#include <bits/sigcontext.h>

typedef struct sigcontext mcontext_t;

typedef struct ucontext {
	unsigned long uc_flags;
	struct ucontext *uc_link;
	stack_t uc_stack;
	mcontext_t uc_mcontext;
	__sigset_t uc_sigmask;
} ucontext_t;

#endif
