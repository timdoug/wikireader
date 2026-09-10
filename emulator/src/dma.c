/*
 * S1C33E07 intelligent DMA and high-speed DMA controller.
 *
 * HSDMA3 drains SPI RX; IDMA0x24 supplies clocks after receive completion.
 * The HSDMA2 alternative supplies TXD at shift start, pipelining the next
 * word through the SPI transmit buffer (manual II.1.5 and V.2.5).
 *
 * Each documented memory/I/O access consumes at least one CPU-AHB clock.
 * The memory timing callback adds SDRAMC queue, row, refresh, and shared-bus
 * waits before that minimum bus phase.
 */

#include <string.h>

#include "cmu.h"
#include "dma.h"
#include "model.h"
#include "itc.h"
#include "sdcard.h"

#define IDMA_BASE_LO  0x00u
#define IDMA_START    0x04u
#define IDMA_ENABLE   0x05u

#define HS_FIRST      0x20u
#define HS_STRIDE     0x10u
#define HS_CNT(ch)    (HS_FIRST + HS_STRIDE * (ch))
#define HS_CTL(ch)    (HS_CNT(ch) + 0x02u)
#define HS_SLO(ch)    (HS_CNT(ch) + 0x04u)
#define HS_SHI(ch)    (HS_CNT(ch) + 0x06u)
#define HS_DLO(ch)    (HS_CNT(ch) + 0x08u)
#define HS_DHI(ch)    (HS_CNT(ch) + 0x0au)
#define HS_EN(ch)     (HS_CNT(ch) + 0x0cu)
#define HS_TF(ch)     (HS_CNT(ch) + 0x0eu)

#define HS_ADV_CTL(ch) (0x62u + HS_STRIDE * (ch))
#define HS_ADV_SRC(ch) (0x64u + HS_STRIDE * (ch))
#define HS_ADV_DST(ch) (0x68u + HS_STRIDE * (ch))
#define HS_MODE        0x9cu

/* Interrupt-controller offsets relative to ITC_BASE. */
#define ITC_FHDMA       0x81u
#define ITC_FSPI        0x89u
#define ITC_HSTRIG23    0x99u
#define ITC_IDMAREQ_SPI 0x9bu
#define ITC_IDMAEN_SPI  0x9cu

static uint16_t get16(const struct dma *d, unsigned off)
{
	return (uint16_t)d->reg[off] | (uint16_t)d->reg[off + 1] << 8;
}

static uint32_t get32(const struct dma *d, unsigned off)
{
	return (uint32_t)get16(d, off) | (uint32_t)get16(d, off + 2) << 16;
}

static void put16(struct dma *d, unsigned off, uint16_t v)
{
	d->reg[off] = (uint8_t)v;
	d->reg[off + 1] = (uint8_t)(v >> 8);
}

static void put32(struct dma *d, unsigned off, uint32_t v)
{
	put16(d, off, (uint16_t)v);
	put16(d, off + 2, (uint16_t)(v >> 16));
}

static uint32_t hs_count(const struct dma *d, unsigned ch)
{
	return ((uint32_t)(get16(d, HS_CTL(ch)) & 0xff) << 16) |
	       get16(d, HS_CNT(ch));
}

static void hs_set_count(struct dma *d, unsigned ch, uint32_t count)
{
	uint16_t ctl = get16(d, HS_CTL(ch));
	put16(d, HS_CNT(ch), (uint16_t)count);
	put16(d, HS_CTL(ch), (ctl & 0xff00u) | ((count >> 16) & 0xffu));
}

static unsigned transfer_size(unsigned size_code)
{
	return size_code == 0 ? 1u : size_code == 1 ? 2u : 4u;
}

static uint32_t advance(uint32_t addr, unsigned mode, unsigned size)
{
	if (mode == 1 || mode == 4)
		return addr - size;
	if (mode == 2 || mode == 3)
		return addr + size;
	return addr;
}

static uint32_t dma_read(struct dma *d, uint32_t addr, unsigned size)
{
	if (d->clock) {
		if (*d->clock < d->bus_available)
			*d->clock = d->bus_available;
		*d->clock += mem_wait(d->mem, MEM_DMA_READ, addr, size, *d->clock);
		++*d->clock;
	}
	d->bus_cycles++;
	return mem_read(d->mem, addr, size);
}

