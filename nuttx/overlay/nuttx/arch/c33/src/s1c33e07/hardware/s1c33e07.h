/****************************************************************************
 * arch/c33/src/s1c33e07/hardware/s1c33e07.h
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

#ifndef __ARCH_C33_SRC_S1C33E07_HARDWARE_S1C33E07_H
#define __ARCH_C33_SRC_S1C33E07_HARDWARE_S1C33E07_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/arch.h>

#define S1C33_REGBASE       0x00300000
#define S1C33_INT_P16T23    (S1C33_REGBASE + 0x267)
#define S1C33_INT_PSI01     (S1C33_REGBASE + 0x26a)
#define S1C33_INT_ESIF01    (S1C33_REGBASE + 0x276)
#define S1C33_INT_F16T23    (S1C33_REGBASE + 0x283)
#define S1C33_INT_FSIF01    (S1C33_REGBASE + 0x286)
#define S1C33_WD_WP         (S1C33_REGBASE + 0x660)
#define S1C33_WD_EN         (S1C33_REGBASE + 0x662)
#define S1C33_T16_CR2A      (S1C33_REGBASE + 0x790)
#define S1C33_T16_CR2B      (S1C33_REGBASE + 0x792)
#define S1C33_T16_CTL2      (S1C33_REGBASE + 0x796)
#define S1C33_T16_PAUSE     (S1C33_REGBASE + 0x7dc)
#define S1C33_T16_ADVMODE   (S1C33_REGBASE + 0x7de)
#define S1C33_T16_CLKCTL2   (S1C33_REGBASE + 0x7e4)
#define S1C33_UART0_TXD     (S1C33_REGBASE + 0xb00)
#define S1C33_UART0_RXD     (S1C33_REGBASE + 0xb01)
#define S1C33_UART0_STATUS  (S1C33_REGBASE + 0xb02)
#define S1C33_UART0_CTL     (S1C33_REGBASE + 0xb03)
#define S1C33_UART1_RXD     (S1C33_REGBASE + 0xb11)
#define S1C33_UART1_STATUS  (S1C33_REGBASE + 0xb12)
#define S1C33_UART1_CTL     (S1C33_REGBASE + 0xb13)
#define S1C33_UART1_IRDA    (S1C33_REGBASE + 0xb14)
#define S1C33_UART1_BRT     (S1C33_REGBASE + 0xb15)
#define S1C33_UART1_BRTL    (S1C33_REGBASE + 0xb16)
#define S1C33_UART1_BRTH    (S1C33_REGBASE + 0xb17)
#define S1C33_SDRAMC_INI    (S1C33_REGBASE + 0x1600)
#define S1C33_SDRAMC_CTL    (S1C33_REGBASE + 0x1604)
#define S1C33_CMU_GATE1     (S1C33_REGBASE + 0x1b04)
#define S1C33_CMU_GATE0     (S1C33_REGBASE + 0x1b00)
#define S1C33_CMU_PROTECT   (S1C33_REGBASE + 0x1b24)

/* Gated clocks.  Writes to any other CMU register are discarded unless
 * CMU_PROTECT holds 0x96, so bracket them.
 */

#define CMU_GATE1_SPI_CKE   (1 << 6)
#define CMU_GATE1_DMA_CKE   (1 << 1)

#define S1C33_P0_DATA       (S1C33_REGBASE + 0x380)
#define S1C33_P0_DIR        (S1C33_REGBASE + 0x381)
#define S1C33_P0_FUNC47     (S1C33_REGBASE + 0x3a1)
#define S1C33_P3_DATA       (S1C33_REGBASE + 0x386)
#define S1C33_P3_DIR        (S1C33_REGBASE + 0x387)
#define S1C33_P5_DATA       (S1C33_REGBASE + 0x38a)
#define S1C33_P5_DIR        (S1C33_REGBASE + 0x38b)
#define S1C33_P5_FUNC03     (S1C33_REGBASE + 0x3aa)
#define S1C33_P6_DATA       (S1C33_REGBASE + 0x38c)
#define S1C33_P6_DIR        (S1C33_REGBASE + 0x38d)
#define S1C33_P6_FUNC47     (S1C33_REGBASE + 0x3ad)
#define S1C33_P8_FUNC03     (S1C33_REGBASE + 0x3b0)
#define S1C33_P9_FUNC47     (S1C33_REGBASE + 0x3b3)

