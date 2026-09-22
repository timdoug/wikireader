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

#define CLOCK_TICK_RATE 48000000

#endif
