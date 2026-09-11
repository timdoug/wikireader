/* SPDX-License-Identifier: LGPL-2.0-or-later */
#ifndef _TCC_STDLIB_H
#define _TCC_STDLIB_H
#include <stddef.h>
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void free(void *);
void exit(int);
void abort(void);
int atoi(const char *);
long strtol(const char *, char **, int);
unsigned long strtoul(const char *, char **, int);
int abs(int);
double strtod(const char *, char **);
float strtof(const char *, char **);
long double strtold(const char *, char **);
long long strtoll(const char *, char **, int);
unsigned long long strtoull(const char *, char **, int);
char *getenv(const char *);
char *realpath(const char *, char *);
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
#endif
