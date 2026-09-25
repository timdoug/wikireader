/*
 * SD card writes.
 *
 * The card is what makes the guest's history, bookmarks and settings
 * outlive a run, so the write path has to put the right bytes in the right
 * block. The hazard it carries is that block data is arbitrary: a byte in
 * the middle of an article can have bit 7 clear and bit 6 set, which is
 * exactly the shape of a command frame. The data phase therefore has to be
 * recognised before any command sniffing, and the tests below write blocks
 * full of command-shaped bytes to keep it that way.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/mem.h"
#include "../src/sdcard.h"
#include "../src/cmu.h"
#include "../src/model.h"
#include "../../samo-lib/drivers/include/mmc_csd.h"

#define SPI_TXD  (REG_BASE + 0x1704)
#define SPI_RXD  (REG_BASE + 0x1700)

#define BLOCKS   64
static const char *IMG = "/tmp/test_sd_card.img";

static int fails;

static void ok(const char *what, bool cond)
{
	printf("%-62s %s\n", what, cond ? "ok" : "FAILED");
	if (!cond)
		fails++;
}

/* One SPI byte exchange, the way the driver does it. */
static uint8_t xchg(struct mem *m, uint8_t out)
{
	mem_write(m, SPI_TXD, 4, out);
	return (uint8_t)mem_read(m, SPI_RXD, 4);
}

static void command(struct mem *m, uint8_t idx, uint32_t arg)
{
	xchg(m, (uint8_t)(0x40 | idx));
	xchg(m, (uint8_t)(arg >> 24));
	xchg(m, (uint8_t)(arg >> 16));
	xchg(m, (uint8_t)(arg >> 8));
	xchg(m, (uint8_t)arg);
	xchg(m, 0x95);                       /* CRC, only checked for CMD0 */
}

/* Clock until the card stops returning idle bytes, as wait_ready() does. */
static uint8_t settle(struct mem *m)
{
	uint8_t r = 0xff;
	for (int i = 0; i < 8; i++) {
		r = xchg(m, 0xff);
		if (r != 0xff)
			return r;
	}
	return r;
}

static uint16_t crc16(const uint8_t *data, size_t len)
{
	uint16_t crc = 0;

	while (len--) {
		crc ^= (uint16_t)*data++ << 8;
		for (unsigned bit = 0; bit < 8; bit++)
			crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
	}
	return crc;
}

static bool read_payload(struct mem *m, uint8_t *data, size_t len)
{
	uint16_t expected;
	uint16_t received;

	if (settle(m) != 0x00)
		return false;
	for (int i = 0; i < 8; i++) {
		if (xchg(m, 0xff) != 0xfe)
			continue;
		for (size_t j = 0; j < len; j++)
			data[j] = xchg(m, 0xff);
		received = (uint16_t)xchg(m, 0xff) << 8;
		received |= xchg(m, 0xff);
		expected = crc16(data, len);
		return received == expected;
	}
	return false;
}

static bool read_register(struct mem *m, uint8_t idx, uint8_t reg[16])
{
	command(m, idx, 0);
	return read_payload(m, reg, 16);
}

/*
 * Send a data block and return the card's data response. Deliberately not
 * using 0xff filler: every byte here is command-shaped.
 */
static uint8_t send_block(struct mem *m, uint8_t token, const uint8_t *data)
{
	xchg(m, token);
	for (int i = 0; i < 512; i++)
		xchg(m, data[i]);
	xchg(m, 0xff);                       /* CRC */
	xchg(m, 0xff);
	return xchg(m, 0xff);
}

static void fill(uint8_t *b, unsigned seed)
{
	for (int i = 0; i < 512; i++)
		/* 0x40..0x7f: bit 7 clear, bit 6 set -- a command frame. */
		b[i] = (uint8_t)(0x40 | ((i + seed) & 0x3f));
}

static bool block_matches(unsigned blk, const uint8_t *want)
{
	uint8_t got[512];
	FILE *f = fopen(IMG, "rb");
	if (!f)
		return false;
	bool eq = fseek(f, (long)blk * 512, SEEK_SET) == 0 &&
		  fread(got, 1, 512, f) == 512 &&
		  memcmp(got, want, 512) == 0;
	fclose(f);
	return eq;
}

static void make_image(void)
{
	FILE *f = fopen(IMG, "wb");
	uint8_t zero[512];
	memset(zero, 0, sizeof zero);
	for (int i = 0; i < BLOCKS; i++)
		fwrite(zero, 1, sizeof zero, f);
	fclose(f);
}