/* SPI controller.  Master mode only here: CTL2 must be zero for that. The
 * card and the serial FLASH share the bus, each with its own chip select on
 * port 5.
 */

#define S1C33_SPI_RXD       (S1C33_REGBASE + 0x1700)
#define S1C33_SPI_TXD       (S1C33_REGBASE + 0x1704)
#define S1C33_SPI_CTL1      (S1C33_REGBASE + 0x1708)
#define S1C33_SPI_CTL2      (S1C33_REGBASE + 0x170c)
#define S1C33_SPI_WAIT      (S1C33_REGBASE + 0x1710)
#define S1C33_SPI_STAT      (S1C33_REGBASE + 0x1714)
#define S1C33_SPI_INT       (S1C33_REGBASE + 0x1718)

/* CTL1: bits per transfer less one at 10, then phase, polarity, the master
 * clock divider (MCLK >> (2 + n)), and the master-mode and enable bits.
 */

#define SPI_CTL1_BPT_SHIFT  10
#define SPI_CTL1_BPT_MASK   (0x1f << 10)
#define SPI_CTL1_CPHA       (1 << 9)
#define SPI_CTL1_CPOL       (1 << 8)
#define SPI_CTL1_MCBR_SHIFT 4
#define SPI_CTL1_MCBR_MASK  (7 << 4)
#define SPI_CTL1_TXDE       (1 << 3)
#define SPI_CTL1_RXDE       (1 << 2)
#define SPI_CTL1_MASTER     (1 << 1)
#define SPI_CTL1_ENA        (1 << 0)

#define SPI_STAT_BSYF       (1 << 6)
#define SPI_STAT_TDEF       (1 << 4)
#define SPI_STAT_RDOF       (1 << 3)
#define SPI_STAT_RDFF       (1 << 2)

/* DMA.  Only the two engines the SD card needs are named: high-speed
 * channel 3 drains the SPI receive register into memory, and intelligent
 * channel 0x24 answers each received byte with a dummy transmit, which is
 * what keeps the clock running while the card talks.
 */

#define S1C33_INT_FDMA      (S1C33_REGBASE + 0x281)
#define S1C33_INT_FSPI      (S1C33_REGBASE + 0x289)
#define S1C33_HSDMA_HTGR2   (S1C33_REGBASE + 0x299)
#define S1C33_IDMAREQ_SPI   (S1C33_REGBASE + 0x29b)
#define S1C33_IDMAEN_SPI    (S1C33_REGBASE + 0x29c)

#define S1C33_IDMABASE0     (S1C33_REGBASE + 0x1100)
#define S1C33_IDMABASE1     (S1C33_REGBASE + 0x1102)
#define S1C33_IDMA_EN       (S1C33_REGBASE + 0x1105)

