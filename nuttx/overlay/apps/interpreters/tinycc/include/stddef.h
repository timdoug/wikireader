/* SPDX-License-Identifier: LGPL-2.0-or-later */
#ifndef _TCC_STDDEF_H
#define _TCC_STDDEF_H
#define NULL ((void *)0)
#define offsetof(t,m) ((size_t)&((t *)0)->m)
typedef unsigned int size_t;
typedef int ptrdiff_t;
typedef int wchar_t;
#endif
