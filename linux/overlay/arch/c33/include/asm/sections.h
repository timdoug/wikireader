/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_SECTIONS_H
#define _ASM_C33_SECTIONS_H

#include <asm-generic/sections.h>

/* The code copied into A0 RAM at boot: asm/iram.h. */
extern char __iram_text_start[], __iram_text_end[];

#endif /* _ASM_C33_SECTIONS_H */
