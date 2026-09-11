/****************************************************************************
 * libs/libc/machine/c33/arch_strlen.S
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

#ifdef LIBC_BUILD_STRLEN

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* size_t strlen(const char *s)
 *
 * r6 = s, result in r4. After the leading bytes bring the pointer to a word
 * boundary, the scan tests four bytes at a time with the usual
 * (v - 0x01010101) & ~v & 0x80808080 zero-byte predicate, then walks the
 * word that tripped it. Reading whole words cannot cross out of the string's
 * own word, so it stays inside the same mapping.
 */

  .text
  .global strlen
  .type strlen, @function

strlen:
  ld.w    %r4,%r6                   /* remember where the string starts */

1:                                  /* leading bytes up to a word boundary */
  ld.w    %r9,%r6
  and     %r9,3
  jreq    2f
  ld.ub   %r9,[%r6]
  cmp     %r9,0
  jreq    4f
  add     %r6,1
  jp      1b

2:
  xld.w   %r5,0x01010101
  xld.w   %r7,0x80808080
3:
  ld.w    %r9,[%r6]+
  ld.w    %r8,%r9
  sub     %r8,%r5
  not     %r9,%r9
  and     %r8,%r9
  and     %r8,%r7
  jreq    3b
  sub     %r6,4                     /* the zero is in the word just read */

5:
  ld.ub   %r9,[%r6]
  cmp     %r9,0
  jreq    4f
  add     %r6,1
  jp      5b

4:
  sub     %r6,%r4
  ld.w    %r4,%r6
  ret
  .size strlen, .-strlen

#endif /* LIBC_BUILD_STRLEN */
