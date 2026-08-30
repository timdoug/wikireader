/*
 * <stdio.h> for the C33 DejaGnu board only.
 *
 * mini-libc has a printf family but no streams: no stdout, no stderr, no
 * fprintf.  Keep this declaration-only overlay self-contained rather than
 * including mini-libc's header: upstream preprocessor tests deliberately use
 * strict C90/C11 modes, while that legacy header contains extensions which
 * are unrelated to the program under test.
 *
 * Both streams go to the same serial port, and the FILE * argument is
 * ignored throughout.  That is enough for the tests that use it: they
 * check the *return values* of fprintf and vfprintf, which is the part
 * worth exercising -- GCC folds those counts at compile time under
 * -fprintf-return-value, and mini-libc getting them wrong is exactly what
 * pr78622 and pr79327 caught.  Nothing here separates the two streams or
 * buffers anything, so do not read the output as a stdio conformance test.
 */

#ifndef TORTURE_STDIO_H
#define TORTURE_STDIO_H

#include <stdarg.h>
#include <stddef.h>

typedef struct
{
  unsigned char opaque;
} FILE;

int uprintf (int (*) (int), const char *, ...)
  __attribute__((format (printf, 2, 3)));
int snprintf (char *, size_t, const char *, ...)
  __attribute__((format (printf, 3, 4)));
int sprintf (char *, const char *, ...)
  __attribute__((format (printf, 2, 3)));
int printf (const char *, ...) __attribute__((format (printf, 1, 2)));
int vuprintf (int (*) (int), const char *, va_list);
int vsnprintf (char *, size_t, const char *, va_list);
int vsprintf (char *, const char *, va_list);
int vprintf (const char *, va_list);
int puts (const char *);
int putchar (int);

extern FILE *stdout;
extern FILE *stderr;
extern FILE *stdin;

int fprintf (FILE *, const char *, ...) __attribute__((format (printf, 2, 3)));
int vfprintf (FILE *, const char *, va_list);

/*
 * GCC rewrites the printf family into these when the format is simple
 * enough -- fprintf(f, "s") becomes fputs, fprintf(f, "%c", c) becomes
 * fputc, and a format with no conversions can become fwrite.  Leaving any
 * of them undefined turns a test that was going to pass into a link
 * failure reported as UNSUPPORTED, which is the failure mode this whole
 * file exists to remove.
 */
int fputc (int, FILE *);
int fputs (const char *, FILE *);
int putc (int, FILE *);
size_t fwrite (const void *, size_t, size_t, FILE *);
int fflush (FILE *);

#endif
