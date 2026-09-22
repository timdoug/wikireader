// SPDX-License-Identifier: GPL-2.0
#include <linux/clocksource/timer-s1c33.h>
#include <linux/init.h>
#include <linux/timekeeping.h>

#include <asm/irq.h>
#include <asm/wikireader.h>

void __init time_init(void)
{
	/*
	 * Clock providers are not up yet, so the timer takes the rate from
	 * the same hardware decoder the clock driver publishes later.
	 */
	s1c33_timer_init(c33_mclk_hz(), C33_IRQ_TIMER2, C33_IRQ_TIMER3);
	c33_lcd_checkpoint(3);
}

void read_persistent_clock64(struct timespec64 *ts)
{
	ts->tv_sec = 0;
	ts->tv_nsec = 0;
}
