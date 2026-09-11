/* SPDX-License-Identifier: LGPL-2.0-or-later */
#ifndef _TCC_STDARG_H
#define _TCC_STDARG_H
typedef __builtin_va_list va_list;
#define va_start(ap,last) __builtin_va_start(ap,last)
#define va_arg(ap,t) __builtin_va_arg(ap,t)
#define va_end(ap) __builtin_va_end(ap)
#define va_copy(dst,src) __builtin_va_copy(dst,src)
#endif
