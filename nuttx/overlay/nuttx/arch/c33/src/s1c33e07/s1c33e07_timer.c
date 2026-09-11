/****************************************************************************
 * arch/c33/src/s1c33e07/s1c33e07_timer.c
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
#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/irq.h>
#include "hardware/s1c33e07.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

static int c33_timer_interrupt(int irq, void *context, void *arg)
{
  putreg8(1 << 2, S1C33_INT_F16T23);
  nxsched_process_timer();
  return OK;
}

void up_timer_initialize(void)
{
  uint32_t count = (uint64_t)CONFIG_S1C33E07_MCLK *
                   CONFIG_USEC_PER_TICK / 64000000;

  DEBUGASSERT(count > 0 && count <= 65536);

  putreg32(0x96, S1C33_CMU_PROTECT);
  modifyreg32(S1C33_CMU_GATE1, 0, 1 << 15);
  putreg32(0, S1C33_CMU_PROTECT);
  putreg16(1, S1C33_T16_ADVMODE);
  putreg16(2, S1C33_T16_CTL2);
  putreg16(8 | 4, S1C33_T16_CLKCTL2);
  putreg16(count - 1, S1C33_T16_CR2A);
  putreg16(count - 1, S1C33_T16_CR2B);
  modifyreg8(S1C33_INT_P16T23, 7, 4);
  putreg8(0x0c, S1C33_INT_F16T23);
  irq_attach(C33_IRQ_TIMER2, c33_timer_interrupt, NULL);
  up_enable_irq(C33_IRQ_TIMER2);
  modifyreg16(S1C33_T16_PAUSE, 1 << 2, 0);
  putreg16(2 | 1, S1C33_T16_CTL2);
}
