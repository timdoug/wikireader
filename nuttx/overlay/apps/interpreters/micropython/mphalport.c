/****************************************************************************
 * apps/interpreters/micropython/mphalport.c
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

/* Everything MicroPython needs from the machine: a terminal and a clock.
 * Both are POSIX here, so the interpreter talks to whichever console the
 * task was started on -- the panel, a serial line, or a pipe.
 */

#include <stdio.h>
#include <math.h>
#include <time.h>
#include <unistd.h>

#include "py/mpconfig.h"
#include "py/mphal.h"
#include "py/runtime.h"
#include "shared/readline/readline.h"

int mp_hal_stdin_rx_chr(void)
{
  unsigned char c = 0;
  ssize_t got;

  fflush(stdout);
  got = read(STDIN_FILENO, &c, 1);

  /* End of input reads as the character that leaves the interpreter.  There
   * is no way to say "no more" to the line editor otherwise: it takes what
   * this returns as a character and asks again, so anything else spins.
   */

  if (got != 1)
    {
      return CHAR_CTRL_D;
    }

  /* The line editor ends a line on a carriage return, which is what a
   * terminal sends; a pipe or a file sends a line feed instead.
   */

  return c == '\n' ? '\r' : c;
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, mp_uint_t len)
{
  ssize_t written = write(STDOUT_FILENO, str, len);

  return written < 0 ? 0 : (mp_uint_t)written;
}

static uint64_t mp_hal_now_us(void)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000000ull + now.tv_nsec / 1000;
}

mp_uint_t mp_hal_ticks_ms(void)
{
  return (mp_uint_t)(mp_hal_now_us() / 1000);
}

mp_uint_t mp_hal_ticks_us(void)
{
  return (mp_uint_t)mp_hal_now_us();
}

mp_uint_t mp_hal_ticks_cpu(void)
{
  return mp_hal_ticks_us();
}

/* The wall clock, for time.time_ns().  Its resolution is the system tick;
 * the extra digits are there because the interface asks for them, not
 * because this clock knows them.
 */

uint64_t mp_hal_time_ns(void)
{
  struct timespec now;

  clock_gettime(CLOCK_REALTIME, &now);
  return (uint64_t)now.tv_sec * 1000000000ull + now.tv_nsec;
}

void mp_hal_delay_ms(mp_uint_t ms)
{
  usleep(ms * 1000);
}

void mp_hal_delay_us(mp_uint_t us)
{
  usleep(us);
}

/* NuttX's maths library has rintf, which rounds to nearest under the
 * current mode, and no nearbyintf, which is the same thing without the
 * inexact flag.  There is no flag to raise on a chip with no floating-point
 * unit, so one is the other here.
 */

float nearbyintf(float x)
{
  return rintf(x);
}

/* Reached when an exception escapes everything, which cannot happen while
 * the interpreter is running its own code.
 */

NORETURN void nlr_jump_fail(void *val)
{
  fprintf(stderr, "micropython: unhandled exception, giving up\n");
  exit(EXIT_FAILURE);
}