static void dma_write(struct dma *d, uint32_t addr, unsigned size,
		      uint32_t value)
{
	if (d->clock) {
		*d->clock += mem_wait(d->mem, MEM_DMA_WRITE, addr, size, *d->clock);
		++*d->clock;
	}
	d->bus_cycles++;
	mem_write(d->mem, addr, size, value);
}

static bool valid_dual_address(uint32_t addr, unsigned size);

static bool hs_transfer(struct dma *d, unsigned ch, bool spi)
{
	uint16_t ctl, shi, dhi, adv;
	uint32_t count, src, dst, initial_src, initial_dst, units;
	unsigned size, smode, dmode, mode;
	bool advanced, reset_src, reset_dst;

	if (!(get16(d, HS_EN(ch)) & 1) || !cmu_dma_enabled(d->cmu))
		return false;

	ctl = get16(d, HS_CTL(ch));
	if (!(ctl & 0x8000u))
		return false; /* single-address external bus mode is not on this board */

	shi = get16(d, HS_SHI(ch));
	dhi = get16(d, HS_DHI(ch));
	adv = get16(d, HS_ADV_CTL(ch));
	advanced = (get16(d, HS_MODE) & 1) != 0;
	mode = dhi >> 14;
	if (mode == 3)
		return false;
	/* Unlimited bus ownership is modeled synchronously: the CPU cannot
	 * make an AHB access until this trigger completes (II.1.3.2). Limited
	 * bursts need CPU/DMA arbitration, not an invented fixed delay. */
	if (mode && (get16(d, 0x9eu) & 15u))
		return false;
	size = transfer_size(advanced && (adv & 1) ? 2 :
			     (shi & 0x4000u) ? 1 : 0);
	smode = (shi >> 12) & 3;
	dmode = (dhi >> 12) & 3;
	reset_src = smode == 2 || (advanced && (adv & 0x10u));
	reset_dst = dmode == 2 || (advanced && (adv & 0x20u));
	if (advanced && (adv & 0x10u))
		smode = 1; /* decrement with initialisation; same per-unit step */
	if (advanced && (adv & 0x20u))
		dmode = 1;

	if (advanced) {
		src = get32(d, HS_ADV_SRC(ch));
		dst = get32(d, HS_ADV_DST(ch));
	} else {
		src = get16(d, HS_SLO(ch)) | (uint32_t)(shi & 0x0fffu) << 16;
		dst = get16(d, HS_DLO(ch)) | (uint32_t)(dhi & 0x0fffu) << 16;
	}

	initial_src = src;
	initial_dst = dst;
	count = hs_count(d, ch);
	units = mode == 1 ? (count ? count : 0x1000000u) :
		mode == 2 ? ((count & 255u) ? (count & 255u) : 256u) : 1u;
	d->reg[HS_TF(ch)] = 0;
	while (units--) {
		if (!valid_dual_address(src, size) || !valid_dual_address(dst, size) ||
		    ((src | dst) & (size - 1)))
			return false;
		/* One read followed by one write, NOT a block-sized staging FIFO.
		 * Each access goes through the SDRAMC's row/queue/refresh model. */
		uint32_t value = dma_read(d, src, size);
		dma_write(d, dst, size, value);
		if (d->clock) {
			*d->clock += spi ? model.dma_extra : model.dma_mem_extra;
			d->bus_available = *d->clock;
		}
		d->hsdma_transfers++;
		d->hsdma_channel_transfers[ch]++;
		src = advance(src, smode, size);
		dst = advance(dst, dmode, size);
		if (mode != 2)
			count = (count - 1) & 0xffffffu;
	}
	if (mode == 2)
		count = (((count >> 8) - 1) & 0xffffu) << 8 | (count & 255u);
	if (mode && reset_src) src = initial_src;
	if (mode && reset_dst) dst = initial_dst;
	if (advanced) {
		put32(d, HS_ADV_SRC(ch), src);
		put32(d, HS_ADV_DST(ch), dst);
	} else {
		put16(d, HS_SLO(ch), (uint16_t)src);
		put16(d, HS_SHI(ch), (shi & 0xf000u) | ((src >> 16) & 0xfffu));
		put16(d, HS_DLO(ch), (uint16_t)dst);
		put16(d, HS_DHI(ch), (dhi & 0xf000u) | ((dst >> 16) & 0xfffu));
	}

	hs_set_count(d, ch, count);
	if ((mode == 2 ? count >> 8 : count) == 0) {
		put16(d, HS_EN(ch), 0);
		d->itc->reg[ITC_FHDMA] |= (uint8_t)(1u << ch);
	}
	return true;
}

