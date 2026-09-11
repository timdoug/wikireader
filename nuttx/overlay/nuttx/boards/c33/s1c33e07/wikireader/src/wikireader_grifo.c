/****************************************************************************
 * boards/c33/s1c33e07/wikireader/src/wikireader_grifo.c
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

/* Powering down and rebooting when NuttX was started as a Grifo application.
 *
 * Grifo is the WikiReader's resident kernel.  Its loader reads an ELF file
 * off the card, places the sections at their linked addresses, and jumps to
 * e_entry; it then stays in memory below 0x10040000, which is why this
 * configuration links above that.  Both ways of stopping the machine --
 * cutting power and resetting through the watchdog -- are board specifics
 * that Grifo already implements, so NuttX asks it rather than repeating it.
 *
 * A Grifo system call is "int 1" followed by a halfword holding the call
 * number; the kernel's handler recovers the number from the return address.
 * Nothing has to be linked against Grifo for that, which is what lets this
 * board call it without adopting the application ABI anywhere else.  The
 * trap does need Grifo's trap table back, since NuttX replaced it at
 * startup.
 *
 * Nothing else has to be given back.  Grifo's System_exit() path runs to
 * power_off() or the reset watchdog with interrupts disabled -- it never
 * enables them unconditionally, and neither does this -- so the interrupt
 * controller can be left exactly as NuttX had it.
 */

#include <nuttx/config.h>
#include <stdint.h>
#include <nuttx/arch.h>
#include <nuttx/board.h>
#include <nuttx/irq.h>
#include "wikireader.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Grifo's System_ExitType, from samo-lib/include/grifo.h. */

#define GRIFO_EXIT_POWER_OFF    1
#define GRIFO_EXIT_REBOOT       2

/* ...and the call number of System_exit, from samo-lib/grifo/stubs/exit.s. */

#define GRIFO_SYSCALL_EXIT      16

#define GRIFO_STRINGIFY_(x)     #x
#define GRIFO_STRINGIFY(x)      GRIFO_STRINGIFY_(x)

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void wikireader_grifo_exit(int code) noreturn_function;

static void wikireader_grifo_exit(int code)
{
  up_irq_save();

  /* Restore the loader's trap table and leave through its exit system call,
   * in one block so that nothing comes between the two.  The call number is
   * the halfword after "int 1" and the argument goes in %r6, like any other
   * first argument.
   *
   * %r6 is loaded inside the asm rather than through a local register
   * variable, which the compiler need only have in its register at the next
   * asm and is free to use for anything until then.
   */

  __asm__ volatile ("ld.w %%r6, %0\n\t"
                    "ld.w %%ttbr, %1\n\t"
                    "int 1\n\t"
                    ".short " GRIFO_STRINGIFY(GRIFO_SYSCALL_EXIT)
                    : : "r" (code), "r" (g_c33_boot_ttbr)
                    : "%r6", "memory");

  /* System_exit() does not come back. */

  for (; ; )
    {
      __asm__ volatile ("halt");
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int board_power_off(int status)
{
  wikireader_grifo_exit(GRIFO_EXIT_POWER_OFF);
}

int board_reset(int status)
{
  wikireader_grifo_exit(GRIFO_EXIT_REBOOT);
}
