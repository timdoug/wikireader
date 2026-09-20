// SPDX-License-Identifier: GPL-2.0
#include <linux/delay.h>
#include <linux/export.h>

void __delay(unsigned long loops)
{
	while (loops--)
		__asm__ volatile ("nop");
}
EXPORT_SYMBOL(__delay);

void __udelay(unsigned long usecs)
{
	__delay(usecs * (loops_per_jiffy / (1000000 / HZ)));
}
EXPORT_SYMBOL(__udelay);
