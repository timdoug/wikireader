/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#define __ARCH_WANT_SYS_CLONE
#define __ARCH_WANT_SYS_FORK
#define __ARCH_WANT_SYS_VFORK
#include <asm-generic/unistd.h>
#define __NR_c33_set_tls (__NR_arch_specific_syscall + 0)
#define __NR_c33_get_tls (__NR_arch_specific_syscall + 1)
#define __NR_c33_get_tls_slot (__NR_arch_specific_syscall + 2)

/* Like asm-generic/unistd.h, also usable to instantiate the syscall table. */
__SYSCALL(__NR_c33_set_tls, sys_c33_set_tls)
__SYSCALL(__NR_c33_get_tls, sys_c33_get_tls)
__SYSCALL(__NR_c33_get_tls_slot, sys_c33_get_tls_slot)
