/****************************************************************************
 * apps/system/bench/libct_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/* Every C library suite on the image, run one after another onto the card.
 *
 * Each of these is an ordinary command and can be typed, but only one of the
 * eight had ever been run on the device: arch_libctest, because it is the one
 * that covers this port's hand-written C33 assembly.  The other seven are
 * stdio and locale machinery -- fmemopen, popen, scanf -- which lean on the
 * scheduler, the filesystem and task spawning far more than on the
 * instruction set, and are therefore exactly where an emulator agreeing
 * proves least.  Three of the bugs found here so far were things no emulator
 * was going to model.
 *
 * The list is here rather than typed for the same reason bench's is: the only
 * useful thing to do with the result is compare it against the same image
 * running somewhere else, and test_libc.py checks a file from either side
 * against one set of patterns.
 *
 * arch_libctest is last.  It runs for about three minutes where the rest take
 * seconds, so anything that goes wrong in the quick seven says so early
 * rather than after the wait.
 */

#include <nuttx/config.h>

#include <sys/utsname.h>
#include <sys/wait.h>

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bench_card.h"
#include "builtin/builtin.h"
#include "nshlib/nshlib.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR char *const g_atomic[]      = { "atomic", NULL };
static FAR char *const g_fmemopen[]    = { "fmemopen_test", NULL };
static FAR char *const g_fopencookie[] = { "fopencookie_test", NULL };
static FAR char *const g_memstream[]   = { "open_memstream_test", NULL };
static FAR char *const g_popen[]       = { "popen_test", NULL };
static FAR char *const g_scanf[]       = { "scanftest", NULL };
static FAR char *const g_wcstombs[]    = { "wcstombs", NULL };
static FAR char *const g_archlibc[]    = { "arch_libctest", NULL };

static FAR char *const *const g_suites[] =
{
  g_atomic,
  g_fmemopen,
  g_fopencookie,
  g_memstream,
  g_popen,
  g_scanf,
  g_wcstombs,

  /* Last: three minutes against everything else's seconds. */

  g_archlibc,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t libct_uptime_ms(void)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

static int libct_run(int fd, FAR char *const *argv)
{
  struct nsh_param_s param;
  uint32_t started;
  int status;
  pid_t pid;

  /* Both of the child's streams are this process's descriptor, so all three
   * share one file description and one offset.  Handing over the path would
   * open the file again per stream, and two handles on one FAT file do not
   * know about each other -- see the same note in bench_main.c, which found
   * that out the hard way.
   *
   * A zero here means descriptor 0, so the unused half has to be said.
   */

  memset(&param, 0, sizeof(param));
  param.fd_in  = -1;
  param.fd_out = fd;
  param.fd_err = fd;

  dprintf(fd, "# command: %s\n", argv[0]);

  started = libct_uptime_ms();
  pid = exec_builtin(argv[0], argv, &param);
  if (pid < 0)
    {
      dprintf(fd, "# %s did not start: %d\n", argv[0], pid);
      printf("could not start (%d)\n", pid);
      return -1;
    }

  if (waitpid(pid, &status, 0) < 0)
    {
      printf("lost track of it (%d)\n", errno);
      return -1;
    }

  printf("%.1f s\n", (libct_uptime_ms() - started) / 1000.0);
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR const char *path = argc > 1 ? argv[1] : CONFIG_SYSTEM_BENCH_LIBC_OUTPUT;
  int count = sizeof(g_suites) / sizeof(g_suites[0]);
  struct utsname name;
  uint32_t started;
  int failures = 0;
  bool tofile;
  int fd;

  fd = bench_card_open(path);
  tofile = fd != STDOUT_FILENO;

  dprintf(fd, "# WikiReader C library test run\n");
  if (uname(&name) == 0)
    {
      dprintf(fd, "# %s %s %s %s\n", name.sysname, name.release,
              name.version, name.machine);
    }

  started = libct_uptime_ms();
  for (int i = 0; i < count; i++)
    {
      printf("[%d/%d] %s ", i + 1, count, g_suites[i][0]);
      fflush(stdout);

      if (libct_run(fd, g_suites[i]) < 0)
        {
          failures++;
        }
    }

  dprintf(fd, "# %d of %d suites ran, in %" PRIu32 " seconds\n",
          count - failures, count, (libct_uptime_ms() - started) / 1000);

  if (!tofile)
    {
      return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

  printf("libct: %d of %d ran, %" PRIu32 " s total\n", count - failures,
         count, (libct_uptime_ms() - started) / 1000);
  bench_card_close(fd, path);

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