static bool valid_descriptor(uint32_t addr)
{
	bool ram = (addr >= DSTRAM_BASE &&
		    addr - DSTRAM_BASE <= DSTRAM_SIZE - 16u) ||
		   (addr >= SDRAM_BASE &&
		    addr - SDRAM_BASE <= SDRAM_SIZE - 16u);
	return ram && !(addr & 0x0fu);
}

static bool valid_dual_address(uint32_t addr, unsigned size)
{
	/* Area 0 and Area 2 are forbidden for IDMA/HSDMA dual transfers. */
	return (addr >= IVRAM_BASE && addr - IVRAM_BASE <= IVRAM_SIZE - size) ||
	       (addr >= DSTRAM_BASE && addr - DSTRAM_BASE <= DSTRAM_SIZE - size) ||
	       (addr >= SDRAM_BASE && addr - SDRAM_BASE <= SDRAM_SIZE - size) ||
	       (addr >= REG_BASE && addr - REG_BASE <= REG_SIZE - size);
}

static bool idma_transfer(struct dma *d, unsigned channel, bool *terminal)
{
	uint32_t base, desc, ctl, count, src, dst, value;
	unsigned size, smode, dmode, transfer_mode;

	*terminal = false;
	if (!(d->reg[IDMA_ENABLE] & 1) || !cmu_dma_enabled(d->cmu))
		return false;

	base = get32(d, IDMA_BASE_LO);
	desc = base + channel * 16u;
	if (!valid_descriptor(desc)) {
		d->invalid_descriptors++;
		return false;
	}

	/* Single-transfer IDMA loads all four control words on every trigger. */
	ctl = dma_read(d, desc, 4);
	count = dma_read(d, desc + 4, 4);
	src = dma_read(d, desc + 8, 4);
	dst = dma_read(d, desc + 12, 4);
	size = transfer_size((ctl >> 16) & 3);
	smode = (ctl >> 12) & 7;
	dmode = (ctl >> 8) & 7;
	transfer_mode = (ctl >> 4) & 3;
	if (transfer_mode == 3 || ((ctl >> 16) & 3) == 3)
		return false;
	if (!valid_dual_address(src, size) || !valid_dual_address(dst, size))
		return false;

	/* The dormant WikiReader path uses one transfer for each SPI request. */
	if (transfer_mode != 0)
		return false;
	value = dma_read(d, src, size);
	dma_write(d, dst, size, value);
	if (d->clock)
		*d->clock += model.dma_extra;
	d->idma_transfers++;
	src = advance(src, smode, size);
	dst = advance(dst, dmode, size);
	count--;

	/* The unchanged control word is also part of the four-word writeback. */
	dma_write(d, desc, 4, ctl);
	dma_write(d, desc + 4, 4, count);
	dma_write(d, desc + 8, 4, src);
	dma_write(d, desc + 12, 4, dst);
	if (d->clock)
		d->bus_available = *d->clock;
	*terminal = count == 0;
	return true;
}

static void service_spi_idma(struct dma *d)
{
	bool terminal;

	if (!(d->itc->reg[ITC_FSPI] & 0x10u) ||
	    !(d->itc->reg[ITC_IDMAREQ_SPI] & 0x10u) ||
	    !(d->itc->reg[ITC_IDMAEN_SPI] & 0x10u) ||
	    !idma_transfer(d, 0x24u, &terminal))
		return;

	/* Hardware IDMA acknowledges the receive cause after a transfer. */
	d->itc->reg[ITC_FSPI] &= (uint8_t)~0x10u;
	if (terminal) {
		uint32_t desc = get32(d, IDMA_BASE_LO) + 0x240u;
		uint32_t ctl = mem_read(d->mem, desc, 4);
		if (!(ctl & 1u))
			d->itc->reg[ITC_IDMAEN_SPI] &= (uint8_t)~0x10u;
		else
			d->itc->reg[ITC_IDMAREQ_SPI] &= (uint8_t)~0x10u;
	}
}

