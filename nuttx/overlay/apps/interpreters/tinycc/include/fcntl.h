/* SPDX-License-Identifier: LGPL-2.0-or-later */
#ifndef _TCC_FCNTL_H
#define _TCC_FCNTL_H
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 64
#define O_EXCL 128
#define O_TRUNC 512
#define O_BINARY 0
int open(const char *, int, ...);
#endif
