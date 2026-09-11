/****************************************************************************
 * arch/c33/include/irq.h
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

#ifndef __ARCH_C33_INCLUDE_IRQ_H
#define __ARCH_C33_INCLUDE_IRQ_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#define NR_IRQS           256
#define C33_IRQ_SWITCH    12
#define C33_IRQ_SIGNAL    13
#define C33_IRQ_SAVE      14
#define C33_IRQ_TIMER2    38
#define C33_IRQ_UART0ERR  56
#define C33_IRQ_UART0RX   57
#define C33_IRQ_UART0TX   58
#define C33_IRQ_UART1ERR  60
#define C33_IRQ_UART1RX   61
#define C33_IRQ_UART1TX   62

#define REG_R0   0
#define REG_R1   1
#define REG_R2   2
#define REG_R3   3
#define REG_R4   4
#define REG_R5   5
#define REG_R6   6
#define REG_R7   7
#define REG_R8   8
#define REG_R9   9
#define REG_R10  10
#define REG_R11  11
#define REG_R12  12
#define REG_R13  13
#define REG_R14  14
#define REG_R15  15
#define REG_ALR  16
#define REG_AHR  17
#define REG_SP   18
#define REG_IRQ  19
#define REG_PSR  22
#define REG_PC   23
#define XCPTCONTEXT_REGS 24
#define XCPTCONTEXT_SIZE (4 * XCPTCONTEXT_REGS)
#define C33_PSR_IE (1 << 4)
#define STACKFRAME_ALIGN 16

#ifndef __ASSEMBLY__
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <arch/types.h>

struct xcptcontext
{
  uint32_t *regs;
  uint32_t *saved_regs;
};

extern uint32_t *g_c33_current_regs;

/* Whatever %ttbr held when up_irqinitialize() ran.  On a board entered from a
 * loader that stays resident, that is the loader's trap table, and restoring
 * it is how the board reaches the loader's traps again.
 */

extern uintptr_t g_c33_boot_ttbr;

static inline uint32_t up_getsp(void)
{
  uint32_t sp;
  __asm__ volatile ("ld.w %0, %%sp" : "=r" (sp));
  return sp;
}

static inline irqstate_t up_irq_save(void)
{
  irqstate_t flags;
  __asm__ volatile ("ld.w %0, %%psr\n\tpsrclr 4"
                    : "=r" (flags) : : "memory");
  return flags;
}

static inline irqstate_t up_irq_enable(void)
{
  irqstate_t flags;
  __asm__ volatile ("ld.w %0, %%psr\n\tpsrset 4"
                    : "=r" (flags) : : "memory");
  return flags;
}

static inline void up_irq_restore(irqstate_t flags)
{
  if ((flags & C33_PSR_IE) != 0)
    {
      __asm__ volatile ("psrset 4" : : : "memory");
    }
  else
    {
      __asm__ volatile ("psrclr 4" : : : "memory");
    }
}

static inline bool up_interrupt_context(void)
{
  return g_c33_current_regs != NULL;
}

static inline uint32_t *up_current_regs(void)
{
  return g_c33_current_regs;
}

#define up_getusrpc(r) (((uint32_t *)((r) ? (r) : up_current_regs()))[REG_PC])
#define up_getusrsp(r) (((uint32_t *)(r))[REG_SP])
#endif
#endif
