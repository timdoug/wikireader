/* Minimal POSIX stat declarations for compile-only C33 tests.  */

#ifndef C33_TEST_SYS_STAT_H
#define C33_TEST_SYS_STAT_H

typedef unsigned short mode_t;

struct stat
{
  mode_t st_mode;
  long st_size;
};

int stat (const char *, struct stat *);
int fstat (int, struct stat *);

#endif
