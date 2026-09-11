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

#define S1C33_P0_DATA       (S1C33_REGBASE + 0x380)
#define S1C33_P0_DIR        (S1C33_REGBASE + 0x381)
#define S1C33_P0_FUNC47     (S1C33_REGBASE + 0x3a1)
#define S1C33_P3_DATA       (S1C33_REGBASE + 0x386)
#define S1C33_P3_DIR        (S1C33_REGBASE + 0x387)
#define S1C33_P8_FUNC03     (S1C33_REGBASE + 0x3b0)
#define S1C33_P9_FUNC47     (S1C33_REGBASE + 0x3b3)

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
