#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise PTY output batching, termios translations, signals and short writes."""

import argparse
from pathlib import Path
import subprocess
import tempfile

STUB = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#define FAR
#define DEBUGASSERT assert
#define inline_function inline
#define OPOST 1
#define OCRNL 2
#define ONLCR 4
#define ONLRET 8
#define ISIG 1
#define INTR_CHAR 3
#define SUSP_CHAR 26
#undef _POSIX_VDISABLE
#define _POSIX_VDISABLE '\0'
typedef unsigned char cc_t;
typedef unsigned int tcflag_t;
"""

# The driver's own pieces, extracted from the tree below STUB: the termios
# structure and helpers from serial.h and serial_termios.c, then pty_write().
PIPE = r"""
struct inode { void *i_private; };
struct file { struct inode *f_inode; };
struct pty_devpair_s;
struct pty_dev_s {
  struct pty_devpair_s *pd_devpair;
  struct file pd_sink;
  bool pd_master;
  pid_t pd_pid;
  struct uart_termios_s pd_termios;
};
struct pty_devpair_s {
  struct pty_dev_s pp_master, pp_slave;
};
static unsigned char output[8192];
static size_t used, calls, capacity;
static int fail_at, fail_errno, got_signal, signal_pid;

/* A non-blocking sink pipe: a full pipe is -EAGAIN, never a zero count. */

