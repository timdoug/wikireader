/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_CLOCK_H
#define _ASM_C33_CLOCK_H

#include <linux/bits.h>
#include <linux/types.h>

/* Gate bits in the clock-management unit's GATEDCLK1 register. */
#define C33_CMU_DMA	BIT(1)
#define C33_CMU_SPI	BIT(6)
#define C33_CMU_TM0	BIT(13)
#define C33_CMU_TM1	BIT(14)
#define C33_CMU_TM2	BIT(15)
#define C33_CMU_TM3	BIT(16)
#define C33_CMU_TM5	BIT(18)
#define C33_CMU_EFSIO	(BIT(5) | BIT(25))

unsigned long c33_mclk_hz(void);

/*
 * Turn peripheral gates on or off directly.  This exists for the timer block,
 * which time_init() brings up long before the common-clock framework has a
 * provider to get a clk from.  Anything that probes as a device should take a
 * clk instead and let the core count its users.
 */
void c33_cmu_gate(u32 mask, bool enable);

#endif
