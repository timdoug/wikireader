/****************************************************************************
 * apps/system/bench/bench_card.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/mount.h>
#include <sys/statfs.h>

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

/* What shape the filesystem is, which decides how much of the card a read
 * can ask for at once.
 *
 * fs_fat32.c clips a multi-sector read to the sectors left in the cluster,
 * so a volume with one sector per cluster never issues CMD18 and pays a
 * command, a response and a token poll for every 512 bytes; the same 2.9 MB
 * read takes 9.78 s at that geometry and 2.63 s at 32 KiB clusters. A
 * throughput number therefore says as much about the card's formatting as
 * about the card, and comparing one against an emulated card of a different
 * shape compares nothing. Record it next to the numbers.
 */

void bench_card_geometry(int fd)
{
  struct statfs buf;

  if (statfs(CONFIG_SYSTEM_BENCH_MOUNT, &buf) < 0)
    {
      dprintf(fd, "# geometry: statfs failed (%d)\n", errno);
      return;
    }

  dprintf(fd, "# geometry: type 0x%lx, block %ld, blocks %ld, free %ld\n",
          (unsigned long)buf.f_type, (long)buf.f_bsize,
          (long)buf.f_blocks, (long)buf.f_bfree);
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
