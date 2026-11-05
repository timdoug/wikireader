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

#define ASYNC_DEPTH 4

struct dma {
	struct mem    *mem;
	struct sdcard *sd;
	struct itc    *itc;
	const struct cmu *cmu;
	uint64_t *clock;
	const bool *cpu_sleeping;
	uint8_t reg[DMA_LEN];

	bool servicing;
	unsigned spi_event_pending;
	/* model.dma_async: SPI transfers whose writes have yet to land. */
	struct async_xfer {
		uint64_t due;
		uint64_t write_due; /* pending HSDMA destination, separate from source response */
		uint64_t asked;	/* when the write first wanted the bus */
		uint32_t src, dst, value;
		bool read_pending; /* HSDMA source has not reached its response yet */
		unsigned size, ch;
		bool done;	/* the channel's last: raise its flag */
		bool write_started; /* destination timing already reserved */
		/* IDMA runs its ten bus phases on the shared event clock. */
		bool idma, idma_hardware;
		unsigned idma_phase;
		uint32_t idma_desc, idma_words[4];
		uint64_t idma_start, idma_generation;
	} q[ASYNC_DEPTH];
	unsigned qn;
	uint64_t engine_free;
	uint64_t spi_rx_generation; /* preserve causes arriving during IDMA writeback */
	uint64_t phase_busy_until; /* actual active AHB phase, independent of fitted idle time */
	uint64_t trig_at[4];	/* when each SPI channel's request came */
	uint64_t room_at;	/* when the queue last had room again */
	bool polling;
	unsigned long long async_delay;	/* cycles the CPU held them up */
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
void dma_set_cpu_sleeping(struct dma *d, const bool *sleeping);
void dma_poll(struct dma *d);

#endif /* DMA_H */
