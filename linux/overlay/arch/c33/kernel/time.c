// SPDX-License-Identifier: GPL-2.0
#include <linux/clk-provider.h>
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/timekeeping.h>

void __init time_init(void)
{
	/* The clock-management unit first: the timer takes its rate from MCLK. */
	of_clk_init(NULL);
	timer_probe();
}

void read_persistent_clock64(struct timespec64 *ts)
{
	ts->tv_sec = 0;
	ts->tv_nsec = 0;
}
