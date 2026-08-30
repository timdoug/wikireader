/* POSIX declarations for compile-only tests on the C33 DejaGnu board.  */

#ifndef C33_TEST_UNISTD_H
#define C33_TEST_UNISTD_H

#include <stddef.h>

#ifndef __C33_SSIZE_T_DEFINED
#define __C33_SSIZE_T_DEFINED
typedef long ssize_t;
#endif

int close (int);
ssize_t read (int, void *, size_t);
ssize_t write (int, const void *, size_t);
int dup (int);
int dup2 (int, int);
int isatty (int);
char *getpass (const char *);

#endif
