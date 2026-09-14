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
 *
 * When the two pointers disagree about where a word boundary falls, no
 * amount of skipping ahead will line them up, and the obvious thing is to
 * give up and go a byte at a time. That costs the device about four times
 * the memory traffic and it shows: 8.6 MB/s aligned against 3.6 mismatched,
 * measured by arch_libctest on hardware. So read whole words from the source
 * and cut each output word out of two of them instead -- the technique
 * mini-libc's bcopy.c already uses on the reader side. Nine instructions
 * move four bytes, and the source is read once.
 */

  .text
  .global memcpy
  .type memcpy, @function

memcpy:
  ld.w    %r4,%r6                   /* the return value is the destination */
  ld.w    %r9,%r6
  xor     %r9,%r7
  and     %r9,3
  jrne    7f                        /* different alignments: merge words */
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

/* The pointers cannot be brought into agreement. Sixteen bytes is the floor:
 * below it the setup costs more than the byte loop, and the word count below
 * needs room to give the tail a whole word back.
 */

7:
  cmp     %r8,16
  jrult   3b

8:                                  /* bring the destination to a word */
  ld.w    %r9,%r6
  and     %r9,3
  jreq    9f
  ld.ub   %r9,[%r7]+
  ld.b    [%r6]+,%r9
  sub     %r8,1
  jp      8b

9:
  ld.w    %r13,%r7
  and     %r13,3                    /* where the source sits inside its word */
  sub     %r7,%r13                  /* round the source down to that word */
  ld.w    %r9,%r13                  /* kept: the tail starts there too */
  ld.w    %r10,%r13
  sll     %r10,3                    /* bits of it belonging to the word before */
  ld.w    %r11,4
  sub     %r11,%r13
  sll     %r11,3                    /* and bits belonging to the word after */
  ld.w    %r5,[%r7]+                /* the first of the pair */

  /* Stop a whole word early. The last pass reads the word after the last one
   * it needs a byte from, which would run off the end of the source if the
   * tail were any shorter; the byte loop picks those bytes up instead. With
   * at least sixteen bytes here the loop still always runs.
   */

  sub     %r8,4
  ld.w    %r13,%r8
  and     %r13,3
  sub     %r8,%r13                  /* bytes the merging loop will write */

  /* Only one register is free across the loop, and two numbers have to
   * survive it. Both are small -- the offset is 1 to 3 and the tail 4 to 7
   * -- so they travel in one.
   */

  add     %r13,4                    /* the tail the byte loop is left */
  sll     %r13,4
  or      %r9,%r13
  add     %r8,%r6                   /* where the destination stops */

10:
  ld.w    %r12,[%r7]+               /* the second of the pair */
  srl     %r5,%r10                  /* what the first contributes, low */
  ld.w    %r13,%r12
  sll     %r13,%r11                 /* what the second contributes, high */
  or      %r5,%r13
  ld.w    [%r6]+,%r5
  ld.w    %r5,%r12                  /* the second becomes the first */
  cmp     %r6,%r8
  jrne    10b

  sub     %r7,4                     /* back to the word the tail starts in */
  ld.w    %r13,%r9
  and     %r13,3
  add     %r7,%r13                  /* and to the byte inside it */
  ld.w    %r8,%r9
  srl     %r8,4                     /* what the byte loop still owes */
  jp      3b
  .size memcpy, .-memcpy

#endif /* LIBC_BUILD_MEMCPY */
