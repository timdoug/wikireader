/****************************************************************************
 * libs/libc/machine/c33/arch_memmove.S
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

#ifdef LIBC_BUILD_MEMMOVE

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* void *memmove(void *dest, const void *src, size_t n)
 *
 * r6 = dest, r7 = src, r8 = n, result in r4. A destination at or below the
 * source copies forward, which is what memcpy already does well. Only the
 * overlapping downward case runs here, and it walks backwards: the core has
 * no pre-decrementing access, so a word costs two subtractions on top of
 * the load, the store, the compare and the branch.
 */

  .text
  .global memmove
  .type memmove, @function

memmove:
  cmp     %r6,%r7
  jrugt   7f
  xjp     memcpy                    /* no overlap going up */
7:
  ld.w    %r4,%r6                   /* the return value is the destination */
  add     %r6,%r8                   /* one past the end of each region */
  add     %r7,%r8
  ld.w    %r9,%r6
  xor     %r9,%r7
  and     %r9,3
  jrne    3f                        /* different alignments: byte at a time */
  cmp     %r8,8
  jrult   3f                        /* too short to pay for the setup */

1:                                  /* bring the destination end to a word */
  ld.w    %r9,%r6
  and     %r9,3
  jreq    2f
  sub     %r6,1
  sub     %r7,1
  ld.ub   %r9,[%r7]
  ld.b    [%r6],%r9
  sub     %r8,1
  jp      1b

2:                                  /* at least four bytes are left here */
  ld.w    %r5,%r8
  and     %r5,3                     /* bytes leading the first whole word */
  sub     %r8,%r5
  ld.w    %r9,%r6
  sub     %r9,%r8                   /* destination end of the word run */
  ld.w    %r8,%r9
5:
  sub     %r6,4
  sub     %r7,4
  ld.w    %r9,[%r7]
  ld.w    [%r6],%r9
  cmp     %r6,%r8
  jrne    5b
  ld.w    %r8,%r5

3:
  cmp     %r8,0
  jreq    4f
6:
  sub     %r6,1
  sub     %r7,1
  ld.ub   %r9,[%r7]
  ld.b    [%r6],%r9
  sub     %r8,1
  jrne    6b
4:
  ret
  .size memmove, .-memmove

#endif /* LIBC_BUILD_MEMMOVE */