static void service_spi(struct dma *d)
{
	if (d->servicing)
		return;
	d->servicing = true;

	do {
		unsigned requests = d->spi_event_pending;
		d->spi_event_pending = 0;
		d->itc->reg[ITC_FSPI] |= (uint8_t)requests;
		for (unsigned ch = 2; ch <= 3; ch++) {
			unsigned select = (d->itc->reg[ITC_HSTRIG23] >>
					   (4 * (ch & 1))) & 15;
			if (select == 9 &&
			    (requests & (ch == 2 ? SPI_DMA_TX : SPI_DMA_RX)))
				d->reg[HS_TF(ch)] = 1;
		}
		/* Hardware priority is HSDMA Ch.0 > ... > Ch.3 > hardware IDMA. */
		for (unsigned ch = 0; ch < 4; ch++) {
			unsigned selector = ch < 2 ? d->itc->reg[0x98u] :
						      d->itc->reg[ITC_HSTRIG23];
			selector = (selector >> (4 * (ch & 1u))) & 0x0fu;
			bool spi = (ch == 2 || ch == 3) && selector == 9;
			if (!spi && selector != 0)
				continue;
			if (d->reg[HS_TF(ch)] & 1)
				hs_transfer(d, ch, spi);
		}

		/* SPI receive DMA maps to hardware IDMA channel 0x24. */
		service_spi_idma(d);
	} while (d->spi_event_pending);
	d->servicing = false;
}

static void spi_event(void *ctx, unsigned requests)
{
	struct dma *d = ctx;
	d->spi_event_pending |= requests;
	service_spi(d);
}

static void software_event(void *ctx, unsigned channels)
{
	struct dma *d = ctx;
	for (unsigned ch = 0; ch < 4; ch++) {
		unsigned selector = d->itc->reg[ch < 2 ? 0x98u : ITC_HSTRIG23];
		selector = (selector >> (4 * (ch & 1u))) & 15u;
		if ((channels & (1u << ch)) && selector == 0)
			d->reg[HS_TF(ch)] = 1;
	}
	service_spi(d);
}

static bool dma_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
		     bool is_write)
{
	struct dma *d = ctx;
	unsigned i = off - DMA_BASE;

	if (i + size > DMA_LEN)
		return false;
	if (!is_write) {
		*val = 0;
		for (unsigned k = 0; k < size; k++)
			*val |= (uint32_t)d->reg[i + k] << (8 * k);
		return true;
	}

	/* Trigger flags are write-one-to-clear rather than ordinary storage. */
	for (unsigned ch = 0; ch < 4; ch++) {
		if (i == HS_TF(ch)) {
			if (*val & 1)
				d->reg[i] = 0;
			return true;
		}
	}

	/* The base register is immutable while global IDMA is enabled. */
	if (i < 4 && (d->reg[IDMA_ENABLE] & 1))
		return true;
	for (unsigned k = 0; k < size; k++)
		d->reg[i + k] = (uint8_t)(*val >> (8 * k));
	for (unsigned ch = 0; ch < 4; ch++) {
		if (i == HS_EN(ch) && (*val & 1u))
			service_spi(d); /* II.1.5: disabled channels retain trigger flags */
	}

	/* A software start transfers the selected channel when globally enabled. */
	if (i == IDMA_START && (*val & 0x80u)) {
		unsigned channel = *val & 0x7fu;
		bool terminal;
		idma_transfer(d, channel, &terminal);
		d->reg[IDMA_START] = channel;
	}
	/* A hardware request received while disabled remains pending. */
	if (i == IDMA_ENABLE && (*val & 1u)) {
		if (!d->servicing) {
			d->servicing = true;
			service_spi_idma(d);
			d->servicing = false;
			service_spi(d);
		}
	}
	return true;
}

void dma_reset(struct dma *d)
{
	struct mem *m = d->mem;
	struct itc *itc = d->itc;
	const struct cmu *cmu = d->cmu;
	uint64_t *clock = d->clock;
	memset(d, 0, sizeof *d);
	d->mem = m;
	d->itc = itc;
	d->cmu = cmu;
	d->clock = clock;
	put32(d, IDMA_BASE_LO, 0x200003a0u);
}

void dma_set_clock(struct dma *d, uint64_t *clock)
{
	d->clock = clock;
}

void dma_attach(struct mem *m, struct dma *d, struct itc *itc,
		const struct cmu *cmu, struct sdcard *sd)
{
	memset(d, 0, sizeof *d);
	d->mem = m;
	d->itc = itc;
	d->cmu = cmu;
	dma_reset(d);
	itc->hsdma_trigger = software_event;
	itc->hsdma_ctx = d;
	mem_add_mmio(m, "dma", DMA_BASE, DMA_LEN, dma_mmio, d);
	sd_set_dma_event(sd, spi_event, d);
}
