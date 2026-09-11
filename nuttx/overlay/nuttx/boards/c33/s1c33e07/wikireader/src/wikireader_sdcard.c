/****************************************************************************
 * boards/c33/s1c33e07/wikireader/src/wikireader_sdcard.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/fs/fs.h>
#include <nuttx/fs/partition.h>
#include <nuttx/mmcsd.h>
#include <nuttx/spi/spi.h>

#include "hardware/s1c33e07.h"
#include "s1c33e07_spi.h"
#include "wikireader.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Chip selects on port 5, both active low. */

#define WR_CS_SDCARD    (1 << 0)
#define WR_CS_EEPROM    (1 << 2)
#define WR_CS_ALL       (WR_CS_SDCARD | WR_CS_EEPROM | (1 << 1))

/* Port 3 drives the card's supply through a buffer:
 *
 *   P32 = VCCEN, active low
 *   P33 = buffer enable, active high
 *
 * The two must not be raised together, hence the mask on every write.
 */

#define WR_SD_VCCEN     (1 << 2)
#define WR_SD_BUFEN     (1 << 3)
#define WR_SD_PWRMASK   (WR_SD_VCCEN | WR_SD_BUFEN)

#define WR_SD_BLOCKDEV  "/dev/mmcsd0"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: wikireader_sdpower
 *
 * Description:
 *   Bring the card's supply up in the order the hardware wants: rail off
 *   and buffer off, then the rail, then the buffer once the rail has
 *   settled.  Raising the buffer into an unpowered card back-feeds it
 *   through its protection diodes.
 *
 ****************************************************************************/

static void wikireader_sdpower(void)
{
  modifyreg8(S1C33_P3_DATA, WR_SD_PWRMASK, WR_SD_VCCEN);
  up_udelay(10);
  modifyreg8(S1C33_P3_DATA, WR_SD_PWRMASK, 0);
  up_udelay(1000);
  modifyreg8(S1C33_P3_DATA, WR_SD_PWRMASK, WR_SD_BUFEN);
}

/****************************************************************************
 * Name: wikireader_sdpins
 *
 * Description:
 *   Put the SPI pins and the two chip selects where the card expects them.
 *   The boot loader has already done this, but relying on that would make
 *   the driver's correctness depend on what ran before it.
 *
 ****************************************************************************/

static void wikireader_sdpins(void)
{
  /* P65 through P67 carry the SPI signals; P64 stays a GPIO. */

  putreg8(0x54, S1C33_P6_FUNC47);

  /* Both chip selects idle high before they become outputs, so nothing
   * sees a glitch.
   */

  putreg8(0x01, S1C33_P5_FUNC03);
  modifyreg8(S1C33_P5_DATA, 0, WR_CS_ALL);
  modifyreg8(S1C33_P5_DIR, 0, WR_CS_ALL);

  modifyreg8(S1C33_P3_DIR, 0, WR_SD_PWRMASK);
  modifyreg8(S1C33_P3_DATA, WR_SD_PWRMASK, WR_SD_VCCEN);
}

/****************************************************************************
 * Name: wikireader_partition
 *
 * Description:
 *   Register each partition the table describes as its own block device,
 *   named after the parent with the partition number appended.
 *
 ****************************************************************************/

static void wikireader_partition(struct partition_s *part, void *arg)
{
  char name[sizeof(WR_SD_BLOCKDEV) + 8];

  if (part->nblocks == 0)
    {
      return;
    }

  snprintf(name, sizeof(name), "%s%zu", WR_SD_BLOCKDEV, part->index + 1);
  if (register_blockpartition(name, 0, WR_SD_BLOCKDEV, part->firstblock,
                              part->nblocks) < 0)
    {
      syslog(LOG_ERR, "WikiReader: cannot register %s\n", name);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: wikireader_sdcard_initialize
 *
 * Description:
 *   Power the card, bring up the SPI bus, and mount the first FAT volume
 *   the partition table names.  A WikiReader card carries a small FAT32
 *   boot partition ahead of a much larger exFAT one that nothing here can
 *   read yet, so the boot partition is the one that gets mounted.
 *
 ****************************************************************************/

int wikireader_sdcard_initialize(void)
{
  struct spi_dev_s *spi;
  int ret;

  wikireader_sdpins();
  wikireader_sdpower();

  spi = s1c33e07_spibus_initialize(0);
  if (spi == NULL)
    {
      return -ENODEV;
    }

  ret = mmcsd_spislotinitialize(0, 0, spi);
  if (ret < 0)
    {
      return ret;
    }

  ret = parse_block_partition(WR_SD_BLOCKDEV, wikireader_partition, NULL);
  if (ret < 0)
    {
      /* An unpartitioned card is still worth trying to mount whole. */

      return nx_mount(WR_SD_BLOCKDEV, CONFIG_WIKIREADER_SDCARD_MOUNT,
                      "vfat", 0, NULL);
    }

  return nx_mount(WR_SD_BLOCKDEV "1", CONFIG_WIKIREADER_SDCARD_MOUNT,
                  "vfat", 0, NULL);
}

/****************************************************************************
 * Name: s1c33e07_spiselect
 *
 * Description:
 *   Assert or release the chip select the device is wired to.  The SPI
 *   driver has no idea which pin that is; the board does.
 *
 ****************************************************************************/

void s1c33e07_spiselect(struct spi_dev_s *dev, uint32_t devid, bool selected)
{
  uint8_t cs;

  switch (SPIDEVID_TYPE(devid))
    {
      case SPIDEVTYPE_MMCSD:
        cs = WR_CS_SDCARD;
        break;

      case SPIDEVTYPE_FLASH:
        cs = WR_CS_EEPROM;
        break;

      default:
        return;
    }

  modifyreg8(S1C33_P5_DATA, selected ? cs : 0, selected ? 0 : cs);
}

/****************************************************************************
 * Name: s1c33e07_spistatus
 *
 * Description:
 *   Report the card as present.  The slot has no detect or write-protect
 *   switch wired to a pin that can be read back, so there is nothing else
 *   truthful to say: a missing card shows up as an identification timeout.
 *
 ****************************************************************************/

uint8_t s1c33e07_spistatus(struct spi_dev_s *dev, uint32_t devid)
{
  return SPIDEVID_TYPE(devid) == SPIDEVTYPE_MMCSD ? SPI_STATUS_PRESENT : 0;
}
