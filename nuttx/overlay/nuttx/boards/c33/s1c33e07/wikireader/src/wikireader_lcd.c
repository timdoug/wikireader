/****************************************************************************
 * boards/c33/s1c33e07/wikireader/src/wikireader_lcd.c
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
#include <stdbool.h>
#include <string.h>
#include <nuttx/arch.h>
#include <nuttx/video/fb.h>
#include "s1c33e07.h"
#include "wikireader.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static bool g_initialized;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int wr_getvideo(struct fb_vtable_s *vtable,
                       struct fb_videoinfo_s *info)
{
  memset(info, 0, sizeof(*info));
  info->fmt = FB_FMT_Y1;
  info->xres = WR_WIDTH;
  info->yres = WR_HEIGHT;
  info->nplanes = 1;
  return OK;
}

static int wr_getplane(struct fb_vtable_s *vtable, int plane,
                       struct fb_planeinfo_s *info)
{
  if (plane != 0)
    {
      return -EINVAL;
    }

  memset(info, 0, sizeof(*info));
  info->fbmem = (void *)WR_FBADDR;
  info->fblen = WR_STRIDE * WR_HEIGHT;
  info->stride = WR_STRIDE;
  info->bpp = 1;
  info->xres_virtual = WR_WIDTH;
  info->yres_virtual = WR_HEIGHT;
  return OK;
}

static struct fb_vtable_s g_fb =
{
  .getvideoinfo = wr_getvideo,
  .getplaneinfo = wr_getplane
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int up_fbinitialize(int display)
{
  if (display != 0)
    {
      return -EINVAL;
    }

  if (g_initialized)
    {
      return OK;
    }

  /* S1C33E07 LCDC, monochrome STN, four data pins.  The panel consumes
   * 240 pixels from a padded 256-pixel scanline.  Its contrast supply and
   * clock divider remain as configured by the card loader.
   */

  modifyreg8(S1C33_P3_DATA, 1, 0);
  modifyreg8(S1C33_P3_DIR, 0, 1);
  putreg8(0x55, S1C33_P8_FUNC03);
  putreg8(0x55, S1C33_P9_FUNC47);
  putreg32(0x96, S1C33_CMU_PROTECT);
  modifyreg32(S1C33_CMU_GATE0, 0, 0x27);
  putreg32(0, S1C33_CMU_PROTECT);
  putreg32(0, S1C33_LCD_POWER);
  putreg32(0, S1C33_LCD_INT);
  putreg32((39 << 16) | 31, S1C33_LCD_HD);
  putreg32((208 << 16) | 207, S1C33_LCD_VD);
  putreg32(0, S1C33_LCD_MR);
  putreg32((1 << 29) | (1 << 4), S1C33_LCD_MODE);
  putreg32(0, S1C33_LCD_PIP);
  memset((void *)WR_FBADDR, 0, WR_STRIDE * WR_HEIGHT);
  putreg32(WR_FBADDR, S1C33_LCD_MADDR);
  putreg32(WR_STRIDE / 4, S1C33_LCD_STRIDE);
  putreg32(3, S1C33_LCD_POWER);
  up_udelay(1000);
  modifyreg8(S1C33_P3_DATA, 0, 1);
  g_initialized = true;
  return OK;
}

struct fb_vtable_s *up_fbgetvplane(int display, int plane)
{
  return display == 0 && plane == 0 ? &g_fb : NULL;
}

void up_fbuninitialize(int display)
{
  if (display == 0)
    {
      modifyreg8(S1C33_P3_DATA, 1, 0);
      putreg32(0, S1C33_LCD_POWER);
      g_initialized = false;
    }
}
