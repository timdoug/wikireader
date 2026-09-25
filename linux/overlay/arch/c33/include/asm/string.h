/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_STRING_H
#define _ASM_C33_STRING_H

/* Word-at-a-time versions in arch/c33/lib/string.S. */
#define __HAVE_ARCH_MEMSET
extern void *memset(void *s, int c, __kernel_size_t n);
#define __HAVE_ARCH_MEMCPY
extern void *memcpy(void *dest, const void *src, __kernel_size_t n);
#define __HAVE_ARCH_MEMMOVE
extern void *memmove(void *dest, const void *src, __kernel_size_t n);

#endif
