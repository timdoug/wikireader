/****************************************************************************
 * boards/c33/s1c33e07/wikireader/src/wikireader.h
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

#ifndef __BOARDS_C33_S1C33E07_WIKIREADER_SRC_WIKIREADER_H
#define __BOARDS_C33_S1C33E07_WIKIREADER_SRC_WIKIREADER_H

#define WR_WIDTH       240
#define WR_HEIGHT      208
#define WR_STRIDE      32
#define WR_FBADDR      0x00080000
#define WR_TERM_HEIGHT 120
#define WR_KEY_HEIGHT  22

#ifdef CONFIG_WIKIREADER_BOOT_PROGRESS
void wikireader_progress(int row, int slot);
#else
#  define wikireader_progress(row, slot)
#endif

int wikireader_sdcard_initialize(void);
int wikireader_sdcard_start(void);
int wikireader_touch_initialize(void);
int wikireader_main(int argc, char **argv);

#endif
