/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_PLATFORM_DATA_SPI_S1C33_H
#define _LINUX_PLATFORM_DATA_SPI_S1C33_H

#include <linux/types.h>

struct spi_board_info;

struct s1c33_spi_platform_data {
	unsigned long (*get_clock_rate)(void);
	void (*set_cs)(unsigned int chip_select, bool high);
	void (*hold_clock)(bool hold, bool high);
	struct spi_board_info *devices;
	unsigned int num_devices;
};

#endif
