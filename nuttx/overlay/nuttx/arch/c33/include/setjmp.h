/****************************************************************************
 * arch/c33/include/setjmp.h
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

#ifndef __ARCH_C33_INCLUDE_SETJMP_H
#define __ARCH_C33_INCLUDE_SETJMP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/compiler.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* %r0-%r3 are the callee-saved registers of the C33 ABI (see
 * host-tools/toolchain-c33/gcc/ABI.md in the WikiReader tree), followed by
 * the caller's stack pointer and the address to resume at.  Nothing else
 * has to be kept: %r4-%r13 are caller-saved, %r15 is the fixed data
 * pointer, and the PSR is not part of the calling convention.
 */

struct setjmp_buf_s
{
  unsigned long regs[6];
};

/* Traditional typedef for setjmp_buf */

typedef struct setjmp_buf_s jmp_buf[1];

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) noreturn_function;

#endif /* __ARCH_C33_INCLUDE_SETJMP_H */
