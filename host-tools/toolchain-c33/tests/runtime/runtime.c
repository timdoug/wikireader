/*
 * Runtime support for the C33 DejaGnu board.
 *
 * Everything here exists because a torture test referenced a symbol that
 * mini-libc does not define, so the test could not be linked and got
 * reported UNSUPPORTED -- which said nothing about the compiler.  The
 * allocator is grifo's own, compiled from samo-lib/grifo/src/memory.c and
 * only wrapped here, so these tests exercise real firmware code under the
 * new toolchain rather than something written for the occasion.
 */

#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

#include "memory.h"		/* grifo's allocator */

/* ------------------------------------------------------------------ *
 * Console
 * ------------------------------------------------------------------ */

/*
 * REG_EFSIF0_TXD, from samo-lib/include/regs.h.  grifo polls the status
 * register for room before storing; the emulator's transmitter always has
 * room and never signals busy (emulator/src/uart.c), so the poll would
 * only cost instructions.  Anything written here lands in the emulator's
 * serial capture, which means a failing test's own printf output is
 * actually readable.
 */
#define EFSIF0_TXD (*(volatile unsigned char *) 0x00300b00)

/*
 * mini-libc's printf() calls vuprintf(putchar, ...) and returns the count
 * the callback accepted, so returning EOF here would corrupt the return
 * value that several tests check.  Return the character.
 */
int
putchar (int c)
{
  EFSIF0_TXD = (unsigned char) c;
  return (unsigned char) c;
}

/* ------------------------------------------------------------------ *
 * Streams
 *
 * mini-libc has the printf family but no streams at all.  These are the
 * thinnest thing that lets a test say fprintf(stdout, ...): the FILE * is
 * ignored and everything goes to the serial port.  See include/stdio.h for
 * why that is enough, and for why fputc/fputs/fwrite have to exist even
 * though no test names them.
 * ------------------------------------------------------------------ */

static FILE stdout_file, stderr_file, stdin_file;

FILE *stdout = &stdout_file;
FILE *stderr = &stderr_file;
FILE *stdin  = &stdin_file;

int
vfprintf (FILE *stream, const char *format, va_list arguments)
{
  (void) stream;
  return vuprintf (putchar, format, arguments);
}

int
fprintf (FILE *stream, const char *format, ...)
{
  int n;
  va_list args;

  (void) stream;
  va_start (args, format);
  n = vuprintf (putchar, format, args);
  va_end (args);
  return n;
}

int
fputc (int c, FILE *stream)
{
  (void) stream;
  return putchar (c);
}

int
putc (int c, FILE *stream)
{
  return fputc (c, stream);
}

/* fputs returns a nonnegative value on success, not the length.  */
int
fputs (const char *s, FILE *stream)
{
  (void) stream;
  while (*s)
    putchar (*s++);
  return 0;
}

/* Returns the number of *items* written, not bytes.  */
size_t
fwrite (const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
  const unsigned char *p = ptr;
  size_t total = size * nmemb;

  (void) stream;
  if (size == 0 || nmemb == 0)
    return 0;
  while (total--)
    putchar (*p++);
  return nmemb;
}

int
fflush (FILE *stream)
{
  (void) stream;
  return 0;
}

/* ------------------------------------------------------------------ *
 * grifo's serial console, which its allocator reports corruption through
 * ------------------------------------------------------------------ */

int
Serial_PutChar (int c)
{
  return putchar (c);
}

void
Serial_print (const char *message)
{
  while (*message)
    putchar (*message++);
}

int
Serial_vuprintf (const char *format, va_list arguments)
{
  return vuprintf (putchar, format, arguments);
}

int
Serial_printf (const char *format, ...)
{
  int n;
  va_list args;

  va_start (args, format);
  n = vuprintf (putchar, format, args);
  va_end (args);
  return n;
}

/* ------------------------------------------------------------------ *
 * malloc, over grifo's page allocator
 * ------------------------------------------------------------------ */

extern char __heap_start[], __heap_end[];

/* Called from crt0.s before main.  */
void
_runtime_init (void)
{
  Memory_initialise ();
  Memory_SetHeap ((uint32_t) __heap_start, (uint32_t) __heap_end);
}

void *
malloc (size_t size)
{
  return Memory_allocate (size, "malloc");
}

/* This is test-board fallback support, not part of the program under test.
 * Let a testcase provide its own free implementation (pr59330 does so to
 * exercise IPA behavior) without creating a multiple-definition link error.
 */
void __attribute__ ((weak))
free (void *p)
{
  /*
   * free(NULL) is defined to do nothing, but Memory_free would mask the
   * null pointer down to page 0 and read a magic number out of the
   * interrupt vectors.
   */
  if (p != NULL)
    Memory_free (p, "free");
}

void *
calloc (size_t n, size_t size)
{
  size_t bytes = n * size;
  void *p = Memory_allocate (bytes, "calloc");

  if (p != NULL)
    memset (p, 0, bytes);
  return p;
}

/*
 * No realloc.  grifo has no equivalent, and reimplementing it here would
 * mean reaching into the allocation header for the old size -- duplicating
 * memory.c's internals in the harness.  No torture test needs it; if one
 * appears it will fail to link, which reports UNSUPPORTED and is the right
 * answer rather than a silently wrong one.
 */
