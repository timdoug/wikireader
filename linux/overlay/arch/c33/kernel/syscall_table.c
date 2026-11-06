// SPDX-License-Identifier: GPL-2.0
#include <linux/syscalls.h>
#include <linux/unistd.h>

#include <asm/syscall.h>

#define __c33_sys_mmap2 __c33_sys_mmap_pgoff

#define __SYSCALL_WITH_COMPAT(nr, native, compat) __SYSCALL(nr, native)

#undef __SYSCALL
#define __SYSCALL(nr, call) asmlinkage long __c33_##call(const struct pt_regs *);
#include <asm/syscall_table_32.h>

#undef __SYSCALL
#define __SYSCALL(nr, call) [nr] = __c33_##call,

/* Not SYSCALL_DEFINE0(), which would allow error injection into it. */
asmlinkage long __c33_sys_ni_syscall(const struct pt_regs *__unused)
{
	return -ENOSYS;
}

const syscall_fn_t c33_sys_call_table[__NR_syscalls] = {
	[0 ... __NR_syscalls - 1] = __c33_sys_ni_syscall,
#include <asm/syscall_table_32.h>
};