int main(void)
{
	model.sd_read_latency = 0;   /* manual-only card timing */
	struct mem mem;
	struct sdcard sd;
	struct port port;
	uint8_t a[512], b[512];

	fill(a, 1);
	fill(b, 200);
	/* A single block, written and read straight back off the host file. */
	make_image();
	if (!mem_init(&mem))
		return 1;
	memset(&port, 0, sizeof port);
	port_reset(&port);
	port.reg[OFF_P5D] &= (uint8_t)~(1u << CS_SDCARD_BIT);
	if (!sd_attach(&mem, &sd, IMG, &port, NULL, false)) {
		printf("cannot attach card image\n");
		return 1;
	}
	ok("an image opened for update is not write protected", !sd.readonly);
	{
		/* SPI_CKE off: the register file and the shifter both stop, so
		 * a kernel that gates the block wrongly fails here, not on the
		 * card.  The gate is on at reset. */
		struct cmu cmu;
		unsigned long before = sd.xfers;

		cmu_attach(&mem, &cmu);
		sd_set_cmu(&sd, &cmu);
		mem_write(&mem, REG_BASE + 0x1b24, 4, 0x96);
		mem_write(&mem, REG_BASE + 0x1b04, 4,
			  mem_read(&mem, REG_BASE + 0x1b04, 4) & ~(1u << 6));
		ok("an unclocked SPI block exchanges nothing and reads as zero",
		   xchg(&mem, 0xff) == 0 && sd.xfers == before &&
		   sd.gated_accesses == 2 && !sd.busy);
		mem_write(&mem, REG_BASE + 0x1b04, 4,
			  mem_read(&mem, REG_BASE + 0x1b04, 4) | (1u << 6));
		ok("re-enabling SPI_CKE brings the bus back",
		   xchg(&mem, 0xff) == 0xff && sd.xfers == before + 1 &&
		   sd.gated_accesses == 2);
		sd_set_cmu(&sd, NULL);
	}
	{
		/* 64 GiB: C_SIZE bit 16 is the part the old driver dropped. */
		uint8_t csd[16];
		sd.blocks = 134217728ULL;
		ok("an SDXC-sized CSD register can be read",
		   read_register(&mem, 9, csd));
		ok("CSD v2 capacity retains the SDXC-sized upper bits",
		   mmc_csd_v2_sector_count(csd) == 134217728UL);
		sd.blocks = BLOCKS;
		ok("small image CSD rounds up to the SDHC capacity quantum",
		   read_register(&mem, 9, csd) &&
		   mmc_csd_v2_sector_count(csd) == 1024);
	}

	/* Linux mmc_spi's complete SDHC discovery/register sequence. */
	command(&mem, 1, 0);
	ok("CMD1 selects legacy byte addressing", settle(&mem) == 0x00 &&
	   sd.byte_addressed);
	command(&mem, 55, 0);
	ok("CMD55 prefixes an application command", settle(&mem) == 0x00);
	command(&mem, 41, 1u << 30);
	ok("ACMD41 returns an SDHC card to block addressing",
	   settle(&mem) == 0x00 && !sd.byte_addressed);
	{
		uint8_t scr[8];
		uint8_t status[64];

		command(&mem, 55, 0);
		ok("CMD55 accepts SEND_SCR", settle(&mem) == 0x00);
		command(&mem, 51, 0);
		ok("ACMD51 returns a CRC-valid SD 2.0 SCR",
		   read_payload(&mem, scr, sizeof scr) &&
		   scr[0] == 0x02 && (scr[1] & 0x0f) == 0x05);

		command(&mem, 55, 0);
		ok("CMD55 accepts SD_STATUS", settle(&mem) == 0x00);
		command(&mem, 13, 0);
		ok("ACMD13 returns a CRC-valid 64-byte SD Status",
		   read_payload(&mem, status, sizeof status));

		command(&mem, 6, 0x00fffff0);
		ok("CMD6 returns a CRC-valid switch-status register",
		   read_payload(&mem, status, sizeof status));
	}
	command(&mem, 13, 0);
	ok("CMD13 returns both bytes of the SPI R2 status",
	   settle(&mem) == 0x00 && xchg(&mem, 0xff) == 0x00);
	command(&mem, 59, 1);
	ok("CMD59 accepts Linux enabling SPI data CRCs", settle(&mem) == 0x00);
	{
		uint8_t block[512];

		command(&mem, 17, 0);
		ok("CMD17 data carries the CRC16 Linux verifies",
		   read_payload(&mem, block, sizeof block));
	}

	/* Removing slot power resets card protocol state, not the controller. */
	sd.byte_addressed = true;
	sd.expect_acmd = true;
	sd.resp_bit = 3;
	port.reg[OFF_P3D] |= 1u << 2;
	sd_poll(&sd);
	ok("slot power-off resets queued protocol and response alignment",
	   !sd.card_powered && sd.idle && !sd.byte_addressed &&
	   !sd.expect_acmd && sd.resp_bit == 0);
	port.reg[OFF_P3D] &= (uint8_t)~(1u << 2);
	sd_poll(&sd);
	ok("slot power-on makes the reset card visible again", sd.card_powered);

	command(&mem, 24, 7);                /* WRITE_BLOCK, block 7 */
	ok("CMD24 is accepted", settle(&mem) == 0x00);
	ok("a block of command-shaped bytes is accepted",
	   (send_block(&mem, 0xFE, a) & 0x1f) == 0x05);
	ok("and reaches the image byte for byte", block_matches(7, a));
	ok("one block counted as written", sd.blocks_written == 1);

	/* Neighbours untouched: the offset has to be right, not just the data. */
	{
		uint8_t zero[512];
		memset(zero, 0, sizeof zero);
		ok("the block before it is untouched", block_matches(6, zero));
		ok("the block after it is untouched", block_matches(8, zero));
	}

	/*
	 * CMD25: consecutive blocks, ended by the stop token. The driver
	 * polls the card ready before every command (release_spi, then
	 * wait_ready), which is what clears the busy byte the previous write
	 * left behind, so do the same here.
	 */
	settle(&mem);
	command(&mem, 25, 20);
	ok("CMD25 is accepted", settle(&mem) == 0x00);
	ok("first block of a multi-block write is accepted",
	   (send_block(&mem, 0xFC, a) & 0x1f) == 0x05);
	settle(&mem);
	ok("second block is accepted",
	   (send_block(&mem, 0xFC, b) & 0x1f) == 0x05);
	settle(&mem);
	xchg(&mem, 0xFD);                    /* STOP_TRAN */
	ok("multi-block write lands on consecutive blocks",
	   block_matches(20, a) && block_matches(21, b));

	/* After the stop token the card takes commands again. */
	command(&mem, 17, 7);
	ok("a command after STOP_TRAN is understood, not eaten as data",
	   settle(&mem) == 0x00);

	/* Discard that unread CMD17 response, then exercise a CMD18 stream. */
	port.reg[OFF_P5D] |= (uint8_t)(1u << CS_SDCARD_BIT);
	ok("a deselected card releases MISO", xchg(&mem, 0xff) == 0xff);
	port.reg[OFF_P5D] &= (uint8_t)~(1u << CS_SDCARD_BIT);
	command(&mem, 18, 3);
	ok("CMD18 is accepted", settle(&mem) == 0x00);
	while (xchg(&mem, 0xff) != 0xfe)
		;
	for (int i = 0; i < 512 + 2; i++)
		xchg(&mem, 0xff);
	{
		unsigned long blocks = sd.blocks_read;
		port.reg[OFF_P5D] |= (uint8_t)(1u << CS_SDCARD_BIT);
		ok("deselecting CMD18 does not prefetch another sector",
		   xchg(&mem, 0xff) == 0xff && sd.blocks_read == blocks);
		port.reg[OFF_P5D] &= (uint8_t)~(1u << CS_SDCARD_BIT);
	}

	sd_close(&sd);
	mem_free(&mem);

	/* A read-only card reports the failure rather than dropping it. */
	make_image();
	if (!mem_init(&mem))
		return 1;
	port_reset(&port);
	port.reg[OFF_P5D] &= (uint8_t)~(1u << CS_SDCARD_BIT);
	if (!sd_attach(&mem, &sd, IMG, &port, NULL, true))
		return 1;
	ok("a card opened read-only says so", sd.readonly);
	command(&mem, 24, 7);
	settle(&mem);
	ok("a rejected write answers with a write error, not acceptance",
	   (send_block(&mem, 0xFE, a) & 0x1f) == 0x0d);
	{
		uint8_t zero[512];
		memset(zero, 0, sizeof zero);
		ok("and the image is left alone", block_matches(7, zero));
	}
	sd_close(&sd);
	mem_free(&mem);
	remove(IMG);

	printf("\n%s\n", fails ? "FAILURES" : "all SD tests passed");
	return fails != 0;
}
