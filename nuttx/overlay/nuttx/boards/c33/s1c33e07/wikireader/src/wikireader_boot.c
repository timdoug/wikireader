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

void board_late_initialize(void)
{
  board_app_initialize(0);
#ifdef CONFIG_WIKIREADER_LCD
  if (fb_register(0, 0) < 0)
    {
      syslog(LOG_ERR, "WikiReader: framebuffer initialization failed\n");
    }

#endif
#ifdef CONFIG_WIKIREADER_TOUCH
  if (wikireader_touch_initialize() < 0)
    {
      syslog(LOG_ERR, "WikiReader: touch initialization failed\n");
    }
#endif
}
