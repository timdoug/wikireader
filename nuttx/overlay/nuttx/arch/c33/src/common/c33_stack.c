/****************************************************************************
 * arch/c33/src/common/c33_stack.c
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
#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>
#include "c33_internal.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int up_create_stack(struct tcb_s *tcb, size_t size, uint8_t ttype)
{
  if (tcb->stack_alloc_ptr != NULL)
    {
      up_release_stack(tcb, ttype);
    }

  size = C33_ALIGN_UP(size);
  tcb->stack_alloc_ptr = kumm_memalign(STACK_ALIGNMENT, size);
  if (tcb->stack_alloc_ptr == NULL)
    {
      return -ENOMEM;
    }

  tcb->stack_base_ptr = tcb->stack_alloc_ptr;
  tcb->adj_stack_size = size;
  tcb->flags |= TCB_FLAG_FREE_STACK;
  return OK;
}

int up_use_stack(struct tcb_s *tcb, void *stack, size_t size)
{
  uintptr_t base = C33_ALIGN_UP(stack);
  uintptr_t top = C33_ALIGN_DOWN((uintptr_t)stack + size);

  if (top <= base + XCPTCONTEXT_SIZE + 16)
    {
      return -EINVAL;
    }

  up_release_stack(tcb, tcb->flags & TCB_FLAG_TTYPE_MASK);
  tcb->stack_alloc_ptr = stack;
  tcb->stack_base_ptr = (void *)base;
  tcb->adj_stack_size = top - base;
  return OK;
}

void up_release_stack(struct tcb_s *tcb, uint8_t ttype)
{
  if ((tcb->flags & TCB_FLAG_FREE_STACK) != 0)
    {
      kumm_free(tcb->stack_alloc_ptr);
    }

  tcb->flags &= ~TCB_FLAG_FREE_STACK;
  tcb->stack_alloc_ptr = NULL;
  tcb->stack_base_ptr = NULL;
  tcb->adj_stack_size = 0;
}

void *up_stack_frame(struct tcb_s *tcb, size_t size)
{
  void *frame = tcb->stack_base_ptr;

  size = C33_ALIGN_UP(size);
  if (frame == NULL || size >= tcb->adj_stack_size)
    {
      return NULL;
    }

  memset(frame, 0, size);
  tcb->stack_base_ptr = (uint8_t *)frame + size;
  tcb->adj_stack_size -= size;
  return frame;
}

void up_initial_state(struct tcb_s *tcb)
{
  uintptr_t sp;

  memset(&tcb->xcp, 0, sizeof(tcb->xcp));
  if (tcb->pid == IDLE_PROCESS_ID)
    {
      tcb->stack_alloc_ptr = _sidle;
      tcb->stack_base_ptr = _sidle;
      tcb->adj_stack_size = _eidle - _sidle;
      return;
    }

  /* A C callee enters at 12 modulo 16 after CALL pushes its return PC. */

  sp = C33_ALIGN_DOWN((uintptr_t)tcb->stack_base_ptr + tcb->adj_stack_size);
  sp -= 4;
  *(uint32_t *)sp = 0;
  tcb->xcp.regs = (uint32_t *)(sp - XCPTCONTEXT_SIZE);
  memset(tcb->xcp.regs, 0, XCPTCONTEXT_SIZE);
  tcb->xcp.regs[REG_SP] = sp;
  tcb->xcp.regs[REG_PC] = (uintptr_t)tcb->start;
  tcb->xcp.regs[REG_PSR] = C33_PSR_IE;
  tcb->xcp.regs[REG_R15] = (uintptr_t)__dp;
}
