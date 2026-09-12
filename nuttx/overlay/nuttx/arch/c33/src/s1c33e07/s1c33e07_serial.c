/****************************************************************************
 * arch/c33/src/s1c33e07/s1c33e07_serial.c
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
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include <nuttx/serial/serial.h>
#include "hardware/s1c33e07.h"
#include "c33_internal.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

static int c33_setup(struct uart_dev_s *dev)
{
  uint32_t divisor = (CONFIG_S1C33E07_MCLK + CONFIG_UART0_BAUD * 8) /
                      (CONFIG_UART0_BAUD * 16) - 1;

  putreg8(0xcb, S1C33_UART0_CTL);
  putreg8(0x10, S1C33_REGBASE + 0xb04);
  putreg8(0, S1C33_REGBASE + 0xb05);
  putreg8(divisor >> 8, S1C33_REGBASE + 0xb07);
  putreg8(divisor, S1C33_REGBASE + 0xb06);
  putreg8(1, S1C33_REGBASE + 0xb05);
  putreg8(7, S1C33_INT_FSIF01);
  return OK;
}

static void c33_shutdown(struct uart_dev_s *dev)
{
  modifyreg8(S1C33_INT_ESIF01, 7, 0);
}

static int c33_serial_interrupt(int irq, void *context, void *arg)
{
  struct uart_dev_s *dev = arg;

  putreg8(7, S1C33_INT_FSIF01);
  uart_recvchars(dev);
  uart_xmitchars(dev);
  putreg8(0, S1C33_UART0_STATUS);
  return OK;
}

static int c33_attach(struct uart_dev_s *dev)
{
  irq_attach(C33_IRQ_UART0RX, c33_serial_interrupt, dev);
  irq_attach(C33_IRQ_UART0TX, c33_serial_interrupt, dev);
  irq_attach(C33_IRQ_UART0ERR, c33_serial_interrupt, dev);
  modifyreg8(S1C33_INT_PSI01, 0x70, 0x50);
  return OK;
}

static void c33_detach(struct uart_dev_s *dev)
{
  c33_shutdown(dev);
  irq_detach(C33_IRQ_UART0RX);
  irq_detach(C33_IRQ_UART0TX);
  irq_detach(C33_IRQ_UART0ERR);
}

static int c33_ioctl(struct file *filep, int cmd, unsigned long arg)
{
  return -ENOTTY;
}

static int c33_receive(struct uart_dev_s *dev, unsigned int *status)
{
  *status = getreg8(S1C33_UART0_STATUS);
  return getreg8(S1C33_UART0_RXD);
}

static void c33_rxint(struct uart_dev_s *dev, bool enable)
{
  irqstate_t flags = up_irq_save();

  modifyreg8(S1C33_INT_ESIF01, 3, enable ? 3 : 0);
  up_irq_restore(flags);
}

static bool c33_rxavailable(struct uart_dev_s *dev)
{
  return (getreg8(S1C33_UART0_STATUS) & 1) != 0;
}

static void c33_send(struct uart_dev_s *dev, int ch)
{
  putreg8(ch, S1C33_UART0_TXD);
}

static void c33_txint(struct uart_dev_s *dev, bool enable)
{
  irqstate_t flags = up_irq_save();

  modifyreg8(S1C33_INT_ESIF01, 4, enable ? 4 : 0);
  if (enable)
    {
      uart_xmitchars(dev);
    }

  up_irq_restore(flags);
}

static bool c33_txready(struct uart_dev_s *dev)
{
  return (getreg8(S1C33_UART0_STATUS) & 2) != 0;
}

static bool c33_txempty(struct uart_dev_s *dev)
{
  return c33_txready(dev) && (getreg8(S1C33_UART0_STATUS) & 0x20) == 0;
}

static const struct uart_ops_s g_uart_ops =
{
  .setup       = c33_setup,
  .shutdown    = c33_shutdown,
  .attach      = c33_attach,
  .detach      = c33_detach,
  .ioctl       = c33_ioctl,
  .receive     = c33_receive,
  .rxint       = c33_rxint,
  .rxavailable = c33_rxavailable,
  .send        = c33_send,
  .txint       = c33_txint,
  .txready     = c33_txready,
  .txempty     = c33_txempty
};

static char g_rxbuffer[CONFIG_UART0_RXBUFSIZE];
static char g_txbuffer[CONFIG_UART0_TXBUFSIZE];
static struct uart_dev_s g_uart0 =
{
  .isconsole = true,
  .recv =
    {
      .size = sizeof(g_rxbuffer),
      .buffer = g_rxbuffer
    },

  .xmit =
    {
      .size = sizeof(g_txbuffer),
      .buffer = g_txbuffer
    },

  .ops = &g_uart_ops
};

void c33_serialinit(void)
{
  uart_register("/dev/console", &g_uart0);
  uart_register("/dev/ttyS0", &g_uart0);
}

void c33_lowsetup(void)
{
  /* The card loader supplies clock, pin mux, and SDRAM initialization. */

  putreg16(0x96, S1C33_WD_WP);
  putreg16(0, S1C33_WD_EN);
  putreg16(0, S1C33_WD_WP);
  c33_setup(&g_uart0);
}

void up_putc(int ch)
{
  while (!c33_txready(&g_uart0))
    {
    }

  c33_send(&g_uart0, ch);
}

void up_nputs(const char *buffer, size_t len)
{
  size_t i;

  for (i = 0; i < len; i++)
    {
      if (buffer[i] == '\n')
        {
          up_putc('\r');
        }

      up_putc(buffer[i]);
    }
}
