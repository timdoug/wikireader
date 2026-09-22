/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CLKSOURCE_TIMER_S1C33_H
#define __CLKSOURCE_TIMER_S1C33_H

#include <linux/init.h>
#include <linux/types.h>

void __init s1c33_timer_init(unsigned long mclk_hz, int event_irq,
			     int wake_irq);

/* Free-running 32-bit MCLK count; zero until the timer block is running. */
u32 s1c33_timer_cycles(void);
bool s1c33_timer_running(void);

#endif /* __CLKSOURCE_TIMER_S1C33_H */
