/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_TIMEX_H
#define _ASM_C33_TIMEX_H

#include <linux/clocksource/timer-s1c33.h>

typedef unsigned long cycles_t;

static inline cycles_t get_cycles(void)
{
	return s1c33_timer_cycles();
}
#define get_cycles get_cycles

/*
 * get_cycles() is 0 until the timer block's counter runs, part way into
 * the boot; the random pool takes the clocksource there is until then, as
 * MIPS does without a counter.
 */
static inline unsigned long random_get_entropy(void)
{
	cycles_t cycles = get_cycles();

	return cycles ?: random_get_entropy_fallback();
}
#define random_get_entropy random_get_entropy

/*
 * Only LATCH and the jiffy conversions derive from this, and both come out
 * exact at any multiple of HZ; the machine itself runs at 48 MHz from reset
 * and 60 MHz once a launcher has started the PLL.
 */
#define CLOCK_TICK_RATE 48000000

#endif
