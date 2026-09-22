/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_DELAY_H
#define _ASM_C33_DELAY_H
extern void __delay(unsigned long cycles);
extern void __udelay(unsigned long usecs);
extern void __ndelay(unsigned long nsecs);
#define udelay(n) __udelay(n)
#define ndelay(n) __ndelay(n)
#endif
