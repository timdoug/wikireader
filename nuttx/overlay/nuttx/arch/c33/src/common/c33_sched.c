/****************************************************************************
 * arch/c33/src/common/c33_sched.c
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
#include <nuttx/arch.h>
#include "sched/sched.h"
#include "task/task.h"
#include "c33_internal.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void up_switch_context(struct tcb_s *tcb, struct tcb_s *rtcb)
{
  if (!up_interrupt_context())
    {
      __asm__ volatile ("int 0" : : : "memory");
    }

  /* Interrupt return selects this_task() after irq_dispatch completes. */
}

void up_exit(int status)
{
  struct tcb_s *tcb;
  uint32_t *regs;

  up_irq_save();
  nxtask_exit();
  tcb = this_task();
  g_running_tasks[0] = tcb;
  regs = tcb->xcp.regs;
  tcb->xcp.regs = NULL;
  c33_restore(regs);
}

static void c33_sigdeliver(void)
{
  struct tcb_s *tcb = this_task();
  uint32_t *saved = tcb->xcp.saved_regs;

  do
    {
      up_irq_enable();
      tcb->sigdeliver(tcb);
      up_irq_save();
    }
  while (!sq_empty(&tcb->sigpendactionq) &&
         (tcb->flags & TCB_FLAG_SIGNAL_ACTION) == 0);
  tcb->sigdeliver = NULL;
  tcb->xcp.saved_regs = NULL;
  c33_restore(saved);
}

void c33_sigsetup(struct tcb_s *tcb)
{
  uintptr_t sp;

  DEBUGASSERT(tcb->xcp.regs != NULL);
  DEBUGASSERT(tcb->xcp.saved_regs == NULL);
  tcb->xcp.saved_regs = tcb->xcp.regs;
  sp = C33_ALIGN_DOWN(tcb->xcp.regs) - 4;
  tcb->xcp.regs = (uint32_t *)(sp - XCPTCONTEXT_SIZE);
  memcpy(tcb->xcp.regs, tcb->xcp.saved_regs, XCPTCONTEXT_SIZE);
  tcb->xcp.regs[REG_SP] = sp;
  tcb->xcp.regs[REG_PC] = (uintptr_t)c33_sigdeliver;
  tcb->xcp.regs[REG_PSR] = 0;
}

void up_schedule_sigaction(struct tcb_s *tcb)
{
  if (tcb == running_task() && !up_interrupt_context())
    {
      __asm__ volatile ("int 1" : : : "memory");
    }
  else
    {
      c33_sigsetup(tcb);
    }
}
