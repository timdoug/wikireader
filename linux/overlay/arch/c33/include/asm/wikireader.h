/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_WIKIREADER_H
#define _ASM_C33_WIKIREADER_H

#define C33_GRIFO_TTBR 0x00000400UL

extern unsigned long c33_boot_ttbr;
extern int c33_grifo_booted;
void c33_lcd_init(void);
void c33_lcd_console_register(void);
void c33_lcd_checkpoint(unsigned int stage);
void c33_lcd_fault(unsigned int vector);

#endif
