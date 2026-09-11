/****************************************************************************
 * arch/c33/src/s1c33e07/s1c33e07_spi.c
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

#include <assert.h>
#include <debug.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/mutex.h>
#include <nuttx/spi/spi.h>

#include "hardware/s1c33e07.h"
#include "s1c33e07_spi.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* MCBR selects MCLK/4 through MCLK/512 in powers of two.  MCLK/4 is 12 MHz
 * with the 48 MHz master clock, comfortably inside the 25 MHz an SD card
 * accepts once it has left identification mode.
 */

#define SPI_MCBR_MAX    7
#define SPI_DIVIDER(n)  (4u << (n))

/* The intelligent DMA channel the receive-full event triggers, the table
 * entry it reads (one per channel, so the table is this long), and the
 * dummy word that follows the table and feeds the transmit register.
 */

/* The word of ones the transmit engine reads over and over lives in the
 * chip's descriptor RAM, past where the boot loader keeps its own control
 * blocks.  Internal RAM has no rows to change, unlike SDRAM, where every
 * read of it would have cost the receive buffer a row activation.
 */

#define SPI_DMA_DUMMY    0x250

/* Below this an engine setup costs more than the CPU would spend simply
 * doing the transfer.  A sector is 512 bytes; the card's registers are 16.
 */

#define SPI_DMA_MINIMUM  64

/* A 512-byte block at the slowest clock this driver selects still lands
 * inside 50 ms, so anything past that is a stall, not slow going.
 */

