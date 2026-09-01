#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/cmu.h"
#include "../src/dma.h"
#include "../src/itc.h"
#include "../src/mem.h"
#include "../src/port.h"
#include "../src/sdcard.h"

#define REG(a) (REG_BASE + (a))
#define SPI_CTL1_8BIT_MASTER_DMA ((7u << 10) | (1u << 3) | (1u << 2) | \
				  (1u << 1) | 1u)

static void setup_read(struct mem *m, uint32_t base, uint32_t dst,
		       bool global_enable)
{
	mem_write(m, REG(0x1708), 4, SPI_CTL1_8BIT_MASTER_DMA);
	mem_write(m, REG(0x1710), 4, 0);

	/* HSDMA Ch.3: 512 byte, dual address, SPI RX -> incrementing RAM. */
	mem_write(m, REG(0x119c), 2, 1);
	mem_write(m, REG(0x1192), 2, 0);
	mem_write(m, REG(0x1150), 2, 512);
	mem_write(m, REG(0x1152), 2, 0x8000);
	mem_write(m, REG(0x1154), 2, 0);
	mem_write(m, REG(0x1156), 2, 0);
	mem_write(m, REG(0x1158), 2, 0);
	mem_write(m, REG(0x115a), 2, 0x2000);
	mem_write(m, REG(0x1194), 4, REG(0x1700));
	mem_write(m, REG(0x1198), 4, dst);
	mem_write(m, REG(0x0299), 1, 0x90);
	mem_write(m, REG(0x115c), 2, 1);

	/* IDMA Ch.0x24: 511 single-byte fixed-source writes to SPI TXD. */
	mem_write(m, REG(0x1105), 1, 0);
	mem_write(m, REG(0x1100), 4, base);
	mem_write(m, base + 0x240, 4, 0);
	mem_write(m, base + 0x244, 4, 511);
	mem_write(m, base + 0x248, 4, SDRAM_BASE);
	mem_write(m, base + 0x24c, 4, REG(0x1704));
	mem_write(m, REG(0x029b), 1, 0x10);
	mem_write(m, REG(0x029c), 1, 0x10);
	mem_write(m, REG(0x1105), 1, global_enable ? 1 : 0);
}

static void reset_all(struct mem *m, struct dma *dma, struct sdcard *sd,
		      struct cmu *cmu, struct itc *itc)
{
	mem_clear_ram(m);
	itc_reset(itc);
	cmu_reset(cmu);
	sd_reset(sd);
	sd->xfers = 0;
	sd->shift_cycles = 0;
	sd->wait_cycles = 0;
	dma_reset(dma);
	mem_write(m, SDRAM_BASE, 1, 0xff);
}

int main(void)
{
	struct mem m;
	struct cmu cmu;
	struct itc itc;
	struct port port;
	struct sdcard sd;
	struct dma dma;
	uint64_t clock = 0;
	const uint32_t table = DSTRAM_BASE;
	const uint32_t dst = SDRAM_BASE + 0x1000;

	assert(mem_init(&m));
	itc_attach(&m, &itc);
	cmu_attach(&m, &cmu);
	port_attach(&m, &port, &itc);
	assert(sd_attach(&m, &sd, NULL, &port, NULL, true));
	dma_attach(&m, &dma, &itc, &cmu, &sd);

	/*
	 * MCBR=0 is MCLK/4 and BPT=7 is eight bits, so the first character
	 * completes after 32 MCLK cycles. SPI_WAIT=0 inserts one divided-clock
	 * period before a following CPU-initiated character.
	 */
	sd_set_clock(&sd, &clock);
	dma_set_clock(&dma, &clock);
	reset_all(&m, &dma, &sd, &cmu, &itc);
	mem_write(&m, REG(0x1708), 4, SPI_CTL1_8BIT_MASTER_DMA);
	mem_write(&m, REG(0x1710), 4, 0);
	mem_write(&m, REG(0x1704), 4, 0xff);
	assert(sd.xfers == 0 && (mem_read(&m, REG(0x1714), 4) & (1u << 6)));
	clock = 31;
	sd_poll(&sd);
	assert(sd.xfers == 0);
	clock = 32;
	sd_poll(&sd);
	assert(sd.xfers == 1 && sd.shift_cycles == 32);
	(void)mem_read(&m, REG(0x1700), 4);
	mem_write(&m, REG(0x1704), 4, 0xff);
	clock = 67;
	sd_poll(&sd);
	assert(sd.xfers == 1);
	clock = 68;
	sd_poll(&sd);
	assert(sd.xfers == 2 && sd.wait_cycles == 4);

	/* The complete documented pipeline clocks and receives all 512 bytes. */
	clock = 0;
	reset_all(&m, &dma, &sd, &cmu, &itc);
	setup_read(&m, table, dst, true);
	mem_write(&m, REG(0x1704), 4, 0xff);
	assert(sd.xfers == 0);
	while (sd.xfers < 512) {
		clock++;
		sd_poll(&sd);
	}
	assert(sd.xfers == 512);
	assert(dma.hsdma_transfers == 512);
	assert(dma.idma_transfers == 511);
	assert(sd.shift_cycles == 512u * 32u);
	assert(sd.wait_cycles == 0);
	assert(dma.bus_cycles == 512u * 2u + 511u * 10u);
	assert(clock == 20474);
	assert((mem_read(&m, REG(0x115c), 2) & 1) == 0);
	assert(mem_read(&m, REG(0x0281), 1) & (1u << 3));
	assert(mem_read(&m, table + 0x244, 4) == 0);
	for (unsigned i = 0; i < 512; i++)
		assert(mem_read(&m, dst + i, 1) == 0xff);

	/* DMA_CKE=0: the trigger occurs, but neither engine can run. */
	reset_all(&m, &dma, &sd, &cmu, &itc);
	/* Failure-mode checks below concern routing, not elapsed time. */
	sd_set_clock(&sd, NULL);
	dma_set_clock(&dma, NULL);
	setup_read(&m, table, dst, true);
	mem_write(&m, REG(0x1b24), 4, 0x96);
	mem_write(&m, REG(0x1b04), 4,
		  mem_read(&m, REG(0x1b04), 4) & ~(1u << 1));
	mem_write(&m, REG(0x1704), 4, 0xff);
	assert(sd.xfers == 1 && dma.hsdma_transfers == 0);
	assert(mem_read(&m, REG(0x115c), 2) & 1);

	/* Commenting out IDMAEN receives one byte, then supplies no more clocks. */
	reset_all(&m, &dma, &sd, &cmu, &itc);
	setup_read(&m, table, dst, false);
	mem_write(&m, REG(0x1704), 4, 0xff);
	assert(sd.xfers == 1 && dma.hsdma_transfers == 1);
	assert(mem_read(&m, REG(0x115c), 2) & 1);
	/* The manual says the unaccepted hardware request remains pending. */
	mem_write(&m, REG(0x1105), 1, 1);
	assert(sd.xfers == 512 && dma.hsdma_transfers == 512);

	/* A0 RAM cannot hold IDMA control information on this chip. */
	reset_all(&m, &dma, &sd, &cmu, &itc);
	setup_read(&m, 0x400, dst, true);
	mem_write(&m, REG(0x1704), 4, 0xff);
	assert(sd.xfers == 1 && dma.hsdma_transfers == 1);
	assert(dma.invalid_descriptors == 1);

	mem_free(&m);
	puts("dma: documented SPI timing, pipeline and failure modes pass");
	return 0;
}
