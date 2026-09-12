/****************************************************************************
 * boards/c33/s1c33e07/wikireader/src/wikireader_touch.c
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
#include <stdint.h>
#include <string.h>
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/kthread.h>
#include <nuttx/semaphore.h>
#include <nuttx/input/touchscreen.h>
#include "s1c33e07.h"
#include "wikireader.h"

/****************************************************************************
 * Preprocessor Definitions
 ****************************************************************************/

#define WR_TOUCH_QUEUE 16
/* What the panel talks at.  CTP_BPS is 38400 in samo-lib/include/samo.h and
 * 9600 in samo-lib/include/boards/samo_a1.h, which is the board this is --
 * the board header is included first and wins.  Against the 60 MHz system
 * clock the divisor works out at 390, which is what grifo writes into the
 * same register.
 */

#define WR_TOUCH_BAUD  9600
#define WR_TOUCH_IRQS  0x30

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct touch_lowerhalf_s g_touch =
{
  .maxpoint = 1,
  .xres = WR_WIDTH,
  .yres = WR_HEIGHT
};

static sem_t g_samples = SEM_INITIALIZER(0);
static struct touch_sample_s g_queue[WR_TOUCH_QUEUE];
static uint8_t g_head;
static uint8_t g_tail;
static uint8_t g_state;
static uint8_t g_id;
static uint16_t g_x;
static uint16_t g_y;
static int16_t g_lastx;
static int16_t g_lasty;
static bool g_pressed;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void wr_touch_report(bool pressed, bool valid)
{
  struct touch_sample_s sample;
  uint8_t next = (g_tail + 1) % WR_TOUCH_QUEUE;
  bool empty = g_head == g_tail;
  int x = g_x >> 1;
  int y = g_y >> 1;

  /* Preserve a release even if its coordinate bytes are invalid. */

  if (x >= WR_WIDTH || y >= WR_HEIGHT)
    {
      if (pressed || !g_pressed)
        {
          return;
        }

      x = g_lastx;
      y = g_lasty;
      valid = false;
    }

  if (!pressed && !g_pressed)
    {
      return;
    }

  if (next == g_head)
    {
      /* Repeated motion may be dropped.  Keep an UP event so an overloaded
       * consumer cannot leave a key permanently depressed.
       */

      if (pressed)
        {
          return;
        }

      g_head = (g_head + 1) % WR_TOUCH_QUEUE;
      valid = false;
    }

  memset(&sample, 0, sizeof(sample));
  sample.npoints = 1;
  sample.point[0].id = g_id;
  sample.point[0].x = x;
  sample.point[0].y = y;
  sample.point[0].flags = (valid ? TOUCH_POS_VALID : 0) | TOUCH_ID_VALID |
                        (pressed ? (g_pressed ? TOUCH_MOVE : TOUCH_DOWN) :
                         TOUCH_UP);
  sample.point[0].timestamp = touch_get_time();
  g_queue[g_tail] = sample;
  g_tail = next;
  g_pressed = pressed;
  g_lastx = x;
  g_lasty = y;
  if (!pressed)
    {
      g_id++;
    }

  if (empty)
    {
      nxsem_post(&g_samples);
    }
}

static void wr_touch_byte(uint8_t byte)
{
  /* 0xaa starts a packet; coordinates are pairs of seven-bit bytes.
   * 0xff means an unchanged coordinate component.  State survives across
   * interrupts, because the hardware FIFO holds only four of six bytes.
   */

  if (byte == 0xaa)
    {
      g_state = 1;
      return;
    }

  if ((byte & 0x80) != 0 && byte != 0xff)
    {
      g_state = 0;
      return;
    }

  switch (g_state++)
    {
      case 1:
        if (byte != 0xff)
          {
            g_x = (g_x & 0x7f) | ((uint16_t)byte << 7);
          }
        break;

      case 2:
        if (byte != 0xff)
          {
            g_x = (g_x & 0x3f80) | byte;
          }
        break;

      case 3:
        if (byte != 0xff)
          {
            g_y = (g_y & 0x7f) | ((uint16_t)byte << 7);
          }
        break;

      case 4:
        if (byte != 0xff)
          {
            g_y = (g_y & 0x3f80) | byte;
          }
        break;

      case 5:
        if (byte <= 1)
          {
            wr_touch_report(byte != 0, true);
          }

        g_state = 0;
        break;

      default:
        g_state = 0;
        break;
    }
}

