#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/cmu.h"
#include "../src/dma.h"
#include "../src/itc.h"
#include "../src/mem.h"
#include "../src/model.h"
#include "../src/port.h"
#include "../src/sdcard.h"

#define REG(a) (REG_BASE + (a))
#define SPI_CTL1_8BIT_MASTER_DMA ((7u << 10) | (1u << 3) | (1u << 2) | \
				  (1u << 1) | 1u)
#define SPI_CTL1_32BIT_MASTER_DMA (SPI_CTL1_8BIT_MASTER_DMA | (31u << 10))

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
	/* Manual-only DMA timing; the fitted per-transfer overhead is not
	   part of what this test checks. */
	model.dma_extra = 0;
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
	/* V.2.8: a nonzero SPI_INT must be cleared before disabling SPI.
	 * Count invalid sequences instead of guessing their physical failure mode. */
	reset_all(&m, &dma, &sd, &cmu, &itc);
	mem_write(&m, REG(0x1718), 4, 0xffffffffu);
	assert(mem_read(&m, REG(0x1718), 4) == 0x1f);
	mem_write(&m, REG(0x1718), 4, 0x14);
	mem_write(&m, REG(0x1708), 4, SPI_CTL1_8BIT_MASTER_DMA);
	mem_write(&m, REG(0x1708), 4, SPI_CTL1_8BIT_MASTER_DMA & ~1u);
	assert(sd.unsafe_disables == 1);
	mem_write(&m, REG(0x1718), 4, 0);
	mem_write(&m, REG(0x1708), 4, SPI_CTL1_8BIT_MASTER_DMA);
	mem_write(&m, REG(0x1708), 4, SPI_CTL1_8BIT_MASTER_DMA & ~1u);
	assert(sd.unsafe_disables == 1);
	mem_write(&m, REG(0x1708), 4, SPI_CTL1_8BIT_MASTER_DMA);
	mem_write(&m, REG(0x1704), 4, 0xff);
	(void)mem_read(&m, REG(0x1708), 4);
	mem_write(&m, REG(0x1710), 4, 0);
	assert(sd.busy_control_accesses == 2);

	/* Model the separate receive mask, including full-width unmasked data. */
	reset_all(&m, &dma, &sd, &cmu, &itc);
	sd_set_clock(&sd, NULL);
	port.reg[OFF_P5D] &= (uint8_t)~(1u << CS_SDCARD_BIT);
	mem_write(&m, REG(0x1708), 4, SPI_CTL1_32BIT_MASTER_DMA);
	sd.resp[0] = 0x12;
	sd.resp[1] = 0x34;
	sd.resp[2] = 0x56;
	sd.resp[3] = 0x78;
	sd.resp_len = 4;
	mem_write(&m, REG(0x171c), 4, (7u << 10) | 2u);
	assert(mem_read(&m, REG(0x171c), 4) == ((7u << 10) | 2u));
	mem_write(&m, REG(0x1704), 4, 0xffffffffu);
	assert(mem_read(&m, REG(0x1700), 4) == 0x78);
	sd.resp_pos = 0;
	mem_write(&m, REG(0x171c), 4, 0);
	mem_write(&m, REG(0x1704), 4, 0xffffffffu);
	assert(mem_read(&m, REG(0x1700), 4) == 0x12345678);

	/* Recorded hardware effect: even an unchanged-width ENA cycle
	 * advances a selected card by one bit. GPIO isolates the clock pin. */
	reset_all(&m, &dma, &sd, &cmu, &itc);
	mem_write(&m, REG(0x3ad), 1, 0x54);
	mem_write(&m, REG(0x1708), 4, SPI_CTL1_8BIT_MASTER_DMA);
	sd.resp[0] = 0x12;
	sd.resp[1] = 0x34;
	sd.resp[2] = 0x56;
	sd.resp_len = 3;
	mem_write(&m, REG(0x1708), 4, SPI_CTL1_8BIT_MASTER_DMA & ~1u);
	mem_write(&m, REG(0x1708), 4, SPI_CTL1_8BIT_MASTER_DMA);
	mem_write(&m, REG(0x1704), 4, 0xff);
	assert(mem_read(&m, REG(0x1700), 4) == 0x24);
	mem_write(&m, REG(0x1704), 4, 0xff);
	assert(mem_read(&m, REG(0x1700), 4) == 0x68);
	assert(sd.unclamped_disables == 1 && sd.resp_bit == 1);
	mem_write(&m, REG(0x38a), 1, 5); /* deselect resets bit alignment */
	mem_write(&m, REG(0x1704), 4, 0xff);
	assert(sd.resp_bit == 0);
	(void)mem_read(&m, REG(0x1700), 4);
	mem_write(&m, REG(0x38a), 1, 4);
	sd.resp[0] = 0x12;
	sd.resp_len = 1;
	mem_write(&m, REG(0x38c), 1, 0x30);
	mem_write(&m, REG(0x38d), 1, 0x80);
	mem_write(&m, REG(0x3ad), 1, 0x14);
	mem_write(&m, REG(0x1708), 4, SPI_CTL1_8BIT_MASTER_DMA & ~1u);
	mem_write(&m, REG(0x1708), 4, SPI_CTL1_8BIT_MASTER_DMA);
	mem_write(&m, REG(0x3ad), 1, 0x54);
	mem_write(&m, REG(0x1704), 4, 0xff);
	assert(mem_read(&m, REG(0x1700), 4) == 0x12);
	assert(sd.unclamped_disables == 1 && sd.resp_bit == 0);
	mem_write(&m, REG(0x3ad), 1, 0); /* independent tests use reset mux */
	sd_set_clock(&sd, &clock);
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

	/* 32-bit SPI: preserve all four bytes, in wire order, with one pair
	 * of DMA requests per word. Nonuniform payload catches truncation and
	 * wrong endian assumptions that the all-0xff byte test cannot see. */
	clock = 0;
	reset_all(&m, &dma, &sd, &cmu, &itc);
	setup_read(&m, table, dst, true);
	port.reg[OFF_P5D] &= (uint8_t)~(1u << CS_SDCARD_BIT);
	for (unsigned i = 0; i < 512; i++)
		sd.resp[i] = (uint8_t)((i * 73) ^ (i >> 3) ^ 0x9d);
	sd.resp[512] = 0x12;
	sd.resp[513] = 0xab;
	sd.resp_len = 514;
	sd.block_timing = true;
	sd.block_first_pos = 0;
	sd.block_last_pos = 511;
	sd.payloads_timed = sd.payload_cycles = 0;
	mem_write(&m, REG(0x1708), 4, SPI_CTL1_32BIT_MASTER_DMA);
	mem_write(&m, REG(0x1192), 2, 1); /* WORDSIZE3 */
	mem_write(&m, REG(0x1150), 2, 128);
	mem_write(&m, table + 0x240, 4, 2u << 16); /* word IDMA */
	mem_write(&m, table + 0x244, 4, 127);
	mem_write(&m, SDRAM_BASE, 4, 0xffffffff);
	mem_write(&m, dst - 4, 4, 0x10293847);
	mem_write(&m, dst + 512, 4, 0xabcde123);
	mem_write(&m, REG(0x1704), 4, 0xffffffff);
	clock = 127;
	sd_poll(&sd);
	assert(sd.xfers == 0);
	while (sd.xfers < 128) {
		clock++;
		sd_poll(&sd);
	}
	assert(sd.resp_pos == 512 && sd.overflows == 0);
	assert(dma.hsdma_transfers == 128 && dma.idma_transfers == 127);
	assert(sd.shift_cycles == 512u * 32u);
	assert(sd.wait_cycles == 0);
	assert(dma.bus_cycles == 128u * 2u + 127u * 10u);
	assert(sd.payloads_timed == 1);
	assert(sd.payload_cycles == 128u * 128u + 127u * 8u);
	for (unsigned i = 0; i < 512; i += 4) {
		uint32_t want = (uint32_t)sd.resp[i] << 24 |
			(uint32_t)sd.resp[i + 1] << 16 |
			(uint32_t)sd.resp[i + 2] << 8 | sd.resp[i + 3];
		assert(mem_read(&m, dst + i, 4) == want);
	}
	assert(mem_read(&m, dst - 4, 4) == 0x10293847);
	assert(mem_read(&m, dst + 512, 4) == 0xabcde123);
	assert(mem_read(&m, REG(0x1150), 2) == 0);
	assert(mem_read(&m, table + 0x244, 4) == 0);
	/* Switching back to byte transfers must leave both CRC bytes unread. */
	mem_write(&m, REG(0x115c), 2, 0);
	mem_write(&m, REG(0x1105), 1, 0);
	mem_write(&m, REG(0x1708), 4, SPI_CTL1_8BIT_MASTER_DMA);
	for (unsigned i = 512; i < 514; i++) {
		mem_write(&m, REG(0x1704), 4, 0xff);
		while (sd.busy) {
			clock++;
			sd_poll(&sd);
		}
		assert(mem_read(&m, REG(0x1700), 4) == sd.resp[i]);
	}
	assert(sd.resp_pos == 514 && sd.overflows == 0);

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
