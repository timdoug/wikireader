/****************************************************************************
 * libs/libc/machine/c33/arch_memcmp.S
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

#ifdef LIBC_BUILD_MEMCMP

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* int memcmp(const void *s1, const void *s2, size_t n)
 *
 * r6 = s1, r7 = s2, r8 = n, result in r4. Equal regions are the common case
 * and compare a word per pass; the word that differs is then rescanned a
 * byte at a time, because the answer is the difference of the first
 * differing pair of unsigned chars, not of the words holding them.
 */

  .text
  .global memcmp
  .type memcmp, @function

memcmp:
  ld.w    %r9,%r6
  xor     %r9,%r7
  and     %r9,3
  jrne    3f                        /* different alignments: byte at a time */
  cmp     %r8,8
  jrult   3f                        /* too short to pay for the setup */

1:                                  /* bring the pointers to a word */
  ld.w    %r9,%r6
  and     %r9,3
  jreq    2f
  ld.ub   %r4,[%r6]+
  ld.ub   %r5,[%r7]+
  cmp     %r4,%r5
  jrne    8f
  sub     %r8,1
  jp      1b

2:                                  /* at least four bytes are left here */
  ld.w    %r5,%r8
  and     %r5,3                     /* bytes trailing the last whole word */
  sub     %r8,%r5
  add     %r8,%r6                   /* end of the word run in s1 */
5:
  ld.w    %r9,[%r6]+
  ld.w    %r4,[%r7]+
  cmp     %r9,%r4
  jrne    7f                        /* the difference is inside this word */
  cmp     %r6,%r8
  jrne    5b
  ld.w    %r8,%r5

3:
  cmp     %r8,0
  jreq    6f
4:
  ld.ub   %r4,[%r6]+
  ld.ub   %r5,[%r7]+
  cmp     %r4,%r5
  jrne    8f
  sub     %r8,1
  jrne    4b
6:
  ld.w    %r4,0
  ret

7:                                  /* rescan the word that differed */
  sub     %r6,4
  sub     %r7,4
  ld.w    %r8,4
  jp      4b

8:
  sub     %r4,%r5
  ret
  .size memcmp, .-memcmp

#endif /* LIBC_BUILD_MEMCMP */
