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

static void board_progress(int stage)
{
  FAR volatile uint8_t *fb = (FAR volatile uint8_t *)WR_FBADDR;
  int row;

  /* A byte is eight pixels; every other byte, so they can be counted. */

  for (row = 0; row < 8; row++)
    {
      fb[row * WR_STRIDE + stage * 2] = 0xff;
    }
}
#else
#  define board_progress(stage)
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
  ret = wikireader_sdcard_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "WikiReader: no card mounted: %d\n", ret);
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
