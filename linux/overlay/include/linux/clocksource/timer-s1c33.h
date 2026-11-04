/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CLKSOURCE_TIMER_S1C33_H
#define __CLKSOURCE_TIMER_S1C33_H

#include <linux/init.h>
#include <linux/types.h>

/* Free-running 32-bit MCLK count; zero until the counter is running. */
u32 s1c33_timer_cycles(void);
/* Whether it is: from the timer block's probe, early in the initcalls. */
bool s1c33_timer_running(void);
/* The count's rate, MCLK's; zero before time_init(). */
unsigned long s1c33_timer_rate(void);

#endif /* __CLKSOURCE_TIMER_S1C33_H */
