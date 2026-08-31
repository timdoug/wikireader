/*
 * S1C33E07 intelligent DMA and high-speed DMA controller.
 *
 * The WikiReader's dormant SD driver uses one precisely documented pipeline:
 * HSDMA Ch.3 drains SPI RX into a buffer, while IDMA Ch.0x24 writes a fixed
 * 0xff byte to SPI TX after each receive request to generate the next clock.
 * HSDMA Ch.2 performs the inverse memory-to-SPI path for block writes.
 *
 * Each documented memory/I/O access consumes at least one CPU-AHB clock.
 * SDRAM and external-memory wait states are deliberately not folded into
 * that minimum; the controller-specific timing model can add them later.
 */

#include <string.h>

#include "cmu.h"
#include "dma.h"
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
	if (d->clock)
		++*d->clock;
	d->bus_cycles++;
	return mem_read(d->mem, addr, size);
}

static void dma_write(struct dma *d, uint32_t addr, unsigned size,
		      uint32_t value)
{
	if (d->clock)
		++*d->clock;
	d->bus_cycles++;
	mem_write(d->mem, addr, size, value);
}

static bool valid_dual_address(uint32_t addr, unsigned size);

static bool hs_transfer(struct dma *d, unsigned ch)
{
	uint16_t ctl, shi, dhi, adv;
	uint32_t count, src, dst, value;
	unsigned size, smode, dmode;

	if (!(get16(d, HS_EN(ch)) & 1) || !cmu_dma_enabled(d->cmu))
		return false;

	ctl = get16(d, HS_CTL(ch));
	if (!(ctl & 0x8000u))
		return false; /* single-address external bus mode is not on this board */

	shi = get16(d, HS_SHI(ch));
	dhi = get16(d, HS_DHI(ch));
	adv = get16(d, HS_ADV_CTL(ch));
	size = transfer_size((get16(d, HS_MODE) & 1) && (adv & 1) ? 2 :
			     (shi & 0x4000u) ? 1 : 0);
	smode = (shi >> 12) & 3;
	dmode = (dhi >> 12) & 3;
	if ((get16(d, HS_MODE) & 1) && (adv & 0x10u))
		smode = 1; /* decrement with initialisation; same per-unit step */
	if ((get16(d, HS_MODE) & 1) && (adv & 0x20u))
		dmode = 1;

	if (get16(d, HS_MODE) & 1) {
		src = get32(d, HS_ADV_SRC(ch));
		dst = get32(d, HS_ADV_DST(ch));
	} else {
		src = get16(d, HS_SLO(ch)) | (uint32_t)(shi & 0x0fffu) << 16;
		dst = get16(d, HS_DLO(ch)) | (uint32_t)(dhi & 0x0fffu) << 16;
	}

	/* The E07 forbids Area 0/2 for dual-address transfers. */
	if (!valid_dual_address(src, size) || !valid_dual_address(dst, size))
		return false;

	d->reg[HS_TF(ch)] = 0;
	/* Dual-address HSDMA is one source and one destination bus phase. */
	value = dma_read(d, src, size);
	dma_write(d, dst, size, value);
	d->hsdma_transfers++;

	src = advance(src, smode, size);
	dst = advance(dst, dmode, size);
	if (get16(d, HS_MODE) & 1) {
		put32(d, HS_ADV_SRC(ch), src);
		put32(d, HS_ADV_DST(ch), dst);
	}

	count = (hs_count(d, ch) - 1) & 0x00ffffffu;
	hs_set_count(d, ch, count);
	if (count == 0) {
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
	d->idma_transfers++;
	src = advance(src, smode, size);
	dst = advance(dst, dmode, size);
	count--;

	/* The unchanged control word is also part of the four-word writeback. */
	dma_write(d, desc, 4, ctl);
	dma_write(d, desc + 4, 4, count);
	dma_write(d, desc + 8, 4, src);
	dma_write(d, desc + 12, 4, dst);
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

	while (d->spi_event_pending) {
		d->spi_event_pending = false;
		/* Every completed byte asserts receive-full and transmit-empty. */
		d->itc->reg[ITC_FSPI] |= 0x30u;

		/* Hardware priority is HSDMA Ch.0 > ... > Ch.3 > hardware IDMA. */
		for (unsigned ch = 0; ch < 4; ch++) {
			unsigned selector = ch < 2 ? d->itc->reg[0x98u] :
						      d->itc->reg[ITC_HSTRIG23];
			selector = (selector >> (4 * (ch & 1u))) & 0x0fu;
			bool spi = (ch == 2 || ch == 3) && selector == 9;
			if (!spi)
				continue;
			d->reg[HS_TF(ch)] = 1;
			hs_transfer(d, ch);
		}

		/* SPI receive DMA maps to hardware IDMA channel 0x24. */
		service_spi_idma(d);
	}
	d->servicing = false;
}

static void spi_event(void *ctx)
{
	struct dma *d = ctx;
	d->spi_event_pending = true;
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
	mem_add_mmio(m, "dma", DMA_BASE, DMA_LEN, dma_mmio, d);
	sd_set_dma_event(sd, spi_event, d);
}
