/* SPDX-License-Identifier: LGPL-2.0-or-later */
#ifndef _TCC_STDIO_H
#define _TCC_STDIO_H
#include <stddef.h>
#include <stdarg.h>
typedef void FILE;
FILE *lib_get_stream(int);
#define stdin lib_get_stream(0)
#define stdout lib_get_stream(1)
#define stderr lib_get_stream(2)
#define EOF (-1)
int printf(const char *, ...);
int sprintf(char *, const char *, ...);
int snprintf(char *, size_t, const char *, ...);
int puts(const char *);
int putchar(int);
int getchar(void);
FILE *fopen(const char *, const char *);
int fclose(FILE *);
size_t fread(void *, size_t, size_t, FILE *);
size_t fwrite(const void *, size_t, size_t, FILE *);
char *fgets(char *, int, FILE *);
int fprintf(FILE *, const char *, ...);
int fflush(FILE *);
FILE *fdopen(int, const char *);
int fputc(int, FILE *);
int fputs(const char *, FILE *);
int vsnprintf(char *, size_t, const char *, va_list);
int vfprintf(FILE *, const char *, va_list);
void perror(const char *);
int remove(const char *);
#endif
