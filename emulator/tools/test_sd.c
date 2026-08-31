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
