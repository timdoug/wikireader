/* POSIX file-control declarations for C33 analyzer tests.  */

#ifndef C33_TEST_FCNTL_H
#define C33_TEST_FCNTL_H

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_ACCMODE 3
#define O_CREAT  0x0200
#define O_TRUNC  0x0400
#define O_APPEND 0x0008

int open (const char *, int, ...);

#endif
