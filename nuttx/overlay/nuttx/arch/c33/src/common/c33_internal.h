/****************************************************************************
 * arch/c33/src/common/c33_internal.h
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

#ifndef __ARCH_C33_SRC_COMMON_C33_INTERNAL_H
#define __ARCH_C33_SRC_COMMON_C33_INTERNAL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/arch.h>
#include <nuttx/irq.h>

#define C33_ALIGN_DOWN(a) ((uintptr_t)(a) & ~(uintptr_t)15)
#define C33_ALIGN_UP(a) (((uintptr_t)(a) + 15) & ~(uintptr_t)15)

extern uint8_t _sbss[];
extern uint8_t _ebss[];
extern uint8_t _sidle[];
extern uint8_t _eidle[];
extern uint8_t _sheap[];
extern uint8_t _eheap[];
extern uint8_t __dp[];
extern const uintptr_t g_c33_vectors[];

void c33_start(void) noreturn_function;
void c33_restore(uint32_t *regs) noreturn_function;
uint32_t *c33_doirq(int irq, uint32_t *regs);
void c33_sigsetup(struct tcb_s *tcb);
void c33_serialinit(void);
void c33_lowsetup(void);
#endif
