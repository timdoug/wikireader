/****************************************************************************
 * libs/libc/machine/c33/arch_memcpy.S
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

#include "libc.h"

#ifdef LIBC_BUILD_MEMCPY

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* void *memcpy(void *dest, const void *src, size_t n)
 *
 * r6 = dest, r7 = src, r8 = n, result in r4. r5 and r9 are the scratch
 * registers; r0-r3 belong to the caller and are left alone.
 *
 * The word loop is two post-incrementing accesses, a compare and a branch,
 * so four instructions move four bytes. The byte loop the generic C
 * implementation uses needs five instructions per byte.
 */

  .text
  .global memcpy
  .type memcpy, @function

memcpy:
  ld.w    %r4,%r6                   /* the return value is the destination */
  ld.w    %r9,%r6
  xor     %r9,%r7
  and     %r9,3
  jrne    3f                        /* different alignments: byte at a time */
  cmp     %r8,8
  jrult   3f                        /* too short to pay for the setup */

1:                                  /* bring the destination to a word */
  ld.w    %r9,%r6
  and     %r9,3
  jreq    2f
  ld.ub   %r9,[%r7]+
  ld.b    [%r6]+,%r9
  sub     %r8,1
  jp      1b

2:                                  /* at least four bytes are left here */
  ld.w    %r5,%r8
  and     %r5,3                     /* bytes trailing the last whole word */
  sub     %r8,%r5
  add     %r8,%r7                   /* source end of the word run */
5:
  ld.w    %r9,[%r7]+
  ld.w    [%r6]+,%r9
  cmp     %r7,%r8
  jrne    5b
  ld.w    %r8,%r5

3:
  cmp     %r8,0
  jreq    4f
6:
  ld.ub   %r9,[%r7]+
  ld.b    [%r6]+,%r9
  sub     %r8,1
  jrne    6b
4:
  ret
  .size memcpy, .-memcpy

#endif /* LIBC_BUILD_MEMCPY */
