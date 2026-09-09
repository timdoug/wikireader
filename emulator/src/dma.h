#ifndef DMA_H
#define DMA_H

#include <stdbool.h>
#include <stdint.h>

#include "mem.h"

struct cmu;
struct itc;
struct sdcard;

/* IDMA and HSDMA registers, REG_BASE+0x1100..0x119f. */
#define DMA_BASE 0x1100u
#define DMA_LEN  0x00a0u

struct dma {
	struct mem    *mem;
	struct itc    *itc;
	const struct cmu *cmu;
	uint64_t *clock;
	uint8_t reg[DMA_LEN];

	bool servicing;
	unsigned spi_event_pending;
	uint64_t bus_available; /* wire events may precede the last DMA bus release */

	unsigned long hsdma_transfers;
	unsigned long hsdma_channel_transfers[4];
	unsigned long idma_transfers;
	unsigned long invalid_descriptors;
	unsigned long long bus_cycles;
};

void dma_attach(struct mem *m, struct dma *d, struct itc *itc,
		const struct cmu *cmu, struct sdcard *sd);
void dma_reset(struct dma *d);
void dma_set_clock(struct dma *d, uint64_t *clock);

#endif /* DMA_H */
