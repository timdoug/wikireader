/****************************************************************************
 * graphics/nxterm/nxterm_batch.c
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
#include <string.h>

#include <nuttx/kmalloc.h>
#include <nuttx/video/fb.h>

#include "nxterm.h"
#include "../nxglib/nxglib.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct nxterm_batch_s
{
  FAR const struct nxterm_operations_s *ops;
  struct fb_planeinfo_s plane;
  struct nxgl_rect_s bounds;
  struct nxgl_rect_s dirty;
  bool modified;
  uint8_t bitmap[];
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void nxterm_batch_dirty(FAR struct nxterm_batch_s *batch,
                               FAR const struct nxgl_rect_s *rect)
{
  if (batch->modified)
    {
      nxgl_rectunion(&batch->dirty, &batch->dirty, rect);
    }
  else
    {
      batch->dirty = *rect;
      batch->modified = true;
    }
}

static int nxterm_batch_fill(FAR struct nxterm_state_s *priv,
                             FAR const struct nxgl_rect_s *rect,
                             nxgl_mxpixel_t color[CONFIG_NX_NPLANES])
{
  FAR struct nxterm_batch_s *batch = priv->batch;
  struct nxgl_rect_s clipped;

  nxgl_rectintersect(&clipped, rect, &batch->bounds);
  if (!nxgl_nullrect(&clipped))
    {
      nxgl_fillrectangle_1bpp(&batch->plane, &clipped, color[0]);
      nxterm_batch_dirty(batch, &clipped);
    }

  return OK;
}

#ifndef CONFIG_NX_WRITEONLY
static int nxterm_batch_move(FAR struct nxterm_state_s *priv,
                             FAR const struct nxgl_rect_s *rect,
                             FAR const struct nxgl_point_s *offset)
{
  FAR struct nxterm_batch_s *batch = priv->batch;
  struct nxgl_rect_s source;
  struct nxgl_rect_s dest;

  /* Clip both ends of the move before accessing the private bitmap. */

  nxgl_rectintersect(&source, rect, &batch->bounds);
  if (!nxgl_nullrect(&source))
    {
      nxgl_rectoffset(&dest, &source, offset->x, offset->y);
      nxgl_rectintersect(&dest, &dest, &batch->bounds);
      if (!nxgl_nullrect(&dest))
        {
          nxgl_rectoffset(&source, &dest, -offset->x, -offset->y);
          nxgl_moverectangle_1bpp(&batch->plane, &source, &dest.pt1);
          nxterm_batch_dirty(batch, &dest);
        }
    }

  return OK;
}
#endif

static int nxterm_batch_bitmap(FAR struct nxterm_state_s *priv,
                               FAR const struct nxgl_rect_s *dest,
                               FAR const void *src[CONFIG_NX_NPLANES],
                               FAR const struct nxgl_point_s *origin,
                               unsigned int stride)
{
  FAR struct nxterm_batch_s *batch = priv->batch;
  struct nxgl_rect_s clipped;

  nxgl_rectintersect(&clipped, dest, &batch->bounds);
  if (!nxgl_nullrect(&clipped))
    {
      nxgl_copyrectangle_1bpp(&batch->plane, &clipped, src[0], origin,
                             stride);
      nxterm_batch_dirty(batch, &clipped);
    }

  return OK;
}

static const struct nxterm_operations_s g_batchops =
{
  nxterm_batch_fill,
#ifndef CONFIG_NX_WRITEONLY
  nxterm_batch_move,
#endif
  nxterm_batch_bitmap
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: nxterm_batch_resize
 *
 * Description:
 *   Allocate a private 1bpp bitmap, retaining existing pixels on resize.
 *   Leave the previous bitmap and operations intact on allocation failure.
 ****************************************************************************/

int nxterm_batch_resize(FAR struct nxterm_state_s *priv,
                        FAR const struct nxgl_size_s *size)
{
  FAR struct nxterm_batch_s *old = priv->batch;
  FAR struct nxterm_batch_s *batch;
  struct nxgl_rect_s overlap;
  struct nxgl_point_s origin;
  size_t stride;
  size_t bytes;

  if (size->w <= 0 || size->h <= 0)
    {
      return -EINVAL;
    }

  if (old != NULL && old->bounds.pt2.x == size->w - 1 &&
      old->bounds.pt2.y == size->h - 1)
    {
      return OK;
    }

  stride = ((size_t)size->w + 7) >> 3;
  bytes = stride * size->h;
  batch = kmm_zalloc(sizeof(*batch) + bytes);
  if (batch == NULL)
    {
      return -ENOMEM;
    }

  batch->ops = old == NULL ? priv->ops : old->ops;
  batch->plane.fbmem = batch->bitmap;
  batch->plane.fblen = bytes;
  batch->plane.stride = stride;
  batch->plane.bpp = 1;
  batch->bounds.pt2.x = size->w - 1;
  batch->bounds.pt2.y = size->h - 1;
  memset(batch->bitmap, (priv->wndo.wcolor[0] & 1) ? 0xff : 0, bytes);

  if (old != NULL)
    {
      origin.x = 0;
      origin.y = 0;
      nxgl_rectintersect(&overlap, &old->bounds, &batch->bounds);
      nxgl_copyrectangle_1bpp(&batch->plane, &overlap, old->bitmap, &origin,
                             old->plane.stride);
    }

  nxterm_batch_dirty(batch, &batch->bounds);
  priv->batch = batch;
  priv->ops = &g_batchops;
  kmm_free(old);
  return OK;
}

/****************************************************************************
 * Name: nxterm_batch_flush
 *
 * Description:
 *   Commit all drawing since the last flush with one NX bitmap operation.
 *   The original bitmap operation waits for the server to finish reading
 *   the source, so the locked buffer can then be reused safely.  Keep dirty
 *   pixels pending when submission fails.
 ****************************************************************************/

int nxterm_batch_flush(FAR struct nxterm_state_s *priv)
{
  FAR struct nxterm_batch_s *batch = priv->batch;
  FAR const void *source[CONFIG_NX_NPLANES];
  struct nxgl_point_s origin;
  int ret = OK;

  if (batch != NULL && batch->modified)
    {
      origin.x = 0;
      origin.y = 0;
      source[0] = batch->bitmap;
      ret = batch->ops->bitmap(priv, &batch->dirty, source, &origin,
                               batch->plane.stride);
      if (ret >= 0)
        {
          batch->modified = false;
        }
    }

  return ret;
}

/****************************************************************************
 * Name: nxterm_batch_free
 ****************************************************************************/

void nxterm_batch_free(FAR struct nxterm_state_s *priv)
{
  if (priv->batch != NULL)
    {
      priv->ops = priv->batch->ops;
      kmm_free(priv->batch);
      priv->batch = NULL;
    }
}
