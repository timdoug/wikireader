/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_SECTIONS_H
#define _ASM_C33_SECTIONS_H

#include <asm-generic/sections.h>

/* A0 RAM's sections, where they run (asm/iram.h). */
extern char __a0_text_start[], __a0_text_end[];
extern char __a0_data_start[], __a0_data_end[];
/* The position-independent part of .a0.text: asm/iram.h. */
extern char __iram_text_start[], __iram_text_end[];

#endif /* _ASM_C33_SECTIONS_H */
