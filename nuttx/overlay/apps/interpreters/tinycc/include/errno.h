/* SPDX-License-Identifier: LGPL-2.0-or-later */
#ifndef _TCC_ERRNO_H
#define _TCC_ERRNO_H
int *__errno(void);
#define errno (*__errno())
#define EEXIST 17
#define ENOENT 2
#define ERANGE 34
#endif
