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

/* Put the card back if the last command took it away.
 *
 * Every one of these commands unmounts when it has written its results, so
 * that the card can be pulled out as soon as the screen says so -- which
 * means the second command of a session finds no filesystem and cannot
 * write anything at all. That is what ubench followed by cardb did: four
 * runs of "Can't open benchmark file /sd/sd_bench", because ubench had
 * already unmounted it.
 *
 * The partition the board mounts at boot is the first one, with the whole
 * card as the fallback for a card with no partition table, and this tries
 * them in the same order.
 */

static void bench_card_mount(void)
{
  if (mount("/dev/mmcsd01", CONFIG_SYSTEM_BENCH_MOUNT, "vfat", 0, NULL) == 0 ||
      mount("/dev/mmcsd0", CONFIG_SYSTEM_BENCH_MOUNT, "vfat", 0, NULL) == 0)
    {
      printf("card remounted\n");
    }
}

int bench_card_open(const char *path)
{
  /* Appending, so that everything written to it keeps its order without
   * the writers having to know each other's offsets; truncating as well,
   * so that a second run is a second file rather than two.
   */

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);

  if (fd < 0)
    {
      bench_card_mount();
      fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    }

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
