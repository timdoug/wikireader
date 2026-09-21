/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_PLATFORM_DATA_SERIAL_S1C33_H
#define _LINUX_PLATFORM_DATA_SERIAL_S1C33_H

#include <linux/types.h>

struct s1c33_uart_platform_data {
	unsigned long clock_rate;
	unsigned int default_baud;
	u8 control;
};

#endif
