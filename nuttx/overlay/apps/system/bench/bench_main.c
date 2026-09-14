/****************************************************************************
 * apps/system/bench/bench_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/* Every benchmark in the image, run one after another, with what they print
 * going to a file on the card rather than to a screen nobody can scroll
 * back.  The card is unmounted at the end, so the answer to "is it finished"
 * is the line that says so, and the card can then be taken out and read
 * somewhere with a keyboard.
 *
 * The iteration counts are here rather than typed, because the only useful
 * thing to do with these numbers is compare them with the same image running
 * somewhere else -- which means both machines have to do identical work, and
 * an argument remembered differently is the easiest way for that to stop
 * being true.  The emulator harness runs this same command for the same
 * reason: there is one list, and it is this one.
 */

#include <nuttx/config.h>

#include <sys/mount.h>
#include <sys/utsname.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
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
 * Private Types
 ****************************************************************************/

struct benchmark_s
{
  FAR const char *name;
  FAR char *const *argv;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR char *const g_coremark[] =
{
  "coremark", NULL
};

static FAR char *const g_dhrystone[] =
{
  "dhrystone", "200000", NULL
};

static FAR char *const g_whetstone[] =
{
  "whetstone", "10", NULL
};

static FAR char *const g_ramspeed[] =
{
  "ramspeed", "-a", "-s", "1048576", "-n", "20", NULL
};

static FAR char *const g_sdbench[] =
{
  "sdbench", "-b", "4096", "-r", "3", "-d", "1000", NULL
};

static FAR char *const g_ubench[] =
{
  "ubench", NULL
};

static FAR char *const g_selftest[] =
{
  "selftest", NULL
};

static const struct benchmark_s g_benchmarks[] =
{
  {
    "coremark", g_coremark
  },
  {
    "dhrystone", g_dhrystone
  },
  {
    "whetstone", g_whetstone
  },
  {
    "ramspeed", g_ramspeed
  },

  /* Not a benchmark of the machine so much as of its parts: what a fetch,
   * a load and a store each cost, which is what the numbers above cannot
   * be taken apart into.
   */

  {
    "ubench", g_ubench
  },

  /* Not a measurement at all: every language on the image, run once, on the
   * machine rather than on a model of it.
   */

  {
    "selftest", g_selftest
  },

  /* Last, because it is the one that writes to the card the results are
   * going to, and a full card should spoil the cheapest measurement here
   * rather than all of them.
   */

  {
    "sdbench", g_sdbench
  },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t bench_uptime_ms(void)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

/* What ran, so that a file found on a card months later can still say which
 * image produced it.
 */

static void bench_header(int fd, FAR const struct benchmark_s *bench)
{
  struct utsname name;

  dprintf(fd, "# WikiReader benchmark run\n");
  if (uname(&name) == 0)
    {
      dprintf(fd, "# %s %s %s %s\n", name.sysname, name.release,
              name.version, name.machine);
    }

  dprintf(fd, "# command: %s", bench->name);
  for (int i = 1; bench->argv[i] != NULL; i++)
    {
      dprintf(fd, " %s", bench->argv[i]);
    }

  dprintf(fd, "\n");
}

/* What the loader left the memory system on.  The boot loader in flash
 * brings the SDRAM up on its most conservative timings and grifo retimes it
 * on the way past, so whether that happened is the difference between a row
 * change costing seven clocks and nineteen -- and it belongs with any
 * number measured here, because it changes all of them.
 */

static void bench_memory(int fd)
{
  uint32_t ctl = *(FAR volatile uint32_t *)0x00301604;
  uint32_t ref = *(FAR volatile uint32_t *)0x00301608;

  dprintf(fd, "# sdram: tRP %u, tRAS %u, tRC %u, refresh 0x%x "
              "(ctl 0x%08" PRIx32 ", ref 0x%08" PRIx32 ")\n",
          (unsigned)(((ctl >> 12) & 3) + 1), (unsigned)(((ctl >> 8) & 7) + 1),
          (unsigned)(((ctl >> 4) & 15) + 1), (unsigned)(ref & 0xfff),
          ctl, ref);
}

static int bench_run(int fd, FAR const struct benchmark_s *bench)
{
  struct nsh_param_s param;
  uint32_t started;
  int status;
  pid_t pid;

  /* Both of the child's streams are this process's descriptor, so all three
   * share one file description and one offset.  Handing over the path
   * instead would open the file twice more, and two handles on one FAT file
   * do not know about each other: whichever closes last writes its own idea
   * of the length into the directory entry, and everything the others wrote
   * is gone.  That is not a theory -- it is what this did first, and the
   * file came back holding nothing but the headers this process wrote.
   *
   * A zero in this structure means descriptor 0, so the unused half has to
   * be said out loud.
   */

  memset(&param, 0, sizeof(param));
  param.fd_in  = -1;
  param.fd_out = fd;
  param.fd_err = fd;

  bench_header(fd, bench);

  started = bench_uptime_ms();
  pid = exec_builtin(bench->argv[0], bench->argv, &param);
  if (pid < 0)
    {
      dprintf(fd, "# %s did not start: %d\n", bench->name, pid);
      printf("could not start (%d)\n", pid);
      return -1;
    }

  if (waitpid(pid, &status, 0) < 0)
    {
      printf("lost track of it (%d)\n", errno);
      return -1;
    }

  printf("%.1f s\n", (bench_uptime_ms() - started) / 1000.0);
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  /* An argument beginning with a slash says where the results go; any
   * other argument names a benchmark to run.  Re-running one of them on
   * the device is how a model change gets judged against the same binary,
   * and waiting three minutes for ubench to say nothing new is how that
   * stops happening.
   */

  FAR const char *path = CONFIG_SYSTEM_BENCH_OUTPUT;
  int count = sizeof(g_benchmarks) / sizeof(g_benchmarks[0]);
  int selected = 0;
  bool tofile = true;
  uint32_t started;
  int failures = 0;
  int fd;
  int a;

  for (a = 1; a < argc; a++)
    {
      if (argv[a][0] == '/')
        {
          path = argv[a];
        }
      else
        {
          selected++;
        }
    }

  fd = bench_card_open(path);
  tofile = fd != STDOUT_FILENO;

  bench_memory(fd);

  started = bench_uptime_ms();
  for (int i = 0; i < count; i++)
    {
      if (selected > 0)
        {
          int want = 0;

          for (a = 1; a < argc; a++)
            {
              if (argv[a][0] != '/' &&
                  strcmp(argv[a], g_benchmarks[i].name) == 0)
                {
                  want = 1;
                }
            }

          if (!want)
            {
              continue;
            }
        }

      printf("[%d/%d] %s ", i + 1, count, g_benchmarks[i].name);
      fflush(stdout);

      if (bench_run(fd, &g_benchmarks[i]) < 0)
        {
          failures++;
        }
    }

  dprintf(fd, "# %d of %d benchmarks ran, in %" PRIu32 " seconds\n",
          count - failures, count, (bench_uptime_ms() - started) / 1000);

  if (!tofile)
    {
      return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

  printf("bench: %d of %d ran, %" PRIu32 " s total\n", count - failures,
         count, (bench_uptime_ms() - started) / 1000);
  bench_card_close(fd, path);

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
