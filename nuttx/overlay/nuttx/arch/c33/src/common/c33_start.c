/****************************************************************************
 * arch/c33/src/common/c33_start.c
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
#include <nuttx/init.h>
#include "c33_internal.h"
#include "hardware/s1c33e07.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const uint8_t g_ram_size_mb[8] =
{
  2, 8, 16, 32, 4, 16, 32, 64
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void c33_start(void)
{
  memset(_sbss, 0, _ebss - _sbss);
  c33_lowsetup();
  nx_start();
}

void up_allocate_heap(void **heap_start, size_t *heap_size)
{
  uintptr_t end = (uintptr_t)_eheap;

  /* Some WikiReader revisions have 16 MiB.  Respect the loader's SDRAM
   * geometry, bounded by the linker's configured RAM region.  Direct ELF
   * boot in wremu omits controller initialization and uses CONFIG_RAM_SIZE.
   */

  if ((getreg32(S1C33_SDRAMC_INI) & (1 << 3)) != 0)
    {
      unsigned int mode = getreg32(S1C33_SDRAMC_CTL) & 7;
      uintptr_t limit = 0x10000000 + ((uintptr_t)g_ram_size_mb[mode] << 20);

      if (end > limit)
        {
          end = limit;
        }
    }

  DEBUGASSERT(end > (uintptr_t)_sheap);
  *heap_start = _sheap;
  *heap_size = end - (uintptr_t)_sheap;
}

void up_initialize(void)
{
  c33_serialinit();
}

void up_idle(void)
{
  /* HALT wakes for enabled ITC causes even with PSR.IE clear. Holding IE
   * clear between the ready-list check and HALT avoids a lost wakeup.
   */

  irqstate_t flags = up_irq_save();

  __asm__ volatile ("halt" : : : "memory");
  up_irq_restore(flags);
}
