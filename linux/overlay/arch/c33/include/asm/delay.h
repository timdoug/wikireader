/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_DELAY_H
#define _ASM_C33_DELAY_H
extern void __delay(unsigned long loops);
extern void __udelay(unsigned long usecs);
#define udelay(n) __udelay(n)
#endif
