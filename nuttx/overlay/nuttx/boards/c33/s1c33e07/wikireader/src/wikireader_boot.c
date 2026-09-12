/****************************************************************************
 * boards/c33/s1c33e07/wikireader/src/wikireader_boot.c
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
#include <stdint.h>
#include <nuttx/board.h>
#include <nuttx/fs/fs.h>
#ifdef CONFIG_WIKIREADER_LCD
#  include <nuttx/video/fb.h>
#endif
#include <syslog.h>
#include "wikireader.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int board_app_initialize(uintptr_t arg)
{
#ifdef CONFIG_FS_TMPFS
  int ret = nx_mount(NULL, "/tmp", "tmpfs", 0, NULL);
  if (ret < 0)
    {
      return ret;
    }
#endif
#ifdef CONFIG_FS_PROCFS
  return nx_mount(NULL, "/proc", "procfs", 0, NULL);
#else
  return OK;
#endif
}

#ifdef CONFIG_WIKIREADER_BOOT_PROGRESS

/****************************************************************************
 * Name: board_progress
 *
 * Description:
 *   One black square along the top of the panel per stage of bringing the
 *   board up, written straight into the framebuffer because none of the
 *   machinery that could say this in words exists yet: the terminal is
 *   started by what this function is reporting on, and the serial console
 *   most of these devices have nothing attached to.
 *
 *   The terminal overwrites the row as soon as it starts, so the squares
 *   are visible for about as long as they are useful.  If the machine stops
 *   during startup, how many of them there are says where.
 *
 ****************************************************************************/

void wikireader_progress(int row, int slot)
{
  FAR volatile uint8_t *fb = (FAR volatile uint8_t *)WR_FBADDR;
  int y;

  /* A byte is eight pixels; every other byte, so they can be counted.  Ten
   * rows apart, so the second line of them is plainly a second line.
   */

  for (y = row * 10; y < row * 10 + 8; y++)
    {
      fb[y * WR_STRIDE + slot * 2] = 0xff;
    }
}

#define board_progress(stage) wikireader_progress(0, (stage))
#else
#  define board_progress(stage)
#endif

#ifdef CONFIG_BOARD_CRASHDUMP_CUSTOM

/****************************************************************************
 * Name: board_crashdump
 *
 * Description:
 *   Everything that stops this board during startup looks the same from the
 *   outside: the panel keeps whatever was on it and nothing else happens.
 *   A machine that stopped because it faulted and one that stopped because
 *   it is waiting for a peripheral that will never answer want different
 *   questions asked of them, so the one that faulted says so -- a solid bar
 *   across the panel, written with nothing but stores to memory, which is
 *   all that can be trusted from here.
 *
 ****************************************************************************/

void board_crashdump(uintptr_t sp, FAR struct tcb_s *tcb,
                     FAR const char *filename, int lineno,
                     FAR const char *msg, FAR void *regs)
{
  FAR volatile uint8_t *fb = (FAR volatile uint8_t *)WR_FBADDR;
  int i;

  for (i = 20 * WR_STRIDE; i < 28 * WR_STRIDE; i++)
    {
      fb[i] = 0xff;
    }
}
#endif

void board_late_initialize(void)
{
#ifdef CONFIG_WIKIREADER_SDCARD
  int ret;
#endif

  board_progress(0);
  board_app_initialize(0);
  board_progress(1);
#ifdef CONFIG_WIKIREADER_LCD
  if (fb_register(0, 0) < 0)
    {
      syslog(LOG_ERR, "WikiReader: framebuffer initialization failed\n");
    }

  board_progress(2);
#endif
#ifdef CONFIG_WIKIREADER_SDCARD
  /* On its own thread, and waited for with a deadline.  Whatever the card
   * does, the terminal comes up: a machine that answers questions about why
   * it has no card is worth more than one that is still trying to find out.
   */

  ret = wikireader_sdcard_start();
  if (ret < 0)
    {
      syslog(LOG_ERR, "WikiReader: card thread did not start: %d\n", ret);
    }

  board_progress(3);
#endif
#ifdef CONFIG_WIKIREADER_TOUCH
  if (wikireader_touch_initialize() < 0)
    {
      syslog(LOG_ERR, "WikiReader: touch initialization failed\n");
    }

  board_progress(4);
#endif
}
