/* SPDX-License-Identifier: LGPL-2.0-or-later */
#ifndef _TCC_UNISTD_H
#define _TCC_UNISTD_H
#include <stddef.h>
typedef int ssize_t;
typedef int off_t;
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
int close(int);
ssize_t read(int, void *, size_t);
ssize_t write(int, const void *, size_t);
off_t lseek(int, off_t, int);
int unlink(const char *);
char *getcwd(char *, size_t);
#endif
