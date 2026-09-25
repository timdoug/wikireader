/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_UACCESS_H
#define _ASM_C33_UACCESS_H

/*
 * No MMU: user memory is ordinary memory.  This is asm-generic's
 * UACCESS_MEMCPY with one difference.  The generic version reaches every
 * 2- and 4-byte user word through get_unaligned, which on this core is a
 * load per byte, because an unaligned word access traps.  User pointers are
 * nearly always aligned, so check and load directly.
 */
#include <linux/align.h>
#include <linux/string.h>
#include <linux/unaligned.h>

static __always_inline unsigned long
raw_copy_from_user(void *to, const void __user *from, unsigned long n)
{
	memcpy(to, (const void __force *)from, n);
	return 0;
}

static __always_inline unsigned long
raw_copy_to_user(void __user *to, const void *from, unsigned long n)
{
	memcpy((void __force *)to, from, n);
	return 0;
}
#define INLINE_COPY_FROM_USER
#define INLINE_COPY_TO_USER

#define __c33_aligned(p, size) IS_ALIGNED((unsigned long)(p), size)

static __always_inline int
__get_user_fn(size_t size, const void __user *from, void *to)
{
	const void *p = (const void __force *)from;

	BUILD_BUG_ON(!__builtin_constant_p(size));

	switch (size) {
	case 1:
		*(u8 *)to = *(const u8 *)p;
		return 0;
	case 2:
		*(u16 *)to = __c33_aligned(p, 2) ? *(const u16 *)p :
			     get_unaligned((const u16 *)p);
		return 0;
	case 4:
		*(u32 *)to = __c33_aligned(p, 4) ? *(const u32 *)p :
			     get_unaligned((const u32 *)p);
		return 0;
	case 8:
		*(u64 *)to = get_unaligned((const u64 *)p);
		return 0;
	default:
		BUILD_BUG();
		return 0;
	}
}
#define __get_user_fn(sz, u, k)	__get_user_fn(sz, u, k)

static __always_inline int
__put_user_fn(size_t size, void __user *to, void *from)
{
	void *p = (void __force *)to;

	BUILD_BUG_ON(!__builtin_constant_p(size));

	switch (size) {
	case 1:
		*(u8 *)p = *(u8 *)from;
		return 0;
	case 2:
		if (__c33_aligned(p, 2))
			*(u16 *)p = *(u16 *)from;
		else
			put_unaligned(*(u16 *)from, (u16 *)p);
		return 0;
	case 4:
		if (__c33_aligned(p, 4))
			*(u32 *)p = *(u32 *)from;
		else
			put_unaligned(*(u32 *)from, (u32 *)p);
		return 0;
	case 8:
		put_unaligned(*(u64 *)from, (u64 *)p);
		return 0;
	default:
		BUILD_BUG();
		return 0;
	}
}
#define __put_user_fn(sz, u, k)	__put_user_fn(sz, u, k)

#define __get_kernel_nofault(dst, src, type, err_label)			\
do {									\
	*((type *)dst) = get_unaligned((type *)(src));			\
	if (0) /* make sure the label looks used to the compiler */	\
		goto err_label;						\
} while (0)

#define __put_kernel_nofault(dst, src, type, err_label)			\
do {									\
	put_unaligned(*((type *)src), (type *)(dst));			\
	if (0) /* make sure the label looks used to the compiler */	\
		goto err_label;						\
} while (0)

#include <asm-generic/uaccess.h>

#endif
