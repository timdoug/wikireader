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
#define OPOST 1
#define OCRNL 2
#define ONLCR 4
#define ONLRET 8
#define CONFIG_TTY_SIGINT_CHAR 3
#define CONFIG_TTY_SIGTSTP_CHAR 26
struct inode { void *i_private; };
struct file { struct inode *f_inode; };
struct pty_devpair_s;
struct pty_dev_s {
  struct pty_devpair_s *pd_devpair;
  struct file pd_sink;
  bool pd_master;
  pid_t pd_pid;
  unsigned pd_oflag;
};
struct pty_devpair_s { struct pty_dev_s pp_master, pp_slave; };
static unsigned char output[8192];
static size_t used, calls, capacity;
static int fail_at, got_signal, signal_pid;
static ssize_t file_write(struct file *file, const void *data, size_t len)
{
  calls++;
  if ((int)calls == fail_at) return -EAGAIN;
  if (len > capacity - used) len = capacity - used;
  memcpy(output + used, data, len);
  used += len;
  return len;
}
#if defined(CONFIG_TTY_SIGINT) || defined(CONFIG_TTY_SIGTSTP)
static int nxsig_kill(pid_t pid, int signo)
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
                if (c == '\r' && (flags & OCRNL)) c = '\n';
                if (c == '\n' && (flags & (ONLCR | ONLRET)))
                  expected[count++] = '\r';
              }
            expected[count++] = c;
          }
        reset(); dev.pd_oflag = flags;
        assert(pty_write(&file, input, len) == (ssize_t)len);
        assert(used == count && memcmp(output, expected, count) == 0);
        assert(got_signal == 0);
      }

  /* Untranslated output must reach the pipe in a single write. */
  reset(); dev.pd_oflag = OPOST | ONLCR;
  memset(input, 'a', sizeof(input));
  assert(pty_write(&file, input, sizeof(input)) == sizeof(input));
  assert(calls == 1 && used == sizeof(input));
  assert(memcmp(output, input, used) == 0);

  /* A partial pipe write reports only consumed input; retrying the tail
   * must neither drop nor duplicate bytes.  Include a zero-length result.
   */
  for (size_t limit = 0; limit < sizeof(input); limit += 7)
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

  /* Control bytes stop their text span and use the opposite endpoint's
   * foreground PID.  They must not leak into the pipe as ordinary text.
   */
  pair.pp_master.pd_pid = 101;
  pair.pp_slave.pd_pid = 202;
  for (int master = 0; master < 2; master++)
    {
      dev.pd_master = master;
#ifdef CONFIG_TTY_SIGINT
      reset();
      assert(pty_write(&file, "ab\003tail", 7) == 1);
      assert(got_signal == SIGINT && signal_pid == (master ? 202 : 101));
      assert(used == 2 && memcmp(output, "ab", 2) == 0);
#endif
#ifdef CONFIG_TTY_SIGTSTP
      reset();
      assert(pty_write(&file, "cd\032tail", 7) == 1);
      assert(got_signal == SIGTSTP && signal_pid == (master ? 202 : 101));
      assert(used == 2 && memcmp(output, "cd", 2) == 0);
#endif
    }
  puts("PASS: 3200 translation cases, bulk writes, partial/error retries and signals");
}
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path,
                        default=Path(__file__).resolve().parents[5])
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    source = (args.source_root / "drivers/serial/pty.c").read_text()
    # Compile the actual function, replacing only its surrounding driver
    # infrastructure with a controllable pipe and signal sink.
    start = source.index("static ssize_t pty_write(FAR struct file *filep,\n")
    end = source.index("\n}\n", start) + 3
    with tempfile.TemporaryDirectory(prefix="wr-pty-output-") as tmp:
        out = Path(tmp)
        (out / "test.c").write_text(STUB + source[start:end] + TEST)
        for signals in ((), ("SIGINT",), ("SIGTSTP",), ("SIGINT", "SIGTSTP")):
            cmd = [args.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-parameter", "-fsanitize=address,undefined",
                   "-g", "-O1", *[f"-DCONFIG_TTY_{s}=1" for s in signals],
                   str(out / "test.c"), "-o", str(out / "test")]
            subprocess.run(cmd, check=True)
            print(f"Signal configuration: {signals}", flush=True)
            subprocess.run([str(out / "test")], check=True)


if __name__ == "__main__":
    main()
