/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_SYSCALLS_H
#define _ASM_C33_SYSCALLS_H

#include <asm-generic/syscalls.h>

struct pt_regs;

asmlinkage long c33_sys_rt_sigreturn(void);
void c33_do_notify_resume(struct pt_regs *regs, int in_syscall);

#endif
