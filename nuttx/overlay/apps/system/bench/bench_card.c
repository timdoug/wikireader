/****************************************************************************
 * apps/system/bench/bench_card.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/mount.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "bench_card.h"

int bench_card_open(const char *path)
{
  /* Appending, so that everything written to it keeps its order without
   * the writers having to know each other's offsets; truncating as well,
   * so that a second run is a second file rather than two.
   */

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);

  if (fd < 0)
    {
      printf("%s cannot be written (%d); results follow instead\n",
             path, errno);
      return STDOUT_FILENO;
    }

  printf("writing %s\n", path);
  return fd;
}

void bench_card_close(int fd, const char *path)
{
  if (fd == STDOUT_FILENO)
    {
      return;
    }

  fsync(fd);
  close(fd);
  sync();

  if (umount(CONFIG_SYSTEM_BENCH_MOUNT) < 0)
    {
      printf("%s is written but %s would not unmount (%d): "
             "poweroff before removing the card\n", path,
             CONFIG_SYSTEM_BENCH_MOUNT, errno);
    }
  else
    {
      printf("%s written, card unmounted -- safe to remove\n", path);
    }
}