static int wr_touch_interrupt(int irq, void *context, void *arg)
{
  uint8_t status = getreg8(S1C33_UART1_STATUS);

  putreg8(WR_TOUCH_IRQS, S1C33_INT_FSIF01);
  if ((status & 0x1c) != 0)
    {
      /* A framing/parity/overrun error invalidates the partial packet.
       * Release without valid coordinates cancels a pending key selection.
       */

      g_state = 0;
      while ((getreg8(S1C33_UART1_STATUS) & 1) != 0)
        {
          getreg8(S1C33_UART1_RXD);
        }

      if (g_pressed)
        {
          wr_touch_report(false, false);
        }
    }
  else
    {
      while ((getreg8(S1C33_UART1_STATUS) & 1) != 0)
        {
          wr_touch_byte(getreg8(S1C33_UART1_RXD));
        }
    }

  putreg8(0, S1C33_UART1_STATUS);
  return OK;
}

static int wr_touch_thread(int argc, char **argv)
{
  struct touch_sample_s sample;
  irqstate_t flags;

  for (; ; )
    {
      nxsem_wait_uninterruptible(&g_samples);
      for (; ; )
        {
          flags = up_irq_save();
          if (g_head == g_tail)
            {
              up_irq_restore(flags);
              break;
            }

          sample = g_queue[g_head];
          g_head = (g_head + 1) % WR_TOUCH_QUEUE;
          up_irq_restore(flags);

          /* The upper half takes a mutex, so publication must run in task
           * context.  The ISR only decodes and queues fixed-size samples.
           */

          touch_event(g_touch.priv, &sample);
        }
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int wikireader_touch_initialize(void)
{
  uint32_t divisor = (CONFIG_S1C33E07_MCLK + WR_TOUCH_BAUD * 8) /
                    (WR_TOUCH_BAUD * 16) - 1;
  int ret;

  ret = touch_register(&g_touch, "/dev/input0", WR_TOUCH_QUEUE);
  if (ret < 0)
    {
      return ret;
    }

  ret = irq_attach(C33_IRQ_UART1RX, wr_touch_interrupt, NULL);
  if (ret >= 0)
    {
      ret = irq_attach(C33_IRQ_UART1ERR, wr_touch_interrupt, NULL);
    }

  if (ret >= 0)
    {
      ret = kthread_create("wr_touch", 140, 4096, wr_touch_thread, NULL);
    }

  if (ret < 0)
    {
      irq_detach(C33_IRQ_UART1RX);
      irq_detach(C33_IRQ_UART1ERR);
      touch_unregister(&g_touch, "/dev/input0");
      return ret;
    }

  modifyreg8(S1C33_P0_FUNC47, 3, 1);
  putreg8(0x4b, S1C33_UART1_CTL);
  putreg8(0x10, S1C33_UART1_IRDA);
  putreg8(0, S1C33_UART1_BRT);
  putreg8(divisor >> 8, S1C33_UART1_BRTH);
  putreg8(divisor, S1C33_UART1_BRTL);
  putreg8(1, S1C33_UART1_BRT);
  modifyreg8(S1C33_P0_DIR, 0, 0x80);
  modifyreg8(S1C33_P0_DATA, 0, 0x80);
  up_udelay(20);
  modifyreg8(S1C33_P0_DATA, 0x80, 0);
  putreg8(0, S1C33_UART1_STATUS);
  putreg8(0x70, S1C33_INT_FSIF01);
  modifyreg8(S1C33_INT_PSI01, 7, 6);
  up_enable_irq(C33_IRQ_UART1ERR);
  up_enable_irq(C33_IRQ_UART1RX);
  return OK;
}
