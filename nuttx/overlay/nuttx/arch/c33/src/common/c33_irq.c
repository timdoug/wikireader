/****************************************************************************
 * arch/c33/src/common/c33_irq.c
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
#include <string.h>
#include <syslog.h>
#include <nuttx/arch.h>
#include <nuttx/irq.h>
#include "sched/sched.h"
#include "c33_internal.h"
#include "hardware/s1c33e07.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

uint32_t *g_c33_current_regs;
uintptr_t g_c33_boot_ttbr;

void up_irqinitialize(void)
{
  unsigned int i;

  /* Keep the trap table we were entered with.  A loader that stays resident
   * across the handoff -- Grifo, when NuttX runs as one of its applications
   * -- needs it back to answer a system call from this side again.
   */

  __asm__ volatile ("ld.w %0, %%ttbr" : "=r" (g_c33_boot_ttbr));

  for (i = 0x270; i <= 0x27f; i++)
    {
      putreg8(0, S1C33_REGBASE + i);
    }

  putreg8(1, S1C33_REGBASE + 0x29f);
  for (i = 0x280; i <= 0x28f; i++)
    {
      putreg8(0xff, S1C33_REGBASE + i);
    }

  __asm__ volatile ("ld.w %%ttbr, %0" : : "r" (g_c33_vectors) : "memory");
  up_irq_enable();
}

static void c33_irq_mask(int irq, bool enable)
{
  uintptr_t reg;
  unsigned int bit;

  if (irq >= 30 && irq <= 51 && ((irq - 30) % 4) < 2)
    {
      unsigned int channel = (irq - 30) / 4;

      reg = S1C33_REGBASE + 0x272 + channel / 2;
      bit = 1 << (2 + (channel % 2) * 4 + (irq - 30) % 4);
    }
  else if (irq >= C33_IRQ_UART0ERR && irq <= C33_IRQ_UART0TX)
    {
      reg = S1C33_INT_ESIF01;
      bit = 1 << (irq - C33_IRQ_UART0ERR);
    }
  else if (irq >= C33_IRQ_UART1ERR && irq <= C33_IRQ_UART1TX)
    {
      reg = S1C33_INT_ESIF01;
      bit = 1 << (4 + irq - C33_IRQ_UART1ERR);
    }
  else
    {
      return;
    }

  modifyreg8(reg, enable ? 0 : bit, enable ? bit : 0);
}

void up_enable_irq(int irq)
{
  c33_irq_mask(irq, true);
}

void up_disable_irq(int irq)
{
  c33_irq_mask(irq, false);
}

uint32_t *c33_doirq(int irq, uint32_t *regs)
{
  /* A scheduling trap runs after the ready-list head has changed.  Until
   * g_c33_current_regs is set, running_task() would name the incoming task.
   */

  struct tcb_s *rtcb = g_running_tasks[0];
  struct tcb_s *tcb;

  if (irq < C33_IRQ_SWITCH || irq == 15)
    {
      up_dump_register(regs);
      PANIC();
    }

  DEBUGASSERT(!up_interrupt_context());
  rtcb->xcp.regs = regs;
  g_c33_current_regs = regs;
  if (irq == C33_IRQ_SIGNAL)
    {
      c33_sigsetup(rtcb);
    }
  else if (irq == C33_IRQ_SAVE)
    {
      memcpy((void *)regs[REG_R6], regs, XCPTCONTEXT_SIZE);
    }
  else if (irq != C33_IRQ_SWITCH)
    {
      irq_dispatch(irq, regs);
    }

  tcb = this_task();
  if (tcb != rtcb)
    {
      nxsched_switch_context(rtcb, tcb);
      g_running_tasks[0] = tcb;
    }

  regs = tcb->xcp.regs;
  g_c33_current_regs = NULL;
  tcb->xcp.regs = NULL;
  return regs;
}

void up_dump_register(void *dumpregs)
{
  uint32_t *regs = dumpregs;
  unsigned int i;

  if (regs == NULL)
    {
      return;
    }

  syslog(LOG_ALERT, "C33 IRQ=%lu PC=%08lx PSR=%08lx SP=%08lx\n",
         (unsigned long)regs[REG_IRQ], (unsigned long)regs[REG_PC],
         (unsigned long)regs[REG_PSR], (unsigned long)regs[REG_SP]);
  for (i = 0; i < 16; i += 4)
    {
      syslog(LOG_ALERT, "r%u: %08lx %08lx %08lx %08lx\n", i,
             (unsigned long)regs[i], (unsigned long)regs[i + 1],
             (unsigned long)regs[i + 2], (unsigned long)regs[i + 3]);
    }
}
