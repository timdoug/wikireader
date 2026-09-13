/****************************************************************************
 * apps/system/bench/cardb_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/* The card, at four block sizes.
 *
 * sdbench with one block size says how fast the card is and nothing about
 * why, which is no use for a model: the emulator's four card parameters --
 * the latency before a read, the latency before the first one, the gap
 * between reads, the latency before a write -- are all still zero, because
 * one number cannot be divided among four unknowns.
 *
 * Several block sizes can. The time a transfer takes is a fixed cost per
 * operation plus a cost per byte, so measuring 512 bytes against 32768
 * separates them: the small blocks are nearly all latency and the large
 * ones nearly all transfer. Two straight lines, one for reading and one for
 * writing, and their intercepts and slopes are the four numbers.
 *
 * It writes to the card it is measuring, which is unavoidable and is what
 * sdbench does anyway; the results file is opened first and closed last.
 */

#include <nuttx/config.h>

#include <sys/wait.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bench_card.h"
#include "builtin/builtin.h"
#include "nshlib/nshlib.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Two runs at each size: enough to see whether a number is repeatable
 * without spending a minute proving it.
 */

static FAR char *const g_b512[]   =
{
  "sdbench", "-b", "512", "-r", "2", "-d", "600", NULL
};

static FAR char *const g_b2048[]  =
{
  "sdbench", "-b", "2048", "-r", "2", "-d", "600", NULL
};

static FAR char *const g_b8192[]  =
{
  "sdbench", "-b", "8192", "-r", "2", "-d", "600", NULL
};

static FAR char *const g_b32768[] =
{
  "sdbench", "-b", "32768", "-r", "2", "-d", "600", NULL
};

static FAR char *const *const g_sweep[] =
{
  g_b512, g_b2048, g_b8192, g_b32768
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int cardb_run(int fd, FAR char *const *argv)
{
  struct nsh_param_s param;
  int status;
  pid_t pid;

  /* The child writes into this process's descriptor; see bench_main.c on
   * why it is not given the path.
   */

  memset(&param, 0, sizeof(param));
  param.fd_in  = -1;
  param.fd_out = fd;
  param.fd_err = fd;

  dprintf(fd, "# command: %s %s %s\n", argv[0], argv[1], argv[2]);

  pid = exec_builtin(argv[0], argv, &param);
  if (pid < 0)
    {
      dprintf(fd, "# did not start: %d\n", pid);
      return -1;
    }

  return waitpid(pid, &status, 0) < 0 ? -1 : 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR const char *path = argc > 1 ? argv[1] :
                         CONFIG_SYSTEM_BENCH_MOUNT "/cardb.txt";
  int count = sizeof(g_sweep) / sizeof(g_sweep[0]);
  int failures = 0;
  int fd;
  int i;

  fd = bench_card_open(path);

  dprintf(fd, "# the card at four block sizes, for the latency and the rate\n");

  for (i = 0; i < count; i++)
    {
      printf("[%d/%d] %s bytes ", i + 1, count, g_sweep[i][2]);
      fflush(stdout);

      if (cardb_run(fd, g_sweep[i]) < 0)
        {
          failures++;
          printf("failed\n");
        }
      else
        {
          printf("done\n");
        }
    }

  bench_card_close(fd, path);
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
