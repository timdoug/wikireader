/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_WIKIREADER_H
#define _ASM_C33_WIKIREADER_H

#include <linux/types.h>

unsigned long c33_mclk_hz(void);
void c33_lcd_init(void);
void c33_lcd_write(const char *text, size_t count);
void c33_lcd_checkpoint(unsigned int stage);
void c33_lcd_fault(unsigned int vector);
void c33_lcd_keyboard_init(void);
void c33_lcd_keyboard_press(int key, bool pressed);
bool c33_tty_inject_char(u8 ch);
void c33_touch_interrupt(void);

#endif
