/****************************************************************************
 * graphics/nxglib/fb/nxglib_lowres.h
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

#ifndef __GRAPHICS_NXGLIB_FB_NXGLIB_LOWRES_H
#define __GRAPHICS_NXGLIB_FB_NXGLIB_LOWRES_H

/****************************************************************************
 * Inline Functions
 ****************************************************************************/

static inline unsigned int nxgl_lowresget(FAR const uint8_t *line,
                                          unsigned int x)
{
  unsigned int shift = NXGL_REMAINDERX(x) * NXGLIB_BITSPERPIXEL;

#ifdef CONFIG_NX_PACKEDMSFIRST
  shift = 8 - NXGLIB_BITSPERPIXEL - shift;
#endif
  return (line[NXGL_SCALEX(x)] >> shift) &
         ((1 << NXGLIB_BITSPERPIXEL) - 1);
}

static inline void nxgl_lowresput(FAR uint8_t *line, unsigned int x,
                                  unsigned int pixel)
{
  unsigned int shift = NXGL_REMAINDERX(x) * NXGLIB_BITSPERPIXEL;
  uint8_t mask;

#ifdef CONFIG_NX_PACKEDMSFIRST
  shift = 8 - NXGLIB_BITSPERPIXEL - shift;
#endif
  mask = ((1 << NXGLIB_BITSPERPIXEL) - 1) << shift;
  line += NXGL_SCALEX(x);
  *line = (*line & ~mask) | (pixel << shift);
}

#endif /* __GRAPHICS_NXGLIB_FB_NXGLIB_LOWRES_H */
