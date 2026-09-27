/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_PLATFORM_DATA_DMA_S1C33_HSDMA_H
#define __LINUX_PLATFORM_DATA_DMA_S1C33_HSDMA_H

#include <linux/dmaengine.h>

/*
 * A request line: the channel and the trigger-source value that channel
 * takes for it (the manual's table II.1.5.1).  The SPI transmit request is
 * channel 2's trigger 9 and the SPI receive request channel 3's.
 */
#define HSDMA_REQUEST(channel, trigger)	((void *)(((channel) << 4) | (trigger)))
#define HSDMA_REQUEST_CHANNEL(request)	(((unsigned long)(request)) >> 4)
#define HSDMA_REQUEST_TRIGGER(request)	(((unsigned long)(request)) & 0xf)

struct s1c33_hsdma_platform_data {
	const struct dma_slave_map *slave_map;
	int slavecnt;
};

#endif
