// SPDX-License-Identifier: GPL-2.0
#include <linux/syscalls.h>
#include <linux/unistd.h>

#include <asm/syscalls.h>

#undef __SYSCALL
#define __SYSCALL(nr, call) [nr] = (call),

#define sys_mmap2 sys_mmap_pgoff
#define sys_rt_sigreturn sys_ni_syscall

void *const c33_sys_call_table[__NR_syscalls] = {
	[0 ... __NR_syscalls - 1] = sys_ni_syscall,
#include <uapi/asm-generic/unistd.h>
};
