// SPDX-License-Identifier: GPL-2.0
/*
 * Delays are counted in MCLK cycles read from the 32-bit timer rather than
 * by spinning a loop: the C33 fetches instructions through the same SDRAM
 * rows as data, so the cost of a loop depends on where the linker happened to
 * place it.  loops_per_jiffy is therefore cycles per jiffy, not iterations.
 */
#include <linux/clocksource/timer-s1c33.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/processor.h>

#include <asm/wikireader.h>

static unsigned long cycles_per_usec(void)
{
	static unsigned long cached;

	/* MCLK is selected before the kernel starts and does not change. */
	if (!cached)
		cached = c33_mclk_hz() / 1000000;
	return cached;
}

void __delay(unsigned long cycles)
{
	u32 start;

	if (!s1c33_timer_running()) {
		/* Before time_init(): bounded, and only ever an overestimate. */
		while (cycles--)
			cpu_relax();
		return;
	}

	start = s1c33_timer_cycles();
	while (s1c33_timer_cycles() - start < cycles)
		cpu_relax();
}
EXPORT_SYMBOL(__delay);

void __udelay(unsigned long usecs)
{
	__delay(usecs * cycles_per_usec());
}
EXPORT_SYMBOL(__udelay);

void __ndelay(unsigned long nsecs)
{
	__delay(DIV_ROUND_UP(nsecs * cycles_per_usec(), 1000));
}
EXPORT_SYMBOL(__ndelay);

void __init calibrate_delay(void)
{
	loops_per_jiffy = c33_mclk_hz() / HZ;
	pr_info("Delay loop: %lu MCLK cycles per jiffy\n", loops_per_jiffy);
}