#define S1C33_HS2_CNT       (S1C33_REGBASE + 0x1140)
#define S1C33_HS2_CTRL      (S1C33_REGBASE + 0x1142)
#define S1C33_HS2_SADR_L    (S1C33_REGBASE + 0x1144)
#define S1C33_HS2_SADR_H    (S1C33_REGBASE + 0x1146)
#define S1C33_HS2_DADR_L    (S1C33_REGBASE + 0x1148)
#define S1C33_HS2_DADR_H    (S1C33_REGBASE + 0x114a)
#define S1C33_HS2_TF        (S1C33_REGBASE + 0x114e)
#define S1C33_HS3_CNT       (S1C33_REGBASE + 0x1150)
#define S1C33_HS3_CTRL      (S1C33_REGBASE + 0x1152)
#define S1C33_HS3_SADR_L    (S1C33_REGBASE + 0x1154)
#define S1C33_HS3_SADR_H    (S1C33_REGBASE + 0x1156)
#define S1C33_HS3_DADR_L    (S1C33_REGBASE + 0x1158)
#define S1C33_HS3_DADR_H    (S1C33_REGBASE + 0x115a)
#define S1C33_HS2_EN        (S1C33_REGBASE + 0x114c)
#define S1C33_HS3_EN        (S1C33_REGBASE + 0x115c)
#define S1C33_HS3_TF        (S1C33_REGBASE + 0x115e)
#define S1C33_HS2_ADVMODE   (S1C33_REGBASE + 0x1182)
#define S1C33_HS2_ADV_SADR_L (S1C33_REGBASE + 0x1184)
#define S1C33_HS2_ADV_SADR_H (S1C33_REGBASE + 0x1186)
#define S1C33_HS2_ADV_DADR_L (S1C33_REGBASE + 0x1188)
#define S1C33_HS2_ADV_DADR_H (S1C33_REGBASE + 0x118a)
#define S1C33_HS3_ADVMODE   (S1C33_REGBASE + 0x1192)
#define S1C33_HS3_ADV_SADR_L (S1C33_REGBASE + 0x1194)
#define S1C33_HS3_ADV_SADR_H (S1C33_REGBASE + 0x1196)
#define S1C33_HS3_ADV_DADR_L (S1C33_REGBASE + 0x1198)
#define S1C33_HS3_ADV_DADR_H (S1C33_REGBASE + 0x119a)
#define S1C33_HS_CNTLMODE   (S1C33_REGBASE + 0x119c)

#define HSDMA_ADVANCED      (1 << 0)     /* CNTLMODE: advanced addressing */
#define HSDMA_DUAL          (1 << 15)    /* CTRL: dual-address transfer */
#define HSDMA_INCR_INIT     (0x2 << 12)  /* ADR_H: increment, reload each run */
#define HSDMA_WORD          (1 << 0)     /* ADVMODE: 32-bit units */
#define HSDMA_TRIGGER_SPI   0x99         /* HTGR2: ch2 SPI transmit, ch3 receive */
#define HSDMA3_FLAG         (1 << 3)     /* INT_FDMA: channel 3 terminal count */
#define IDMA_SPI_ENABLE     (1 << 4)     /* IDMAREQ/IDMAEN: the SPI cause */

/* Descriptor RAM.  The engines only accept control blocks from internal
 * DSTRAM or SDRAM, 16-byte aligned.  The boot loader keeps its own table
 * here, which is harmless: it is not running.
 */

#define S1C33_DSTRAM        0x00084000

#define S1C33_LCD_INT       (S1C33_REGBASE + 0x1a00)
#define S1C33_LCD_POWER     (S1C33_REGBASE + 0x1a04)
#define S1C33_LCD_HD        (S1C33_REGBASE + 0x1a10)
#define S1C33_LCD_VD        (S1C33_REGBASE + 0x1a14)
#define S1C33_LCD_MR        (S1C33_REGBASE + 0x1a18)
#define S1C33_LCD_MODE      (S1C33_REGBASE + 0x1a60)
#define S1C33_LCD_MADDR     (S1C33_REGBASE + 0x1a70)
#define S1C33_LCD_STRIDE    (S1C33_REGBASE + 0x1a74)
#define S1C33_LCD_PIP       (S1C33_REGBASE + 0x1a88)

#define getreg8(a) (*(volatile uint8_t *)(uintptr_t)(a))
#define getreg16(a) (*(volatile uint16_t *)(uintptr_t)(a))
#define getreg32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define putreg8(v,a) (getreg8(a) = (v))
#define putreg16(v,a) (getreg16(a) = (v))
#define putreg32(v,a) (getreg32(a) = (v))

static inline void modifyreg8(uintptr_t addr, uint8_t clear, uint8_t set)
{
  putreg8((getreg8(addr) & ~clear) | set, addr);
}

static inline void modifyreg16(uintptr_t addr, uint16_t clear, uint16_t set)
{
  putreg16((getreg16(addr) & ~clear) | set, addr);
}

static inline void modifyreg32(uintptr_t addr, uint32_t clear, uint32_t set)
{
  putreg32((getreg32(addr) & ~clear) | set, addr);
}

#endif
