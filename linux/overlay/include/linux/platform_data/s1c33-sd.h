/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_PLATFORM_DATA_S1C33_SD_H
#define __LINUX_PLATFORM_DATA_S1C33_SD_H

#include <linux/types.h>

struct s1c33_sd_platform_data {
	/*
	 * Hold SCLK at a level on the pin while the controller is disabled
	 * and re-enabled: a disabled controller that was driving the pin
	 * leaves an edge on the wire, which the card counts as a clock.
	 */
	void (*hold_clock)(bool hold, bool high);
	/* The only memory HSDMA can reach. */
	unsigned long dma_memory_start;
	unsigned long dma_memory_end;
	/* Supply ramp after power-up, in milliseconds. */
	unsigned int powerup_msecs;
};

#endif
