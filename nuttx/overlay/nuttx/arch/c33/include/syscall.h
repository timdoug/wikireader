/****************************************************************************
 * arch/c33/include/syscall.h
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

/* <sys/syscall.h> includes this unconditionally, so every architecture has
 * to have one even when, as here, there is nothing in it.  This port is a
 * flat build: an application calls the kernel by calling it, so there are no
 * sys_call trap stubs to declare.  The traps the port does use -- 0 for a
 * context switch, 1 for signal delivery, 2 for diagnostics -- belong to the
 * scheduler, not to a system call interface, and are in c33_context.S.
 */

#ifndef __ARCH_C33_INCLUDE_SYSCALL_H
#define __ARCH_C33_INCLUDE_SYSCALL_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifndef __ASSEMBLY__
#ifdef __cplusplus
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

#undef EXTERN
#ifdef __cplusplus
}
#endif
#endif /* __ASSEMBLY__ */

#endif /* __ARCH_C33_INCLUDE_SYSCALL_H */
