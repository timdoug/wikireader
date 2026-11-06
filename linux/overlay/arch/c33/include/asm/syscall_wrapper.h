/* SPDX-License-Identifier: GPL-2.0 */
/*
 * System call entry points that take the exception frame, after
 * arch/riscv/include/asm/syscall_wrapper.h.
 *
 * A call's arguments are the words in %r6-%r11, r[6]-r[11] of the frame.
 * Each argument is one word, or two for a 64-bit one, low word first, with
 * no padding before it: musl's __SYSCALL_LL_E and __SYSCALL_LL_O are the
 * same.  __c33_sys_*() reads each argument from its own words and calls
 * __se_sys_*() with the declared types, so the C33 calling convention,
 * which puts a 64-bit fourth word in %r9:%r10 and the rest on the stack,
 * is the compiler's business alone.
 */
#ifndef __ASM_C33_SYSCALL_WRAPPER_H
#define __ASM_C33_SYSCALL_WRAPPER_H

#include <linux/build_bug.h>
#include <asm/ptrace.h>

asmlinkage long __c33_sys_ni_syscall(const struct pt_regs *regs);

/* The words argument a of type t takes in the frame. */
#define __C33_SC_WORDS(t)	(__TYPE_IS_LL(t) ? 2 : 1)
#define __C33_SC_FIELD(t, a)	unsigned long a[__C33_SC_WORDS(t)];
#define __C33_SC_ARG(t, a)						\
	__builtin_choose_expr(__TYPE_IS_LL(t),				\
		(long long)((unsigned long long)			\
			    __c33_args->a[__C33_SC_WORDS(t) - 1] << 32	\
			    | __c33_args->a[0]),			\
		(long)__c33_args->a[0])

/* __MAP() without the commas, for struct members. */
#define __C33_MAP1(m, t, a)		m(t, a)
#define __C33_MAP2(m, t, a, ...)	m(t, a) __C33_MAP1(m, __VA_ARGS__)
#define __C33_MAP3(m, t, a, ...)	m(t, a) __C33_MAP2(m, __VA_ARGS__)
#define __C33_MAP4(m, t, a, ...)	m(t, a) __C33_MAP3(m, __VA_ARGS__)
#define __C33_MAP5(m, t, a, ...)	m(t, a) __C33_MAP4(m, __VA_ARGS__)
#define __C33_MAP6(m, t, a, ...)	m(t, a) __C33_MAP5(m, __VA_ARGS__)
#define __C33_MAP(n, ...)		__C33_MAP##n(__VA_ARGS__)

#define __SYSCALL_DEFINEx(x, name, ...)					\
	asmlinkage long __c33_sys##name(const struct pt_regs *regs);	\
	ALLOW_ERROR_INJECTION(__c33_sys##name, ERRNO);			\
	static long __se_sys##name(__MAP(x,__SC_LONG,__VA_ARGS__));	\
	static inline long __do_sys##name(__MAP(x,__SC_DECL,__VA_ARGS__)); \
	asmlinkage long __c33_sys##name(const struct pt_regs *regs)	\
	{								\
		const struct {						\
			__C33_MAP(x, __C33_SC_FIELD, __VA_ARGS__)	\
		} *__c33_args = (const void *)&regs->r[6];		\
									\
		BUILD_BUG_ON(sizeof(*__c33_args) >			\
			     6 * sizeof(unsigned long));		\
		return __se_sys##name(__MAP(x,__C33_SC_ARG,__VA_ARGS__)); \
	}								\
	static long __se_sys##name(__MAP(x,__SC_LONG,__VA_ARGS__))	\
	{								\
		long ret = __do_sys##name(__MAP(x,__SC_CAST,__VA_ARGS__)); \
		__MAP(x,__SC_TEST,__VA_ARGS__);				\
		__PROTECT(x, ret,__MAP(x,__SC_ARGS,__VA_ARGS__));	\
		return ret;						\
	}								\
	static inline long __do_sys##name(__MAP(x,__SC_DECL,__VA_ARGS__))

#define SYSCALL_DEFINE0(sname)						\
	SYSCALL_METADATA(_##sname, 0);					\
	asmlinkage long __c33_sys_##sname(const struct pt_regs *__unused); \
	ALLOW_ERROR_INJECTION(__c33_sys_##sname, ERRNO);		\
	asmlinkage long __c33_sys_##sname(const struct pt_regs *__unused)

#define COND_SYSCALL(name)						\
	asmlinkage long __weak __c33_sys_##name(const struct pt_regs *regs); \
	asmlinkage long __weak __c33_sys_##name(const struct pt_regs *regs) \
	{								\
		return sys_ni_syscall();				\
	}

#endif /* __ASM_C33_SYSCALL_WRAPPER_H */
