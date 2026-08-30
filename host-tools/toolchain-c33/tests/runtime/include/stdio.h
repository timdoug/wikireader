/*
 * <stdio.h> for the C33 DejaGnu board only.
 *
 * mini-libc has a printf family but no complete stream implementation.  Keep
 * this declaration-only overlay self-contained rather than
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

#define EOF (-1)
#define BUFSIZ 1024
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#ifndef __C33_SSIZE_T_DEFINED
#define __C33_SSIZE_T_DEFINED
typedef long ssize_t;
#endif

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

/* Declaration-only stream surface for compile-time and analyzer tests.  The
   board classifies links which require these absent mini-libc routines as
   unsupported.  */
FILE *fopen (const char *, const char *);
FILE *freopen (const char *, const char *, FILE *);
int fclose (FILE *);
size_t fread (void *, size_t, size_t, FILE *);
char *fgets (char *, int, FILE *);
int fgetc (FILE *);
int getc (FILE *);
int getchar (void);
int ungetc (int, FILE *);
int fscanf (FILE *, const char *, ...) __attribute__((format (scanf, 2, 3)));
int scanf (const char *, ...) __attribute__((format (scanf, 1, 2)));
int sscanf (const char *, const char *, ...)
  __attribute__((format (scanf, 2, 3)));
int vfscanf (FILE *, const char *, va_list);
int vscanf (const char *, va_list);
int vsscanf (const char *, const char *, va_list);
int fseek (FILE *, long, int);
long ftell (FILE *);
void rewind (FILE *);
void clearerr (FILE *);
int feof (FILE *);
int ferror (FILE *);
int fileno (FILE *);
void perror (const char *);
int remove (const char *);
int rename (const char *, const char *);

#endif
