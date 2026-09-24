/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_PLATFORM_DATA_SPI_S1C33_H
#define _LINUX_PLATFORM_DATA_SPI_S1C33_H

#include <linux/types.h>

struct s1c33_spi_platform_data {
	void (*hold_clock)(bool hold, bool high);
	unsigned long dma_memory_start;
	unsigned long dma_memory_end;
};

#endif