#define SPI_DMA_TIMEOUT  MSEC2TICK(50)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct s1c33e07_spidev_s
{
  struct spi_dev_s dev;         /* Externally visible part, must be first */
  mutex_t lock;                 /* Bus arbitration between MMC/SD and FLASH */
  uint32_t ctl1;                /* Shadow of CTL1: see spi_putctl1() */
  uint32_t actual;              /* Bit rate the divider in ctl1 produces */
  int nbits;                    /* Bits per transfer currently programmed */
  bool dma_off;                 /* Set for good by a DMA transfer that stalled */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int spi_lock(struct spi_dev_s *dev, bool lock);
static uint32_t spi_setfrequency(struct spi_dev_s *dev, uint32_t frequency);
static void spi_setmode(struct spi_dev_s *dev, enum spi_mode_e mode);
static void spi_setbits(struct spi_dev_s *dev, int nbits);
static uint32_t spi_send(struct spi_dev_s *dev, uint32_t wd);
static void spi_exchange(struct spi_dev_s *dev, const void *txbuffer,
                         void *rxbuffer, size_t nwords);
static void spi_wait_idle(void);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct spi_ops_s g_spiops =
{
  .lock         = spi_lock,
  .select       = s1c33e07_spiselect,
  .setfrequency = spi_setfrequency,
  .setmode      = spi_setmode,
  .setbits      = spi_setbits,
  .status       = s1c33e07_spistatus,
  .send         = spi_send,
  .exchange     = spi_exchange,
};

static struct s1c33e07_spidev_s g_spidev =
{
  .dev.ops = &g_spiops,
  .lock    = NXMUTEX_INITIALIZER,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: spi_putctl1
 *
 * Description:
 *   Write the control register, keeping the enable bit set.  Taking ENA
 *   low while a chip select is asserted costs the attached card a bit of
 *   whatever it is shifting out, so the driver only ever rewrites the
 *   register in place.
 *
 ****************************************************************************/

static void spi_wait_idle(void)
{
  while ((getreg32(S1C33_SPI_STAT) & SPI_STAT_BSYF) != 0)
    {
    }
}

/****************************************************************************
 * Name: spi_putctl1
 *
 * Description:
 *   Write the control register, keeping the enable bit set.  Everything but
 *   the character width can be changed in place; spi_setwidth() handles the
 *   one field that cannot.
 *
 ****************************************************************************/

static void spi_putctl1(struct s1c33e07_spidev_s *priv, uint32_t ctl1)
{
  /* TXDE and RXDE gate the two DMA request lines.  They cost nothing while
   * the engines are idle, and leaving them set means a transfer never has
   * to touch this register to arrange one.
   */

  priv->ctl1 = ctl1 | SPI_CTL1_TXDE | SPI_CTL1_RXDE |
               SPI_CTL1_MASTER | SPI_CTL1_ENA;

  /* CTL1 is undefined while a transfer is in flight (manual V.2.8). */

  spi_wait_idle();
  putreg32(priv->ctl1, S1C33_SPI_CTL1);
}

/****************************************************************************
 * Name: spi_transfer
 *
 * Description:
 *   Clock one word out and the reply in.  The transmit register holds one
 *   word behind the shift register, so wait for the shifter before writing
 *   or the word is dropped, and then for the receive-full flag, which
 *   reading RXD clears.
 *
 ****************************************************************************/

static uint32_t spi_transfer(uint32_t wd)
{
  spi_wait_idle();
  putreg32(wd, S1C33_SPI_TXD);

  while ((getreg32(S1C33_SPI_STAT) & SPI_STAT_RDFF) == 0)
    {
    }

  return getreg32(S1C33_SPI_RXD);
}

/****************************************************************************
 * Name: spi_lock
 ****************************************************************************/

static int spi_lock(struct spi_dev_s *dev, bool lock)
{
  struct s1c33e07_spidev_s *priv = (struct s1c33e07_spidev_s *)dev;

  return lock ? nxmutex_lock(&priv->lock) : nxmutex_unlock(&priv->lock);
}

/****************************************************************************
 * Name: spi_setfrequency
 ****************************************************************************/

static uint32_t spi_setfrequency(struct spi_dev_s *dev, uint32_t frequency)
{
  struct s1c33e07_spidev_s *priv = (struct s1c33e07_spidev_s *)dev;
  unsigned int mcbr;

  /* Pick the fastest divider that stays at or below what was asked for.
   * Identification mode wants 400 kHz or less, which MCLK/128 meets.
   * Nothing reaches the bottom of the table by accident: a card whose CSD
   * names a transfer speed the driver cannot parse asks for zero, and the
   * slowest clock is the right answer to that.
   */

  for (mcbr = 0; mcbr < SPI_MCBR_MAX; mcbr++)
    {
      if (CONFIG_S1C33E07_MCLK / SPI_DIVIDER(mcbr) <= frequency)
        {
          break;
        }
    }

  priv->actual = CONFIG_S1C33E07_MCLK / SPI_DIVIDER(mcbr);
  spi_putctl1(priv, (priv->ctl1 & ~SPI_CTL1_MCBR_MASK) |
                    (mcbr << SPI_CTL1_MCBR_SHIFT));

  spiinfo("frequency: %" PRIu32 " actual: %" PRIu32 "\n",
          frequency, priv->actual);
  return priv->actual;
}

/****************************************************************************
 * Name: spi_setmode
 ****************************************************************************/

static void spi_setmode(struct spi_dev_s *dev, enum spi_mode_e mode)
{
  struct s1c33e07_spidev_s *priv = (struct s1c33e07_spidev_s *)dev;
  uint32_t bits;

  switch (mode)
    {
      case SPIDEV_MODE0:
        bits = 0;
        break;

      case SPIDEV_MODE1:
        bits = SPI_CTL1_CPHA;
        break;

      case SPIDEV_MODE2:
        bits = SPI_CTL1_CPOL;
        break;

      case SPIDEV_MODE3:
        bits = SPI_CTL1_CPOL | SPI_CTL1_CPHA;
        break;

      default:
        DEBUGPANIC();
        return;
    }

  spi_putctl1(priv, (priv->ctl1 & ~(SPI_CTL1_CPOL | SPI_CTL1_CPHA)) | bits);
}

/****************************************************************************
 * Name: spi_setbits
 ****************************************************************************/

static void spi_setbits(struct spi_dev_s *dev, int nbits)
{
  struct s1c33e07_spidev_s *priv = (struct s1c33e07_spidev_s *)dev;

  /* BPT holds the count less one, so 1 through 32 bits are expressible.
   * Only whole bytes are useful to the block layer above.
   */

  DEBUGASSERT(nbits == 8 || nbits == 16 || nbits == 32);

  priv->nbits = nbits;
  spi_putctl1(priv, (priv->ctl1 & ~SPI_CTL1_BPT_MASK) |
                    ((nbits - 1) << SPI_CTL1_BPT_SHIFT));
}

/****************************************************************************
 * Name: spi_send
 ****************************************************************************/

static uint32_t spi_send(struct spi_dev_s *dev, uint32_t wd)
{
  return spi_transfer(wd);
}

/****************************************************************************
 * Name: spi_dma_stop
 ****************************************************************************/

static void spi_dma_stop(void)
{
  putreg16(0, S1C33_HS2_EN);
  putreg16(0, S1C33_HS3_EN);

  /* The boot loader drives the same card with an intelligent-DMA channel
   * instead of high-speed channel 2.  Silence it: it is armed against the
   * same receive event and would answer these transfers too.
   */

  modifyreg8(S1C33_IDMAEN_SPI, IDMA_SPI_ENABLE, 0);
  modifyreg8(S1C33_IDMAREQ_SPI, IDMA_SPI_ENABLE, 0);
  putreg8(0, S1C33_IDMA_EN);
}

/****************************************************************************
 * Name: spi_setwidth
 *
 * Description:
 *   Change the character width, which the controller only accepts across an
 *   enable cycle.  That cycle is not free here: a probe on this board found
 *   the card losing one bit of whatever it was shifting out each time the
 *   enable bit was taken low with the chip select asserted.  So park the
 *   clock pin as a GPIO held at the level CPOL names, change the width
 *   behind it, and hand the pin back once the new divider has settled.
 *   Interrupts stay off across the pin handover, since a half-switched pin
 *   is a clock edge at the card.
 *
 ****************************************************************************/

static void spi_setwidth(struct s1c33e07_spidev_s *priv, uint32_t ctl1)
{
  irqstate_t flags = up_irq_save();
  uint8_t mux  = getreg8(S1C33_P6_FUNC47);
  uint8_t dir  = getreg8(S1C33_P6_DIR);
  uint8_t data = getreg8(S1C33_P6_DATA);
  bool hold    = (mux & 0xc0) == 0x40;
  uint32_t interrupts = getreg32(S1C33_SPI_INT);
  unsigned settle;

  if (hold)
    {
      putreg8((data & ~0x80) | ((ctl1 & SPI_CTL1_CPOL) ? 0x80 : 0),
              S1C33_P6_DATA);
      putreg8(dir | 0x80, S1C33_P6_DIR);
      putreg8(mux & ~0xc0, S1C33_P6_FUNC47);
    }

  /* V.2.8 also wants the interrupt sources clear before the enable bit
   * goes low, whatever the controller's master enable says.
   */

  putreg32(0, S1C33_SPI_INT);
  modifyreg32(S1C33_SPI_CTL1, SPI_CTL1_ENA, 0);
  putreg32(ctl1 & ~SPI_CTL1_ENA, S1C33_SPI_CTL1);
  putreg32(ctl1, S1C33_SPI_CTL1);
  priv->ctl1 = ctl1;

  if (hold)
    {
      for (settle = SPI_DIVIDER((ctl1 >> SPI_CTL1_MCBR_SHIFT) & 7);
           settle > 0; settle--)
        {
          __asm__ volatile ("nop");
        }

      putreg8(mux, S1C33_P6_FUNC47);
      putreg8(dir, S1C33_P6_DIR);
      putreg8(data, S1C33_P6_DATA);
    }

  putreg32(interrupts, S1C33_SPI_INT);
  up_irq_restore(flags);
}

/****************************************************************************
 * Name: spi_unswap
 *
 * Description:
 *   A 32-bit character arrives most significant byte first, which is the
 *   opposite of how the word then sits in memory.  Reverse each word back
 *   into card order.
 *
 ****************************************************************************/

static void spi_unswap(uint8_t *buffer, size_t nbytes)
{
  uint32_t *words = (uint32_t *)buffer;

  for (; nbytes >= 4; nbytes -= 4)
    {
      uint32_t value = *words;

      __asm__ ("swap\t%0,%1" : "=r" (value) : "r" (value));
      *words++ = value;
    }
}

/****************************************************************************
 * Name: spi_recv_dma
 *
 * Description:
 *   Receive with the DMA engines rather than the CPU, and return how many
 *   bytes arrived.  High-speed channel 3 drains the receive register into
 *   the caller's buffer, and channel 2 refills the transmit register from a
 *   fixed word of ones; because the transmit register empties when the
 *   shifter takes its contents rather than when it finishes with them, the
 *   next word is already queued by the time the current one is on the wire.
 *   One CPU write starts the chain.
 *
 *   Characters are 32 bits wide here, which is the entire point: a sector
 *   costs 128 requests instead of 512, and the byte-at-a-time engine setup
 *   is dearer than simply letting the CPU do it.
 *
 *   Anything short of the whole buffer is the caller's to finish, and DMA
 *   stays off for the rest of the session: an engine that stopped early has
 *   already left the card mid-block, and guessing at why is worse than
 *   being slow.
 *
 ****************************************************************************/

static size_t spi_recv_dma(struct s1c33e07_spidev_s *priv, uint8_t *dest,
                           size_t nbytes)
{
  const uintptr_t dummy = S1C33_DSTRAM + SPI_DMA_DUMMY;
  uintptr_t buffer = (uintptr_t)dest;
  uint32_t narrow;
  clock_t start;
  size_t words;
  size_t drained;

  /* The engines write to SDRAM dependably and to the internal RAMs less
   * so, which is the boot loader's experience on this part as well.  Short
   * or misaligned runs are cheaper for the CPU to do itself.
   */

  if (priv->dma_off || nbytes < SPI_DMA_MINIMUM ||
      ((buffer | nbytes) & 3) != 0 || buffer < CONFIG_RAM_START)
    {
      return 0;
    }

  words = nbytes / 4;
  *(volatile uint32_t *)dummy = 0xffffffff;

  spi_wait_idle();
  narrow = priv->ctl1;
  spi_setwidth(priv, (narrow & ~SPI_CTL1_BPT_MASK) |
                     (31 << SPI_CTL1_BPT_SHIFT));

  putreg16(HSDMA_ADVANCED, S1C33_HS_CNTLMODE);
  spi_dma_stop();

  putreg16(HSDMA_WORD, S1C33_HS3_ADVMODE);
  putreg16(words, S1C33_HS3_CNT);
  putreg16(HSDMA_DUAL, S1C33_HS3_CTRL);
  putreg16(0, S1C33_HS3_SADR_L);
  putreg16(0, S1C33_HS3_SADR_H);
  putreg16(0, S1C33_HS3_DADR_L);
  putreg16(HSDMA_INCR_INIT, S1C33_HS3_DADR_H);
  putreg16(S1C33_SPI_RXD & 0xffff, S1C33_HS3_ADV_SADR_L);
  putreg16(S1C33_SPI_RXD >> 16, S1C33_HS3_ADV_SADR_H);
  putreg16(buffer & 0xffff, S1C33_HS3_ADV_DADR_L);
  putreg16(buffer >> 16, S1C33_HS3_ADV_DADR_H);

  putreg16(HSDMA_WORD, S1C33_HS2_ADVMODE);
  putreg16(words - 1, S1C33_HS2_CNT);
  putreg16(HSDMA_DUAL, S1C33_HS2_CTRL);
  putreg16(0, S1C33_HS2_SADR_L);
  putreg16(0, S1C33_HS2_SADR_H);
  putreg16(0, S1C33_HS2_DADR_L);
  putreg16(0, S1C33_HS2_DADR_H);
  putreg16(dummy & 0xffff, S1C33_HS2_ADV_SADR_L);
  putreg16(dummy >> 16, S1C33_HS2_ADV_SADR_H);
  putreg16(S1C33_SPI_TXD & 0xffff, S1C33_HS2_ADV_DADR_L);
  putreg16(S1C33_SPI_TXD >> 16, S1C33_HS2_ADV_DADR_H);

  putreg8(HSDMA_TRIGGER_SPI, S1C33_HSDMA_HTGR2);
  putreg8(0x30, S1C33_INT_FSPI);
  putreg16(1, S1C33_HS2_TF);
  putreg16(1, S1C33_HS3_TF);
  putreg8(HSDMA3_FLAG | (1 << 2), S1C33_INT_FDMA);

  putreg16(1, S1C33_HS3_EN);
  if (words > 1)
    {
      putreg16(1, S1C33_HS2_EN);
    }

  /* Away it goes. */

  putreg32(0xffffffff, S1C33_SPI_TXD);

  start = clock_systime_ticks();
  while ((getreg8(S1C33_INT_FDMA) & HSDMA3_FLAG) == 0 &&
         clock_systime_ticks() - start <= SPI_DMA_TIMEOUT)
    {
    }

  if ((getreg8(S1C33_INT_FDMA) & HSDMA3_FLAG) != 0)
    {
      putreg8(HSDMA3_FLAG, S1C33_INT_FDMA);
      spi_dma_stop();
      spi_wait_idle();
      spi_setwidth(priv, narrow);
      spi_unswap(dest, nbytes);
      return nbytes;
    }

  /* Stop asking for more, let whatever is already on the wire land, and
   * count what the receive channel actually wrote.  A word may be sitting
   * unread in the receive register on top of that.
   */

  spi_dma_stop();
  spi_wait_idle();
  drained = (words - (getreg16(S1C33_HS3_CNT) & 0xffff)) * 4;
  if ((getreg32(S1C33_SPI_STAT) & SPI_STAT_RDFF) != 0 && drained < nbytes)
    {
      *(uint32_t *)(dest + drained) = getreg32(S1C33_SPI_RXD);
      drained += 4;
    }

  spi_setwidth(priv, narrow);
  spi_unswap(dest, drained);
  priv->dma_off = true;
  spierr("DMA stalled after %zu of %zu bytes; CPU transfers from now on\n",
         drained, nbytes);
  return drained;
}

/****************************************************************************
 * Name: spi_exchange
 *
 * Description:
 *   Exchange a block.  A NULL transmit buffer clocks out the all-ones a
 *   card expects while it is the one talking; a NULL receive buffer throws
 *   the reply away.
 *
 ****************************************************************************/

static void spi_exchange(struct spi_dev_s *dev, const void *txbuffer,
                         void *rxbuffer, size_t nwords)
{
  struct s1c33e07_spidev_s *priv = (struct s1c33e07_spidev_s *)dev;
  const uint8_t *src = txbuffer;
  uint8_t *dest = rxbuffer;

  DEBUGASSERT(priv->nbits == 8);

  if (src == NULL && dest != NULL)
    {
      size_t done = spi_recv_dma(priv, dest, nwords);

      dest   += done;
      nwords -= done;
    }

  while (nwords-- > 0)
    {
      uint32_t rx = spi_transfer(src ? *src++ : 0xff);
      if (dest)
        {
          *dest++ = (uint8_t)rx;
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: s1c33e07_spibus_initialize
 ****************************************************************************/

struct spi_dev_s *s1c33e07_spibus_initialize(int port)
{
  struct s1c33e07_spidev_s *priv = &g_spidev;

  if (port != 0)
    {
      return NULL;
    }

  putreg32(0x96, S1C33_CMU_PROTECT);
  modifyreg32(S1C33_CMU_GATE1, 0, CMU_GATE1_SPI_CKE | CMU_GATE1_DMA_CKE);
  putreg32(0, S1C33_CMU_PROTECT);

  /* Master mode requires CTL2 to be zero, and a wait of zero puts no idle
   * clocks between words.
   */

  putreg32(0, S1C33_SPI_CTL2);
  putreg32(0, S1C33_SPI_WAIT);
  putreg32(0, S1C33_SPI_INT);

  priv->ctl1 = 0;
  spi_setbits(&priv->dev, 8);
  spi_setmode(&priv->dev, SPIDEV_MODE0);
  spi_setfrequency(&priv->dev, 400000);

  /* Discard anything the shift register picked up before we got here. */

  getreg32(S1C33_SPI_RXD);
  return &priv->dev;
}
