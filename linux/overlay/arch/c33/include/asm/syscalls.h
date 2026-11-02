/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_SYSCALLS_H
#define _ASM_C33_SYSCALLS_H

#include <asm-generic/syscalls.h>

struct pt_regs;

asmlinkage long c33_sys_rt_sigreturn(void);
asmlinkage long sys_c33_set_tls(unsigned long tls);

/* In kernel text, where a signal handler returns: kernel/irq-entry.S. */
void c33_rt_sigreturn_trampoline(void);

#endif
