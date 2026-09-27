/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_PLATFORM_DATA_S1C33_SD_H
#define __LINUX_PLATFORM_DATA_S1C33_SD_H

#include <linux/types.h>

struct s1c33_sd_platform_data {
	/* Supply ramp after power-up, in milliseconds. */
	unsigned int powerup_msecs;
};

#endif