static ssize_t file_write(struct file *file, const void *data, size_t len)
{
  calls++;
  if ((int)calls == fail_at) return fail_errno;
  if (used == capacity) return -EAGAIN;
  if (len > capacity - used) len = capacity - used;
  memcpy(output + used, data, len);
  used += len;
  return len;
}
#if defined(CONFIG_TTY_SIGINT) || defined(CONFIG_TTY_SIGTSTP)
static int nxsig_kill_foreground(pid_t pid, int signo)
{
  assert(got_signal == 0);
  got_signal = signo;
  signal_pid = pid;
  return 0;
}
#endif
static void reset(void)
{
  used = calls = 0;
  capacity = sizeof(output);
  fail_at = -1;
  fail_errno = -EAGAIN;
  got_signal = signal_pid = 0;
}
"""

TEST = r"""
static unsigned seed = 123456;
static unsigned rnd(void)
{
  seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
  return seed;
}
int main(void)
{
  struct pty_devpair_s pair = {0};
  struct pty_dev_s dev = {.pd_devpair = &pair};
  struct inode inode = {&dev};
  struct file file = {&inode};
  char input[512];
  unsigned char expected[1024];

  /* The pair's terminal settings live in the slave: ISIG and the INTR and
   * SUSP characters are read from there for both ends.
   */

  pair.pp_slave.pd_termios.lflag = ISIG;
#ifdef CONFIG_TTY_SIGINT
  pair.pp_slave.pd_termios.vintr = INTR_CHAR;
#endif
#ifdef CONFIG_TTY_SIGTSTP
  pair.pp_slave.pd_termios.vsusp = SUSP_CHAR;
#endif

  /* With no foreground PID, every byte is text.  CR is inserted before NL
   * under ONLCR only, ONLRET is not acted on, and a CR turned into NL by
   * OCRNL is not given a CR of its own.
   */

  for (unsigned flags = 0; flags < 16; flags++)
    for (unsigned trial = 0; trial < 200; trial++)
      {
        size_t len = rnd() % sizeof(input), count = 0;
        for (size_t i = 0; i < len; i++)
          {
            unsigned char c = rnd();
            input[i] = c;
            if (flags & OPOST)
              {
                if (c == '\n' && (flags & ONLCR))
                  expected[count++] = '\r';
                else if (c == '\r' && (flags & OCRNL))
                  c = '\n';
              }
            expected[count++] = c;
          }
        reset(); dev.pd_termios.oflag = flags;
        assert(pty_write(&file, input, len) == (ssize_t)len);
        assert(used == count && memcmp(output, expected, count) == 0);
        assert(got_signal == 0);
      }

  /* Untranslated output must reach the pipe in a single write. */
  reset(); dev.pd_termios.oflag = OPOST | ONLCR;
  memset(input, 'a', sizeof(input));
  assert(pty_write(&file, input, sizeof(input)) == sizeof(input));
  assert(calls == 1 && used == sizeof(input));
  assert(memcmp(output, input, used) == 0);

  /* A full pipe after a partial write reports only consumed input; retrying
   * the tail must neither drop nor duplicate bytes.  A pipe full from the
   * start reports the error itself.
   */
  reset(); capacity = 0;
  assert(pty_write(&file, input, sizeof(input)) == -EAGAIN && used == 0);
  for (size_t limit = 7; limit < sizeof(input); limit += 7)
    {
      reset(); capacity = limit;
      assert(pty_write(&file, input, sizeof(input)) == (ssize_t)limit);
      assert(used == limit);
      capacity = sizeof(output);
      assert(pty_write(&file, input + limit, sizeof(input) - limit) ==
             (ssize_t)(sizeof(input) - limit));
      assert(used == sizeof(input) && memcmp(output, input, used) == 0);
    }
  reset(); fail_at = 1;
  assert(pty_write(&file, "abc", 3) == -EAGAIN && used == 0);
  reset(); fail_at = 3;
  assert(pty_write(&file, "\nabc", 4) == 1);
  assert(used == 2 && memcmp(output, "\r\n", 2) == 0);
  fail_at = -1;
  assert(pty_write(&file, "abc", 3) == 3);
  assert(used == 5 && memcmp(output, "\r\nabc", 5) == 0);

  /* A closed master is EIO on the slave and stays EPIPE on the master. */
  for (int master = 0; master < 2; master++)
    {
      reset(); fail_at = 1; fail_errno = -EPIPE; dev.pd_master = master;
      assert(pty_write(&file, "abc", 3) == (master ? -EPIPE : -EIO));
    }

  /* Control bytes stop their text span, are consumed, and signal the
   * opposite endpoint's foreground PID.  They must not leak into the pipe
   * as ordinary text.
   */
  pair.pp_master.pd_pid = 101;
  pair.pp_slave.pd_pid = 202;
  for (int master = 0; master < 2; master++)
    {
      dev.pd_master = master;
#ifdef CONFIG_TTY_SIGINT
      reset();
      assert(pty_write(&file, "ab\003tail", 7) == 3);
      assert(got_signal == SIGINT && signal_pid == (master ? 202 : 101));
      assert(used == 2 && memcmp(output, "ab", 2) == 0);
#endif
#ifdef CONFIG_TTY_SIGTSTP
      reset();
      assert(pty_write(&file, "cd\032tail", 7) == 3);
      assert(got_signal == SIGTSTP && signal_pid == (master ? 202 : 101));
      assert(used == 2 && memcmp(output, "cd", 2) == 0);
#endif
    }

  /* Without ISIG, or with the character disabled, a control byte is text. */
  reset(); pair.pp_slave.pd_termios.lflag = 0;
  assert(pty_write(&file, "ab\003\032tail", 8) == 8);
  assert(got_signal == 0 && used == 8 && memcmp(output, "ab\003\032tail", 8) == 0);
  pair.pp_slave.pd_termios.lflag = ISIG;
#ifdef CONFIG_TTY_SIGINT
  reset(); pair.pp_slave.pd_termios.vintr = _POSIX_VDISABLE;
  assert(pty_write(&file, "ab\003tail", 7) == 7);
  assert(got_signal == 0 && used == 7);
  pair.pp_slave.pd_termios.vintr = INTR_CHAR;
#endif
  puts("PASS: 3200 translation cases, bulk writes, partial/error retries, EIO and signals");
}
"""


def extract(text, start, end="\n}\n"):
    """Return text from START through the first END after it."""
    begin = text.index(start)
    return text[begin:text.index(end, begin) + len(end)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path,
                        default=Path(__file__).resolve().parents[5])
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    root = args.source_root
    pty = (root / "drivers/serial/pty.c").read_text()
    serial_h = (root / "include/nuttx/serial/serial.h").read_text()
    termios_c = (root / "drivers/serial/serial_termios.c").read_text()
    # Compile the actual function and the termios helpers it calls,
    # replacing only the surrounding driver infrastructure with a
    # controllable pipe and signal sink.
    driver = (extract(serial_h, "struct uart_termios_s\n{", "\n};\n") +
              extract(serial_h, "static inline_function size_t\n"
                                "uart_termios_out_char(") +
              extract(termios_c, "FAR const char *\n"
                                 "uart_termios_isig_find(") +
              PIPE +
              extract(pty, "static ssize_t pty_write(FAR struct file *filep,\n"
                           "                         FAR const char *buffer, "
                           "size_t len)\n"))
    with tempfile.TemporaryDirectory(prefix="wr-pty-output-") as tmp:
        out = Path(tmp)
        (out / "test.c").write_text(STUB + driver + TEST)
        for signals in ((), ("SIGINT",), ("SIGTSTP",), ("SIGINT", "SIGTSTP")):
            cmd = [args.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-parameter", "-Wno-sign-compare",
                   "-fsanitize=address,undefined",
                   "-g", "-O1", *[f"-DCONFIG_TTY_{s}=1" for s in signals],
                   str(out / "test.c"), "-o", str(out / "test")]
            subprocess.run(cmd, check=True)
            print(f"Signal configuration: {signals}", flush=True)
            subprocess.run([str(out / "test")], check=True)


if __name__ == "__main__":
    main()
