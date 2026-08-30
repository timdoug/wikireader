/* <stdlib.h> for the C33 DejaGnu board only.  */

#ifndef C33_TEST_STDLIB_H
#define C33_TEST_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fffffffL

typedef struct
{
  int quot;
  int rem;
} div_t;

typedef struct
{
  long quot;
  long rem;
} ldiv_t;

void abort (void) __attribute__((noreturn));
void exit (int) __asm__("__stop_progExec__") __attribute__((noreturn));

int abs (int) __attribute__((const));
long labs (long) __attribute__((const));
div_t div (int, int);
ldiv_t ldiv (long, long);

long strtol (const char *, char **, int);
unsigned long strtoul (const char *, char **, int);
int atoi (const char *);
long atol (const char *);

void *malloc (size_t) __attribute__((malloc));
void *calloc (size_t, size_t) __attribute__((malloc));
void *realloc (void *, size_t);
void free (void *);
void *bsearch (const void *, const void *, size_t, size_t,
	       int (*) (const void *, const void *));

int rand (void);
void srand (unsigned int);
int rand_r (unsigned int *);

/* POSIX, used by GCC's analyzer diagnostic-path tests.  */
long random (void);
char *getenv (const char *);
int putenv (char *);
int setenv (const char *, const char *, int);
int unsetenv (const char *);

/* Legacy mini-libc extensions used by the board runtime.  */
char *itoa (int, char *, int);
char *utoa (unsigned int, char *, int);
char *ltoa (long, char *, int);
char *ultoa (unsigned long, char *, int);

#endif
