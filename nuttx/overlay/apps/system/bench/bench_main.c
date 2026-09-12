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
  FAR const char *path = argc > 1 ? argv[1] : CONFIG_SYSTEM_BENCH_OUTPUT;
  int count = sizeof(g_benchmarks) / sizeof(g_benchmarks[0]);
  bool tofile = true;
  uint32_t started;
  int failures = 0;
  int fd;

  /* Appending, so that this process and the benchmarks it starts can write
   * to the file without keeping each other's offsets in mind; truncating
   * here as well, so that a second run is a second file rather than two.
   */

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
  if (fd < 0)
    {
      /* Better to print the numbers where they can at least be read than to
       * refuse to measure anything.
       */

      printf("bench: cannot write %s (%d); results follow instead\n",
             path, errno);
      fd = STDOUT_FILENO;
      tofile = false;
    }
  else
    {
      printf("bench: writing %s\n", path);
    }

  started = bench_uptime_ms();
  for (int i = 0; i < count; i++)
    {
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

  /* Get it onto the card and then take the filesystem down, so that pulling
   * the card is safe the moment this says it is.
   */

  fsync(fd);
  close(fd);
  sync();

  printf("bench: %d of %d ran, %" PRIu32 " s total\n", count - failures,
         count, (bench_uptime_ms() - started) / 1000);

  if (umount(CONFIG_SYSTEM_BENCH_MOUNT) < 0)
    {
      printf("bench: %s is written but %s would not unmount (%d): "
             "poweroff before removing the card\n", path,
             CONFIG_SYSTEM_BENCH_MOUNT, errno);
    }
  else
    {
      printf("bench: %s written, card unmounted -- safe to remove\n", path);
    }

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
