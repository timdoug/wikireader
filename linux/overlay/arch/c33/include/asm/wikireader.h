/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_WIKIREADER_H
#define _ASM_C33_WIKIREADER_H

#include <linux/types.h>

void c33_lcd_init(void);
void c33_lcd_write(const char *text, size_t count);
void c33_lcd_checkpoint(unsigned int stage);
void c33_lcd_fault(unsigned int vector);

#endif
