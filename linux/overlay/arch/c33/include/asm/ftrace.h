/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_FTRACE_H
#define _ASM_C33_FTRACE_H

#ifndef __ASSEMBLY__
/*
 * The syscall table holds the __c33_sys_*() wrappers: syscall tracing
 * finds a call's metadata by the name after the "__c33_" prefix.
 */
#define ARCH_HAS_SYSCALL_MATCH_SYM_NAME
static inline bool arch_syscall_match_sym_name(const char *sym,
					       const char *name)
{
	return !strcmp(sym + 6, name);
}
#endif

#endif
