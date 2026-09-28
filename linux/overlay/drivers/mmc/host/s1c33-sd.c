// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SD cards on the Epson S1C33's synchronous serial interface.
 *
 * The WikiReader's card is the only device on this controller, so this is
 * an MMC host in SPI mode that drives the controller itself, rather than a
 * generic SPI controller under mmc_spi.  The protocol handling follows
 * mmc_spi; the transport does not.  There a block costs several SPI
 * messages, each validated, accounted, chip-selected and scheduled, and a
 * DMA completion interrupt that puts the reader to sleep; on this CPU that
 * came to about 1.9 ms a block against 0.3 ms on the wire.  Here every
 * byte goes straight to the controller's registers, and a data block is one
 * HSDMA transfer whose completion is polled.
 *
 * Protocol handling adapted from drivers/mmc/host/mmc_spi.c:
 * (C) Copyright 2005, Intec Automation, Mike Lavender
 * (C) Copyright 2006-2007, David Brownell
 * (C) Copyright 2007, Axis Communications, Hans-Peter Nilsson
 * (C) Copyright 2007, ATRON electronic GmbH, Jan Nikitenko
 */
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/crc-itu-t.h>
#include <linux/crc7.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/gpio/consumer.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/math64.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/slot-gpio.h>
#include <linux/module.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include <linux/swab.h>
#include <linux/hrtimer.h>
#include <linux/timex.h>
#include <linux/unaligned.h>

#include <linux/platform_data/s1c33-sd.h>

#include <asm/iram.h>

/* Synchronous serial interface channel 0 */
#define SPI_RXD		0x00
#define SPI_TXD		0x04
#define SPI_CTL1	0x08
#define SPI_CTL2	0x0c
#define SPI_WAIT	0x10
#define SPI_STAT	0x14
#define SPI_INT		0x18

#define SPI_BPT(bits)	(((bits) - 1U) << 10)
#define SPI_DIV_SHIFT	4
#define SPI_RX_DMA	BIT(2)
#define SPI_TX_DMA	BIT(3)
#define SPI_MASTER	BIT(1)
#define SPI_ENABLE	BIT(0)
#define SPI_BUSY	BIT(6)
#define SPI_RX_FULL	BIT(2)

#define SPI_POLLS	1000000

/* The interrupt controller's causes and IDMA requests for this port */
#define ITC_IDMA_REQ		0x00
#define ITC_IDMA_ENABLE		0x01
#define ITC_IDMA_SPI_BIT	BIT(4)
#define ITC_SPI_DMA_FLAGS	(BIT(4) | BIT(5))

/* Data block tokens and responses (mmc_spi.c) */
#define SPI_MMC_RESPONSE_CODE(x)	((x) & 0x1f)
#define SPI_RESPONSE_ACCEPTED		((2 << 1) | 1)
#define SPI_RESPONSE_CRC_ERR		((5 << 1) | 1)
#define SPI_RESPONSE_WRITE_ERR		((6 << 1) | 1)
#define SPI_TOKEN_SINGLE		0xfe
#define SPI_TOKEN_MULTI_WRITE		0xfc
#define SPI_TOKEN_STOP_TRAN		0xfd

#define SD_BLOCKSIZE		512
#define SD_BLOCKSATONCE		256
#define SD_R1B_TIMEOUT_MS	3000
#define SD_READY_MS		3000	/* for a card to answer after power-up */

/*
 * Where a block read's cycles go, for comparing the device with wremu:
 * read_timing in the device's sysfs directory.  Off unless asked for.
 */
enum sd_phase {
	SD_T_TOKEN,	/* the gap and the start token */
	SD_T_SETUP,	/* starting the block's transfer, or the stream's */
	SD_T_AHEAD,	/* preparing the next block's, overlapped */
	SD_T_CHECK,	/* order and CRC */
	SD_T_POLL,	/* waiting for the port to go idle, or for the stream */
	SD_T_STATUS,	/* asking the receive channel */
	SD_T_TAIL,	/* the CRC bytes from the last word */
	SD_T_REQUEST,	/* the rest of each request: map, unmap, the last check */
	SD_T_COMMAND,	/* commands and their responses, stop commands too */
	SD_T_WRITE,	/* writes, whole */
	SD_T_PHASES
};

struct sd_timing {
	bool on;
	u32 blocks, requests;
	/* Streamed reads: transfers, gap bytes, requests that carried on
	 * and the bytes already in when they did, and blocks that failed. */
	u32 transfers, gap_bytes, carried, in_hand, errors;
	u32 commands, written;
	u64 cycles[SD_T_PHASES];
};

#define SD_STREAM_SIZE		SZ_64K
#define SD_STREAM_BLOCK		(SD_BLOCKSIZE + 8)	/* a block's words */

/* A multiple-block read coming in: see sd_read_stream(). */
struct sd_stream {
	u32 *words;		/* the ring */
	u32 wrap[SD_STREAM_BLOCK / 4];	/* a block that wraps, put together */
	unsigned int pos;	/* the next byte to look at */
	unsigned int avail;	/* the bytes known to be in */
	unsigned int start;	/* where the transfer under way began */
	unsigned int end;	/* ...and where it ends */
	bool running;		/* a transfer is under way */
	bool open;		/* the card is reading on, unstopped */
	bool broken;		/* ...but a transfer failed between requests */
	u32 due;		/* get_cycles() when the transfer should end */
	u32 next_arg;		/* the read command that would carry on */
	dma_cookie_t rx_cookie;
};

struct s1c33_sd {
	struct mmc_host *mmc;
	struct device *dev;
	void __iomem *base;
	phys_addr_t base_phys;
	void __iomem *spi_flags;	/* the port's ITC cause flags */
	void __iomem *idma;		/* its IDMA request and enable */
	struct dma_chan *rx_chan;	/* HSDMA 3 on SPI receive */
	struct dma_chan *tx_chan;	/* HSDMA 2 on SPI transmit */
	struct scatterlist rx_sg;	/* each block's, readied once */
	u8 *ones;			/* the all-ones HSDMA 2 sends */
	dma_addr_t ones_dma;
	phys_addr_t ones_phys;		/* ...when they are in internal RAM */
	/*
	 * Send them for a block, resubmitted for every one, in turn: the next
	 * block's is submitted while the last one's still runs.
	 */
	struct dma_async_tx_descriptor *ones_txd[2];
	unsigned int ones_next;
	struct sd_stream stream;	/* multiple-block reads come in here */
	dma_addr_t stream_dma;
	/* Tends a transfer left running between requests. */
	struct hrtimer stream_timer;
	bool idle_polls;		/* the idle loop spins rather than halts */
	bool no_stream;			/* the card shifts its tokens */
	const struct s1c33_sd_platform_data *pdata;
	struct gpio_desc *cs;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins_default;
	struct pinctrl_state *pins_hold;	/* SCLK parked low, or NULL */
	unsigned long clock;		/* MCLK, read once at probe */
	unsigned int divider;		/* SCLK = MCLK >> (divider + 2) */
	u32 control;			/* CTL1 as programmed */
	unsigned char power_mode;
	/* Put a block in order and check it: sd_unpack_crc(), in A0 RAM. */
	u16 (*unpack_crc)(const u32 *words, u32 *out, unsigned int n,
			  u32 carry, unsigned int lead, const u16 *tables);
	u16 *crc_tables;		/* its tables, in A0 RAM too */
	u8 rx[8];			/* received, not yet read */
	unsigned int rx_len, rx_pos;
	unsigned long dma_blocks;
	struct sd_timing timing;
};

/* Charge the cycles since *@t to @phase and restart the clock. */
static inline void sd_charge(struct s1c33_sd *host, enum sd_phase phase,
			     u32 *t)
{
	u32 now;

	if (likely(!host->timing.on))
		return;
	now = get_cycles();
	host->timing.cycles[phase] += now - *t;
	*t = now;
}

static inline u32 sd_clock(struct s1c33_sd *host)
{
	return unlikely(host->timing.on) ? get_cycles() : 0;
}

/****************************************************************************/
/* Transport */

/*
 * Every character on the wire is 32 bits, commands and tokens included.
 * The controller has to be disabled to change its character size, and
 * disabling it with the card selected costs the card a clock edge, so the
 * size never changes: bytes are packed into words on the way out, and the
 * bytes of each word received queue up in @rx for whoever reads next.  An
 * all-ones byte is idle on the card's input, so outgoing words are padded
 * with them.  The first byte on the wire is each word's top byte.
 */

static int sd_wait(struct s1c33_sd *host, u32 flag, bool wanted)
{
	unsigned int count;

	for (count = 0; count < SPI_POLLS; count++)
		if (!!(readl(host->base + SPI_STAT) & flag) == wanted)
			return 0;
	return -ETIMEDOUT;
}

/*
 * Program the clock, with both DMA requests enabled for good: they only
 * raise ITC flags until a transfer arms HSDMA.  Disabling the controller
 * while it drives SCLK puts an edge on the wire, so the pin is parked at
 * its idle level, the "hold" pin state, across the change.  Only a new
 * clock rate gets here, a few times a boot.
 */
static void sd_configure(struct s1c33_sd *host)
{
	u32 control = SPI_BPT(32) | host->divider << SPI_DIV_SHIFT |
		SPI_MASTER | SPI_RX_DMA | SPI_TX_DMA;
	unsigned int settle;

	if (control == host->control)
		return;
	if (host->pins_hold)
		pinctrl_select_state(host->pinctrl, host->pins_hold);
	writel(0, host->base + SPI_CTL1);
	writel(control, host->base + SPI_CTL1);
	writel(control | SPI_ENABLE, host->base + SPI_CTL1);
	for (settle = 4U << host->divider; settle; settle--)
		cpu_relax();
	if (host->pins_hold)
		pinctrl_select_state(host->pinctrl, host->pins_default);
	host->control = control;
}

/* One word each way. */
static int sd_word(struct s1c33_sd *host, u32 out, u32 *in)
{
	sd_configure(host);
	if (sd_wait(host, SPI_BUSY, false))
		return -ETIMEDOUT;
	writel(out, host->base + SPI_TXD);
	if (sd_wait(host, SPI_RX_FULL, true))
		return -ETIMEDOUT;
	*in = readl(host->base + SPI_RXD);
	return 0;
}

static void sd_rx_drop(struct s1c33_sd *host)
{
	host->rx_len = host->rx_pos = 0;
}

static void sd_rx_queue(struct s1c33_sd *host, const u8 *bytes,
			unsigned int n)
{
	sd_rx_drop(host);
	memcpy(host->rx, bytes, n);
	host->rx_len = n;
}

/* The next byte from the card, clocking a word in when none is queued. */
static int sd_rx_byte(struct s1c33_sd *host)
{
	if (host->rx_pos == host->rx_len) {
		u32 word;
		int ret = sd_word(host, ~0U, &word);

		if (ret)
			return ret;
		put_unaligned_be32(word, host->rx);
		host->rx_len = 4;
		host->rx_pos = 0;
	}
	return host->rx[host->rx_pos++];
}

static int sd_rx_bytes(struct s1c33_sd *host, u8 *rx, unsigned int n)
{
	unsigned int i;
	int value;

	for (i = 0; i < n; i++) {
		value = sd_rx_byte(host);
		if (value < 0)
			return value;
		rx[i] = value;
	}
	return 0;
}

/*
 * Send @n bytes after enough all-ones to fill whole words, and forget
 * what came back: nothing the card sends during a command or token counts.
 */
static int sd_tx(struct s1c33_sd *host, const u8 *tx, unsigned int n)
{
	unsigned int pad = -n & 3;
	unsigned int i, j;
	u32 word, in;
	int ret;

	sd_rx_drop(host);
	for (i = 0; i < pad + n; i += 4) {
		word = 0;
		for (j = i; j < i + 4; j++)
			word = word << 8 | (j < pad ? 0xff : tx[j - pad]);
		ret = sd_word(host, word, &in);
		if (ret)
			return ret;
	}
	return 0;
}

/* The card releases its data line eight clocks after it is deselected. */
static void sd_deselect(struct s1c33_sd *host)
{
	u32 in;

	gpiod_set_value(host->cs, 0);
	sd_word(host, ~0U, &in);
	sd_rx_drop(host);
}

static void sd_select(struct s1c33_sd *host)
{
	sd_rx_drop(host);
	gpiod_set_value(host->cs, 1);
}

/*
 * Clock in bytes until one differs from @byte: the gap before a data
 * token or response (all-ones) or the card's busy signal (zeroes).
 */
static int sd_skip(struct s1c33_sd *host, unsigned long timeout, u8 byte)
{
	unsigned long deadline = jiffies + timeout;
	unsigned int count = 0;
	int value;

	for (;;) {
		value = sd_rx_byte(host);
		if (value != byte)
			return value;
		if (++count % 256)
			continue;
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
		/* A write or an erase can keep a card busy for a while. */
		cond_resched();
	}
}

static int sd_wait_unbusy(struct s1c33_sd *host, unsigned long timeout)
{
	int value = sd_skip(host, timeout, 0);

	return value < 0 ? value : 0;
}

/* Poll a transfer to its end: HSDMA completion is found by asking. */
static int sd_dma_wait(struct dma_chan *chan, dma_cookie_t cookie)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(100);
	enum dma_status status;

	while ((status = dmaengine_tx_status(chan, cookie, NULL)) ==
	       DMA_IN_PROGRESS)
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
	return status == DMA_COMPLETE ? 0 : -EIO;
}

/*
 * A block read in flight.  Reads are pipelined: once block N+1's token is
 * in and its transfer started, block N is put in order and checked while
 * N+1 crosses the wire, which would otherwise idle through all that work.
 */
struct sd_read {
	u8 *buf;
	unsigned int len;
	dma_addr_t addr;	/* the buffer for HSDMA, if it can reach it */
	bool dma;		/* the words came by HSDMA, in wire order */
	bool prepared;		/* its transfer is submitted, not yet issued */
	unsigned int lead;	/* block bytes that came in the token's word */
	u8 first[4];		/* ... and are here */
	unsigned int bitshift;	/* how far the card shifted the block */
	u8 leftover;
	u8 crc[2];
	dma_cookie_t rx_cookie, tx_cookie;
};

/*
 * A block's transfer: @len bytes, a multiple of four, as words to its
 * buffer, which is word aligned: HSDMA 3 moves them to memory while HSDMA
 * 2 feeds all-ones to the transmitter, whose first word the CPU writes to
 * start the exchange.  The words land in wire order, first byte on top.
 *
 * Preparing and submitting the descriptors is most of the cost, hundreds
 * of instructions from SDRAM, and needs nothing from the card, so the next
 * block's is done while this one crosses the wire, which it does not slow:
 * the transfer's time is the DMA's own.  Only issuing waits for the token.
 * Returns with nothing submitted on failure, or with the caller to
 * terminate.
 */
static int sd_dma_prepare(struct s1c33_sd *host, struct sd_read *r)
{
	struct dma_async_tx_descriptor *rxd, *txd;

	sg_dma_address(&host->rx_sg) = r->addr;
	sg_dma_len(&host->rx_sg) = r->len;
	rxd = dmaengine_prep_slave_sg(host->rx_chan, &host->rx_sg, 1,
				      DMA_DEV_TO_MEM, 0);
	if (!rxd)
		return -ENOMEM;
	r->rx_cookie = dmaengine_submit(rxd);
	if (r->rx_cookie < 0)
		return -EBUSY;
	if (r->len == SD_BLOCKSIZE) {
		host->ones_next ^= 1;
		txd = host->ones_txd[host->ones_next];
	} else {
		txd = dmaengine_prep_slave_single(host->tx_chan,
						  host->ones_dma, r->len - 4,
						  DMA_MEM_TO_DEV, 0);
		if (!txd)
			return -ENOMEM;
	}
	r->tx_cookie = dmaengine_submit(txd);
	if (r->tx_cookie < 0)
		return -EBUSY;
	r->prepared = true;
	return 0;
}

static void sd_dma_go(struct s1c33_sd *host)
{
	writeb(ITC_SPI_DMA_FLAGS, host->spi_flags);
	dma_async_issue_pending(host->rx_chan);
	dma_async_issue_pending(host->tx_chan);
	writel(~0U, host->base + SPI_TXD);
}

static void sd_dma_stop(struct s1c33_sd *host)
{
	dmaengine_terminate_sync(host->rx_chan);
	dmaengine_terminate_sync(host->tx_chan);
}

/*
 * Wait for a transfer to end.  The CPU polls rather than sleeping for an
 * interrupt: the block takes a fraction of a millisecond, less than a sleep
 * and wakeup would, and a polling CPU does not halt, which on this part
 * would stop the DMA's request pipeline.
 *
 * The port stays busy until the last word is in: HSDMA 2 refills the
 * transmitter long before a word has shifted out.  Wait for that on a
 * register, in a loop that runs from the fetch buffer, and only then ask
 * the channels: each status poll runs hundreds of instructions from SDRAM,
 * and those fetches slowed the transfer itself by half.  Only the receive
 * channel is asked.  Every word it took was clocked in by one the transmit
 * channel sent, so that channel is done too, and the provider retires it
 * when its descriptor is next submitted.
 */
static int sd_dma_finish(struct s1c33_sd *host, struct sd_read *r, u32 *t)
{
	int ret;

	sd_wait(host, SPI_BUSY, false);
	sd_charge(host, SD_T_POLL, t);
	ret = sd_dma_wait(host->rx_chan, r->rx_cookie);
	if (!ret && sd_wait(host, SPI_BUSY, false))
		ret = -ETIMEDOUT;
	sd_charge(host, SD_T_STATUS, t);
	if (ret) {
		sd_dma_stop(host);
		return ret;
	}
	host->dma_blocks++;
	return 0;
}

/*
 * Put DMA'd words, wire order with the first byte on top, back into memory
 * order and @lead bytes further on, behind the @lead bytes that came before
 * them.  One pass: each word is swapped and shifted in registers, with no
 * second copy.  The @lead bytes pushed off the end were queued when the
 * transfer ended.
 */
static void sd_unpack(u32 *words, unsigned int n, const u8 *first,
		      unsigned int lead)
{
	unsigned int shift = lead * 8;
	u32 carry = 0, word;
	unsigned int i;

	if (!lead) {
		for (i = 0; i < n; i++)
			words[i] = swab32(words[i]);
		return;
	}
	for (i = 0; i < lead; i++)
		carry |= (u32)first[i] << (8 * i);
	for (i = 0; i < n; i++) {
		word = swab32(words[i]);
		words[i] = carry | word << shift;
		carry = word >> (32 - shift);
	}
}

static int sd_write_words(struct s1c33_sd *host, const u8 *tx,
			  unsigned int len)
{
	unsigned int i;
	u32 in;
	int ret;

	for (i = 0; i < len; i += 4) {
		ret = sd_word(host, get_unaligned_be32(tx + i), &in);
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * The CRC16 tables for four bytes at a time: the first 256 entries are
 * crc_itu_t_table, and each next 256 the CRC of a byte followed by one more
 * zero byte.
 */
#define SD_CRC_TABLES_SIZE	(4 * sizeof(crc_itu_t_table))

static void sd_crc_tables_init(u16 *t)
{
	unsigned int v;
	u16 c;

	memcpy(t, crc_itu_t_table, sizeof(crc_itu_t_table));
	for (v = 256; v < 4 * 256; v++) {
		c = t[v - 256];
		t[v] = c << 8 ^ t[c >> 8];
	}
}

/*
 * Put a DMA'd block in memory order into @out, which may be @words, and
 * return its CRC, in one pass.
 * @words holds @n words in wire order, first byte on top; the block's
 * first @lead bytes came before them and are in @carry, first byte lowest,
 * and the last @lead bytes of @words are past the block.  Each word goes
 * into the CRC four bytes at a time, straight from wire order, then is
 * swapped and shifted @lead bytes on.
 *
 * About 40 instructions a word: too long for the fetch queue, so it runs
 * from A0 RAM (asm/iram.h), where it costs well under the byte-at-a-time
 * CRC and a separate unpack from SDRAM.  It calls nothing and refers to no
 * data by address, so it can run from there.
 */
static u16 __iramfunc sd_unpack_crc(const u32 *words, u32 *out,
				    unsigned int n, u32 carry,
				    unsigned int lead, const u16 *tables)
{
	const u16 *t0 = tables, *t1 = tables + 256;
	const u16 *t2 = tables + 512, *t3 = tables + 768;
	unsigned int shift = lead * 8, i;
	u32 raw, x, word;
	u16 crc = 0;

	for (i = 0; i < lead; i++)
		crc = crc << 8 ^ t0[(crc >> 8 ^ carry >> (8 * i)) & 0xff];

	if (!lead) {
		for (i = 0; i < n; i++) {
			raw = words[i];
			x = raw ^ (u32)crc << 16;
			crc = t3[x >> 24] ^ t2[(x >> 16) & 0xff] ^
			      t1[(x >> 8) & 0xff] ^ t0[x & 0xff];
			out[i] = swab32(raw);
		}
		return crc;
	}

	for (i = 0; i < n - 1; i++) {
		raw = words[i];
		x = raw ^ (u32)crc << 16;
		crc = t3[x >> 24] ^ t2[(x >> 16) & 0xff] ^
		      t1[(x >> 8) & 0xff] ^ t0[x & 0xff];
		word = swab32(raw);
		out[i] = carry | word << shift;
		carry = word >> (32 - shift);
	}
	/* The last word: only its top 4 - @lead bytes are the block's. */
	raw = words[i];
	for (i = 0; i < 4 - lead; i++)
		crc = crc << 8 ^ t0[(crc >> 8 ^ raw >> (24 - 8 * i)) & 0xff];
	out[n - 1] = carry | swab32(raw) << shift;
	return crc;
}

/*
 * crc_itu_t(), compiled here so that its loop is aligned (see the
 * Makefile).  The loop is 26 bytes, which the C33 runs from its fetch
 * buffer only when it starts a 16-byte line; otherwise every byte of every
 * block fetches it again from SDRAM, about 70 cycles a byte.
 *
 * Its table is the copy in A0 RAM.  With the table in SDRAM, each byte's
 * lookup and the next byte's load open two different SDRAM rows, about 36
 * cycles a byte; from internal RAM only the block's own row is open, and
 * stays open.  Block reads by HSDMA use sd_unpack_crc() instead.
 */
static u16 sd_crc(const u16 *table, const u8 *buf, unsigned int len)
{
	u16 crc = 0;

	while (len--)
		crc = (crc << 8) ^ table[((crc >> 8) ^ *buf++) & 0xff];
	return crc;
}

/****************************************************************************/
/* Commands */

/*
 * For SPI, cmd->resp[0] holds R1_SPI bits in its low byte and any R2_SPI
 * bits in the next; cmd->resp[1] holds the four bytes of R3 and R7.
 * Returns zero or a negative errno, leaving the card selected only when
 * @cs_on asks for it and all went well.
 */
static int sd_response(struct s1c33_sd *host, struct mmc_command *cmd,
		       bool cs_on)
{
	unsigned int bitshift = 0;
	u16 rotator;
	u8 leftover = 0;
	int value = 0;
	int byte = 0xff;
	int i;

	/*
	 * N(CR) is 1..8 all-ones bytes; some cards take longer.  The first is
	 * ignored: after STOP_TRANSMISSION it can still carry data bits.
	 */
	byte = sd_rx_byte(host);
	if (byte < 0) {
		value = byte;
		goto done;
	}
	byte = 0xff;
	for (i = 1; i < 16 && byte == 0xff; i++) {
		byte = sd_rx_byte(host);
		if (byte < 0) {
			value = byte;
			goto done;
		}
	}
	if (byte == 0xff) {
		value = -ETIMEDOUT;
		goto done;
	}

	if (byte & 0x80) {
		/* A card that shifts its response by a few bits */
		rotator = byte << 8;
		byte = sd_rx_byte(host);
		if (byte < 0) {
			value = byte;
			goto done;
		}
		rotator |= byte;
		while (rotator & 0x8000) {
			bitshift++;
			rotator <<= 1;
		}
		cmd->resp[0] = rotator >> 8;
		leftover = rotator;
	} else {
		cmd->resp[0] = byte;
	}
	cmd->error = 0;

	if (cmd->resp[0] != 0) {
		if ((R1_SPI_PARAMETER | R1_SPI_ADDRESS) & cmd->resp[0])
			value = -EFAULT;
		else if (R1_SPI_ILLEGAL_COMMAND & cmd->resp[0])
			value = -ENOSYS;
		else if (R1_SPI_COM_CRC & cmd->resp[0])
			value = -EILSEQ;
		else if ((R1_SPI_ERASE_SEQ | R1_SPI_ERASE_RESET) & cmd->resp[0])
			value = -EIO;
		/* else R1_SPI_IDLE: the card is still resetting */
	}

	switch (mmc_spi_resp_type(cmd)) {
	case MMC_RSP_SPI_R1B:
		/* R1 then busy: STOP_TRANSMISSION, erases */
		sd_wait_unbusy(host, msecs_to_jiffies(cmd->busy_timeout ?
				cmd->busy_timeout : SD_R1B_TIMEOUT_MS));
		break;
	case MMC_RSP_SPI_R2:
		/* R1 then a second status byte: SEND_STATUS */
		byte = sd_rx_byte(host);
		if (byte < 0) {
			value = byte;
			goto done;
		}
		if (bitshift)
			cmd->resp[0] |= ((leftover << 8) | (byte << bitshift)) &
				0xff00;
		else
			cmd->resp[0] |= byte << 8;
		break;
	case MMC_RSP_SPI_R3:
		/* R1 then four bytes: OCR, IF_COND */
		rotator = leftover << 8;
		cmd->resp[1] = 0;
		for (i = 0; i < 4; i++) {
			cmd->resp[1] <<= 8;
			byte = sd_rx_byte(host);
			if (byte < 0) {
				value = byte;
				goto done;
			}
			if (bitshift) {
				rotator |= byte << bitshift;
				cmd->resp[1] |= rotator >> 8;
				rotator <<= 8;
			} else {
				cmd->resp[1] |= byte;
			}
		}
		break;
	case MMC_RSP_SPI_R1:
		break;
	default:
		if (value >= 0)
			value = -EINVAL;
		goto done;
	}

	if (value >= 0 && cs_on)
		return value;
done:
	if (value < 0)
		cmd->error = value;
	sd_deselect(host);
	return value;
}

static int sd_command(struct s1c33_sd *host, struct mmc_command *cmd,
		      bool cs_on)
{
	u8 frame[7];
	int status;

	/* An all-ones byte to be sure the card is ready, then the command. */
	frame[0] = 0xff;
	frame[1] = 0x40 | cmd->opcode;
	put_unaligned_be32(cmd->arg, frame + 2);
	frame[6] = crc7_be(0, frame + 1, 5) | 0x01;

	sd_select(host);
	status = sd_tx(host, frame, sizeof(frame));
	if (status < 0) {
		cmd->error = status;
		sd_deselect(host);
		return status;
	}
	return sd_response(host, cmd, cs_on);
}

/****************************************************************************/
/* Data */

/*
 * Begin reading one block: skip the all-ones gap and find the start token,
 * then start its transfer.  A card may shift the token, and so everything
 * after it, by a few bits; that is undone when the block is finished, as
 * mmc_spi does.  Buffers HSDMA cannot reach are read here, a byte at a time
 * through the receive queue.
 */
static int sd_read_start(struct s1c33_sd *host, struct sd_read *r,
			 unsigned long timeout, u32 *t)
{
	int status, ret;

	/* At least one card sends a zero byte before the all-ones. */
	status = sd_rx_byte(host);
	if (status == 0xff || status == 0)
		status = sd_skip(host, timeout, 0xff);
	if (status < 0)
		return status;
	/* A data error token: 0000xxxx */
	if (!(status & 0xf0))
		return -EIO;

	r->bitshift = 7;
	while (status & 0x80) {
		status <<= 1;
		r->bitshift--;
	}
	r->leftover = status << 1;
	sd_charge(host, SD_T_TOKEN, t);
	if (!r->dma) {
		ret = sd_rx_bytes(host, r->buf, r->len);
		return ret ? ret : sd_rx_bytes(host, r->crc, 2);
	}

	/*
	 * HSDMA writes only whole aligned words, so the block is read onto
	 * itself where it lies and sd_unpack() moves it up behind the bytes
	 * that came in the token's word.
	 */
	r->lead = host->rx_len - host->rx_pos;
	memcpy(r->first, host->rx + host->rx_pos, r->lead);
	sd_rx_drop(host);
	ret = r->prepared ? 0 : sd_dma_prepare(host, r);
	if (ret) {
		sd_dma_stop(host);
	} else {
		sd_dma_go(host);
		r->prepared = false;
	}
	sd_charge(host, SD_T_SETUP, t);
	return ret;
}

/*
 * Wait for a block's transfer, then take its CRC.  The last word's
 * bytes past the block, @lead of them, are the CRC and whatever follows,
 * and go back on the receive queue for it and the next token.
 */
static int sd_read_wait(struct s1c33_sd *host, struct sd_read *r, u32 *t)
{
	u8 tail[4];
	u32 last;
	unsigned int i;
	int ret;

	if (!r->dma)
		return 0;
	ret = sd_dma_finish(host, r, t);
	if (ret)
		return ret;
	if (r->lead) {
		last = swab32(((u32 *)r->buf)[r->len / 4 - 1]) >>
			(32 - r->lead * 8);
		for (i = 0; i < r->lead; i++)
			tail[i] = last >> (8 * i);
	}
	sd_rx_queue(host, tail, r->lead);
	ret = sd_rx_bytes(host, r->crc, 2);
	sd_charge(host, SD_T_TAIL, t);
	return ret;
}

/* Put a block in order and check it; the wire is free meanwhile. */
static int sd_read_finish(struct s1c33_sd *host, struct sd_read *r)
{
	u32 carry = 0;
	unsigned int i;
	u16 crc;

	if (r->dma && !r->bitshift) {
		for (i = 0; i < r->lead; i++)
			carry |= (u32)r->first[i] << (8 * i);
		crc = host->unpack_crc((u32 *)r->buf, (u32 *)r->buf,
				       r->len / 4, carry, r->lead,
				       host->crc_tables);
		if (host->mmc->use_spi_crc &&
		    get_unaligned_be16(r->crc) != crc)
			return -EILSEQ;
		return 0;
	}

	/* A card that shifted the block: in order first, then check it. */
	if (r->dma)
		sd_unpack((u32 *)r->buf, r->len / 4, r->first, r->lead);

	if (r->bitshift) {
		unsigned int bitshift = r->bitshift, bitright = 8 - bitshift;
		u8 leftover = r->leftover, temp;
		unsigned int i;

		for (i = 0; i < r->len; i++) {
			temp = r->buf[i];
			r->buf[i] = leftover | (temp >> bitshift);
			leftover = temp << bitright;
		}
		for (i = 0; i < sizeof(r->crc); i++) {
			temp = r->crc[i];
			r->crc[i] = leftover | (temp >> bitshift);
			leftover = temp << bitright;
		}
	}

	if (host->mmc->use_spi_crc &&
	    get_unaligned_be16(r->crc) !=
	    sd_crc(host->crc_tables, r->buf, r->len))
		return -EILSEQ;
	return 0;
}

/* Where the request's next block goes: its scatterlist entry and offset. */
struct sd_cursor {
	struct scatterlist *sg;
	unsigned int offset;
	bool mapped;
};

static bool sd_next_block(struct sd_cursor *at, struct mmc_data *data,
			  struct sd_read *r)
{
	while (at->sg && at->offset >= at->sg->length) {
		at->sg = sg_next(at->sg);
		at->offset = 0;
	}
	if (!at->sg)
		return false;
	r->len = min(at->sg->length - at->offset, data->blksz);
	r->buf = sg_virt(at->sg) + at->offset;
	r->addr = at->mapped ? sg_dma_address(at->sg) + at->offset :
		DMA_MAPPING_ERROR;
	r->dma = r->addr != DMA_MAPPING_ERROR && !(r->addr & 3);
	r->prepared = false;
	at->offset += r->len;
	return true;
}

/*
 * Read a request's blocks.  Each block is started, then the one before it
 * finished, then the next one's transfer prepared, then the new one waited
 * for, so putting block N in order, checking its CRC and preparing block
 * N+2's transfer happen while block N+1 arrives.  The request is mapped
 * once, for HSDMA to fill block by block.
 */
static void sd_read(struct s1c33_sd *host, struct mmc_data *data,
		    unsigned long timeout)
{
	struct sd_read reads[2], *cur = &reads[0], *prev = NULL, *next;
	struct sd_cursor at = { .sg = data->sg };
	unsigned int left = data->blocks;
	struct device *dma_dev = NULL;
	struct scatterlist *sg;
	unsigned int n_sg;
	int status = 0, checked;
	bool ahead = false;
	u32 t = sd_clock(host);

	if (host->rx_chan) {
		dma_dev = dmaengine_get_dma_device(host->rx_chan);
		if (dma_map_sg(dma_dev, data->sg, data->sg_len,
			       DMA_FROM_DEVICE) != data->sg_len)
			dma_dev = NULL;
	}
	at.mapped = dma_dev;

	if (!sd_next_block(&at, data, cur))
		left = 0;
	while (left--) {
		sd_charge(host, SD_T_REQUEST, &t);
		status = sd_read_start(host, cur, timeout, &t);
		ahead = false;
		checked = 0;
		next = prev ? prev : &reads[1];
		if (prev) {
			checked = sd_read_finish(host, prev);
			if (!checked)
				data->bytes_xfered += prev->len;
			prev = NULL;
			sd_charge(host, SD_T_CHECK, &t);
		}
		/*
		 * Only whole blocks: their transmit side is a descriptor
		 * kept for reuse, so preparing ahead allocates nothing that
		 * could be left over.
		 */
		if (!status && left && sd_next_block(&at, data, next) &&
		    next->dma && next->len == SD_BLOCKSIZE) {
			ahead = !sd_dma_prepare(host, next);
			sd_charge(host, SD_T_AHEAD, &t);
		}
		/* A transfer that started is drained either way. */
		if (!status)
			status = sd_read_wait(host, cur, &t);
		host->timing.blocks++;
		if (!status)
			status = checked;
		if (status)
			goto out;
		prev = cur;
		cur = next;
	}
	if (prev) {
		status = sd_read_finish(host, prev);
		if (!status)
			data->bytes_xfered += prev->len;
	}
out:
	if (status) {
		data->error = status;
		/* Drop a transfer submitted for a block never started. */
		if (ahead || cur->prepared)
			sd_dma_stop(host);
	}
	if (dma_dev)
		dma_unmap_sg(dma_dev, data->sg, data->sg_len, DMA_FROM_DEVICE);
	for_each_sg(data->sg, sg, data->sg_len, n_sg)
		flush_dcache_page(sg_page(sg));
	sd_charge(host, SD_T_REQUEST, &t);
	host->timing.requests++;
}

/*
 * Streamed reads.  In a multiple-block read the card sends each block's
 * gap, token, data and CRC one after another, and simply waits whenever the
 * host stops clocking.  So once the CPU has found the first token, the read
 * comes in by DMA into a ring buffer, and the CPU finds each token, puts
 * each block in order and checks it behind the DMA, as far as the
 * transfer's residue says has come in.  That does away with all that
 * sd_read() does between transfers: clocking in each token and CRC a word
 * at a time, and issuing both channels for every block.
 *
 * Nor does the stream end with the request.  When a request's blocks are
 * in, the card is left reading on and the stop command is answered without
 * being sent; if the next request reads on from there, as sequential reads
 * do, its blocks are already coming in, having crossed the wire while the
 * kernel copied the last ones out.  Any other request, a new clock or power
 * state, or an error stops the stream first (sd_stream_close()).
 *
 * The ring holds the wire's bytes four to a word, first on top, as HSDMA
 * stores them; positions in it count bytes since the stream began.  A
 * transfer runs to the ring's end or to the first byte not yet used,
 * whichever comes first, and the next starts as soon as it ends and there
 * is room: the CPU looks when the transfer should be done, after each
 * block, and between requests a timer does.  A block that wraps is put
 * together in one place first.  The residue is of words read from the
 * port, so the last two may still be on their way.
 *
 * While a transfer runs, the idle loop must not halt the CPU: HALT drops
 * the port's DMA requests, for good, and the transfer would stop dead.
 */
#define SD_STREAM_MIN		SD_BLOCKSIZE	/* the least worth a transfer */
#define SD_STREAM_SLACK		8

static inline u32 *sd_stream_at(const struct sd_stream *s, unsigned int i)
{
	return s->words + (i & (SD_STREAM_SIZE - 1)) / 4;
}

static inline u8 sd_stream_byte(const struct sd_stream *s, unsigned int i)
{
	return *sd_stream_at(s, i) >> (24 - 8 * (i % 4));
}

static void sd_idle_polls(struct s1c33_sd *host, bool polls)
{
	if (host->idle_polls != polls)
		cpu_idle_poll_ctrl(polls);
	host->idle_polls = polls;
}

/* The ring's bytes not in use: neither read yet nor still to come. */
static unsigned int sd_stream_free(const struct sd_stream *s)
{
	return SD_STREAM_SIZE - (s->end - (s->pos & ~3U));
}

/*
 * Stream on into the free part of the ring, as far as its end, the port
 * being idle.
 */
static int sd_stream_start(struct s1c33_sd *host, struct sd_stream *s)
{
	DEFINE_RAW_FLEX(struct dma_interleaved_template, xt, sgl, 1);
	struct dma_async_tx_descriptor *rxd, *txd;
	unsigned int len = min(sd_stream_free(s),
			       SD_STREAM_SIZE - (s->end & (SD_STREAM_SIZE - 1)));
	dma_cookie_t cookie;

	rxd = dmaengine_prep_slave_single(host->rx_chan, host->stream_dma +
					  (s->end & (SD_STREAM_SIZE - 1)),
					  len, DMA_DEV_TO_MEM, 0);
	if (!rxd)
		return -ENOMEM;
	s->rx_cookie = dmaengine_submit(rxd);
	if (s->rx_cookie < 0)
		return -EBUSY;
	s->running = true;	/* for sd_stream_close() to stop, from here */
	sd_idle_polls(host, true);
	host->timing.transfers++;
	/*
	 * All-ones from one word: the CPU writes the first to start.  A
	 * transfer of one word, just short of the ring's end, is that alone.
	 */
	if (len > 4) {
		xt->src_start = host->ones_dma;
		xt->src_inc = false;
		xt->dst_start = host->base_phys + SPI_TXD;
		xt->dst_inc = false;
		xt->dir = DMA_MEM_TO_DEV;
		xt->numf = 1;
		xt->frame_size = 1;
		xt->sgl[0].size = len - 4;
		txd = dmaengine_prep_interleaved_dma(host->tx_chan, xt, 0);
		if (!txd)
			return -ENOMEM;
		cookie = dmaengine_submit(txd);
		if (cookie < 0)
			return -EBUSY;
	}
	s->start = s->end;
	s->end += len;
	/* 32 bits at MCLK/(4 << divider) a word, and the DMA's own few. */
	s->due = get_cycles() + len / 4 * (32 * (4 << host->divider) + 16);
	if (sd_wait(host, SPI_BUSY, false))
		return -ETIMEDOUT;
	sd_dma_go(host);
	return 0;
}

/*
 * Bring the stream up to date: how far the transfer under way has come,
 * and if it has ended, the next one started if there is room.
 */
static int sd_stream_service(struct s1c33_sd *host, struct sd_stream *s)
{
	struct dma_tx_state state;
	enum dma_status status;
	unsigned int in;

	if (s->running) {
		status = dmaengine_tx_status(host->rx_chan, s->rx_cookie,
					     &state);
		if (status == DMA_IN_PROGRESS) {
			in = s->end - state.residue;
			if (in >= s->start + SD_STREAM_SLACK)
				s->avail = max(s->avail, in - SD_STREAM_SLACK);
			return 0;
		}
		if (status != DMA_COMPLETE)
			return -EIO;
		s->avail = s->end;
		s->running = false;
		host->dma_blocks++;
		sd_idle_polls(host, false);
	}
	/* A ring nearly full waits for the CPU to use some. */
	if (sd_stream_free(s) < SD_STREAM_MIN)
		return 0;
	return sd_stream_start(host, s);
}

/* The same, but only once the transfer under way should have ended. */
static int sd_stream_kick(struct s1c33_sd *host, struct sd_stream *s)
{
	if (s->running && (s32)(get_cycles() - s->due) < 0)
		return 0;
	return sd_stream_service(host, s);
}

/* Until @need bytes from s->pos are in. */
static int sd_stream_wait(struct s1c33_sd *host, struct sd_stream *s,
			  unsigned int need, u32 *t)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(100);
	int ret;

	while (s->avail - s->pos < need) {
		ret = sd_stream_service(host, s);
		if (ret)
			return ret;
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
	}
	sd_charge(host, SD_T_POLL, t);
	return 0;
}

/*
 * Between requests: keep the stream going until the ring is full, looking
 * when each transfer should have ended.  An error leaves the stream for the
 * next request to close, since stopping it here could sleep.
 */
static enum hrtimer_restart sd_stream_timer(struct hrtimer *timer)
{
	struct s1c33_sd *host = container_of(timer, struct s1c33_sd,
					     stream_timer);
	struct sd_stream *s = &host->stream;
	s32 left;

	if (sd_stream_service(host, s)) {
		s->broken = true;
		return HRTIMER_NORESTART;
	}
	if (!s->running)
		return HRTIMER_NORESTART;
	left = max_t(s32, s->due - get_cycles(), 1000);
	hrtimer_forward_now(timer, ns_to_ktime(div_u64((u64)left * NSEC_PER_SEC,
						       host->clock)));
	return HRTIMER_RESTART;
}

/* Stop the stream's transfer, if any, and empty the port. */
static void sd_stream_halt(struct s1c33_sd *host)
{
	struct sd_stream *s = &host->stream;

	s->open = false;
	if (!s->running)
		return;
	sd_dma_stop(host);
	s->running = false;
	sd_idle_polls(host, false);
	/* A word the DMA never took would answer a command's first. */
	sd_wait(host, SPI_BUSY, false);
	while (readl(host->base + SPI_STAT) & SPI_RX_FULL)
		readl(host->base + SPI_RXD);
}

/*
 * End a stream left open, sending the stop command the last request's was
 * answered for.
 */
static void sd_stream_close(struct s1c33_sd *host)
{
	struct mmc_command stop = {
		.opcode = MMC_STOP_TRANSMISSION,
		.flags = MMC_RSP_SPI_R1B | MMC_RSP_R1B | MMC_CMD_AC,
	};

	if (!host->stream.open)
		return;
	sd_stream_halt(host);
	sd_command(host, &stop, false);
}

/* Leave the stream running between requests, with the timer to tend it. */
static void sd_stream_leave(struct s1c33_sd *host)
{
	struct sd_stream *s = &host->stream;

	if (!s->open)
		return;
	if (sd_stream_service(host, s)) {
		sd_stream_close(host);
		return;
	}
	if (s->running)
		hrtimer_start(&host->stream_timer, 0, HRTIMER_MODE_REL_SOFT);
}

/* Whether a read can be streamed: several whole blocks, each in one piece. */
static bool sd_can_stream(struct s1c33_sd *host, struct mmc_data *data)
{
	struct scatterlist *sg;
	unsigned int n_sg;

	if (!host->stream.words || host->no_stream || data->blocks < 2 ||
	    data->blksz != SD_BLOCKSIZE)
		return false;
	for_each_sg(data->sg, sg, data->sg_len, n_sg)
		if (sg->length % SD_BLOCKSIZE || (unsigned long)sg_virt(sg) & 3)
			return false;
	return true;
}

/*
 * Start a new stream: find the card's first token a word at a time, as
 * sd_read_start() does, and begin the stream with the block's bytes that
 * came in its word.
 */
static int sd_stream_first(struct s1c33_sd *host, unsigned long timeout)
{
	struct sd_stream *s = &host->stream;
	unsigned int lead, i;
	u32 word;
	int status;

	status = sd_rx_byte(host);
	if (status == 0xff || status == 0)
		status = sd_skip(host, timeout, 0xff);
	if (status < 0)
		return status;
	if (status != SPI_TOKEN_SINGLE) {
		/* A data error token: 0000xxxx */
		if (!(status & 0xf0))
			return -EIO;
		/* Shifted by a few bits: read a block at a time from now on. */
		host->no_stream = true;
		return -EILSEQ;
	}
	s->open = true;
	s->broken = false;
	s->pos = s->avail = s->start = s->end = 0;
	lead = host->rx_len - host->rx_pos;
	if (lead) {
		word = ~0U;
		for (i = 0; i < lead; i++)
			word = word << 8 | host->rx[host->rx_pos + i];
		s->words[0] = word;
		s->avail = s->end = 4;
		s->pos = 4 - lead;
	}
	sd_rx_drop(host);
	return 0;
}

/* Skip the next gap in the stream and take its token. */
static int sd_stream_token(struct s1c33_sd *host, unsigned long timeout,
			   u32 *t)
{
	unsigned long deadline = jiffies + timeout;
	struct sd_stream *s = &host->stream;
	unsigned int gap = 0;
	int ret;
	u8 token;

	for (;;) {
		while (s->pos != s->avail && sd_stream_byte(s, s->pos) == 0xff) {
			s->pos++;
			gap++;
		}
		if (s->pos != s->avail)
			break;
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
		ret = sd_stream_wait(host, s, 1, t);
		if (ret)
			return ret;
	}
	host->timing.gap_bytes += gap;
	token = sd_stream_byte(s, s->pos++);
	if (token == SPI_TOKEN_SINGLE)
		return 0;
	/* A data error token, 0000xxxx, or a stream out of step. */
	host->timing.errors++;
	return token & 0xf0 ? -EILSEQ : -EIO;
}

/*
 * Read @data's blocks from the stream: a new one, or with @more, the one
 * the last request left open, which has come on since.
 */
static int sd_read_stream(struct s1c33_sd *host, struct mmc_data *data,
			  bool more, unsigned long timeout, u32 *t)
{
	struct sd_stream *s = &host->stream;
	struct scatterlist *sg = data->sg;
	unsigned int offset = 0, left = data->blocks;
	unsigned int word, lead;
	int ret;
	u32 carry, *words;
	u16 crc;

	if (more) {
		ret = sd_stream_service(host, s);
		if (ret)
			goto out;
		host->timing.carried++;
		host->timing.in_hand += s->avail - s->pos;
	} else {
		ret = sd_stream_first(host, timeout);
		if (ret)
			goto out;
	}
	sd_charge(host, SD_T_TOKEN, t);

	for (;;) {
		/* A new stream's first token is in already. */
		if (more) {
			ret = sd_stream_token(host, timeout, t);
			sd_charge(host, SD_T_TOKEN, t);
			if (ret)
				goto out;
		}
		more = true;

		ret = sd_stream_wait(host, s, SD_BLOCKSIZE + 2, t);
		if (ret)
			goto out;
		while (offset >= sg->length) {
			sg = sg_next(sg);
			offset = 0;
		}
		words = sd_stream_at(s, s->pos);
		word = SD_STREAM_SIZE / 4 - (words - s->words);
		if (word < SD_STREAM_BLOCK / 4) {
			memcpy(s->wrap, words, word * 4);
			memcpy(s->wrap + word, s->words,
			       SD_STREAM_BLOCK - word * 4);
			words = s->wrap;
		}
		lead = -s->pos & 3;
		carry = lead ? swab32(*words) >> (8 * (4 - lead)) : 0;
		crc = host->unpack_crc(words + !!lead, sg_virt(sg) + offset,
				       SD_BLOCKSIZE / 4, carry, lead,
				       host->crc_tables);
		if (host->mmc->use_spi_crc &&
		    crc != (sd_stream_byte(s, s->pos + SD_BLOCKSIZE) << 8 |
			    sd_stream_byte(s, s->pos + SD_BLOCKSIZE + 1))) {
			host->timing.errors++;
			ret = -EILSEQ;
			goto out;
		}
		s->pos += SD_BLOCKSIZE + 2;
		offset += SD_BLOCKSIZE;
		data->bytes_xfered += SD_BLOCKSIZE;
		host->timing.blocks++;
		sd_charge(host, SD_T_CHECK, t);
		if (!--left)
			return 0;
		ret = sd_stream_kick(host, s);
		sd_charge(host, SD_T_SETUP, t);
		if (ret)
			goto out;
	}

out:
	/* The caller sends the stop command itself. */
	sd_stream_halt(host);
	return ret;
}

/*
 * Write one block: a gap byte, the token, the data and its CRC, then the
 * card's data-response byte and its busy signal.  Some cards answer a few
 * bits late, so the response is looked for bit by bit, as mmc_spi does.
 */
static int sd_write_block(struct s1c33_sd *host, const u8 *buf,
			  unsigned int len, bool multiple,
			  unsigned long timeout)
{
	u8 head[2] = { 0xff, multiple ? SPI_TOKEN_MULTI_WRITE :
					SPI_TOKEN_SINGLE };
	u16 crc = 0xffff;
	u8 reply[4], back[4];
	u32 pattern, in;
	int status;

	if (host->mmc->use_spi_crc)
		crc = sd_crc(host->crc_tables, buf, len);

	/*
	 * The CRC goes out in a word of its own, padded with all-ones, and
	 * the card starts its response straight after it: the word's last two
	 * bytes back are the first two of the reply.
	 */
	status = sd_tx(host, head, sizeof(head));
	if (!status)
		status = sd_write_words(host, buf, len);
	if (!status)
		status = sd_word(host, crc << 16 | 0xffff, &in);
	if (!status) {
		put_unaligned_be32(in, back);
		sd_rx_queue(host, back + 2, 2);
		status = sd_rx_bytes(host, reply, sizeof(reply));
	}
	if (status)
		return status;

	/* The first three bits are undefined; the code follows the first 0. */
	pattern = get_unaligned_be32(reply) | 0xe0000000;
	while (pattern & 0x80000000)
		pattern <<= 1;
	pattern >>= 27;

	switch (pattern) {
	case SPI_RESPONSE_ACCEPTED:
		break;
	case SPI_RESPONSE_CRC_ERR:
		return -EILSEQ;
	case SPI_RESPONSE_WRITE_ERR:
		return -EIO;
	default:
		return -EPROTO;
	}

	/* Done if the busy signal already ended within the reply bytes. */
	if (reply[3] & 0x01)
		return 0;
	return sd_wait_unbusy(host, timeout);
}

/* @more: the read carries on from the stream the last one left open. */
static void sd_data(struct s1c33_sd *host, struct mmc_data *data, bool more)
{
	bool multiple = data->blocks > 1;
	struct scatterlist *sg;
	unsigned long timeout;
	unsigned int n_sg;
	int status = 0;
	u32 t;

	timeout = data->timeout_ns / 1000 + data->timeout_clks * 1000000 /
		(host->clock >> (host->divider + 2));
	timeout = usecs_to_jiffies(timeout) + 1;

	if (!(data->flags & MMC_DATA_WRITE)) {
		if (sd_can_stream(host, data)) {
			u32 t = sd_clock(host);
			int ret = sd_read_stream(host, data, more, timeout, &t);

			if (ret)
				data->error = ret;
			for_each_sg(data->sg, sg, data->sg_len, n_sg)
				flush_dcache_page(sg_page(sg));
			sd_charge(host, SD_T_REQUEST, &t);
			host->timing.requests++;
			return;
		}
		sd_read(host, data, timeout);
		return;
	}
	t = sd_clock(host);

	for_each_sg(data->sg, sg, data->sg_len, n_sg) {
		const u8 *buf = sg_virt(sg);
		unsigned int length = sg->length;

		while (length) {
			unsigned int len = min(length, data->blksz);

			status = sd_write_block(host, buf, len, multiple,
						timeout);
			if (status)
				break;
			data->bytes_xfered += len;
			host->timing.written++;
			buf += len;
			length -= len;
			if (!multiple)
				break;
		}
		if (status) {
			data->error = status;
			break;
		}
	}

	/* A multiple-block write ends with its own token, then busy. */
	if (multiple) {
		u8 stop[2] = { SPI_TOKEN_STOP_TRAN, 0xff };

		status = sd_tx(host, stop, sizeof(stop));
		if (!status)
			status = sd_wait_unbusy(host, timeout);
		if (status && !data->error)
			data->error = status;
	}
	sd_charge(host, SD_T_WRITE, &t);
}

/* The read command, if any, that would carry on where @mrq leaves off. */
static u32 sd_next_arg(struct s1c33_sd *host, struct mmc_request *mrq)
{
	struct mmc_card *card = host->mmc->card;
	u32 blocks = mrq->data->blocks;

	if (!card)
		return ~0U;
	return mrq->cmd->arg + (mmc_card_is_blockaddr(card) ? blocks : blocks << 9);
}

static void sd_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct s1c33_sd *host = mmc_priv(mmc);
	struct sd_stream *s = &host->stream;
	int crc_retry = 5;
	u32 t = sd_clock(host);
	bool more;
	int status;

	hrtimer_cancel(&host->stream_timer);
retry:
	more = s->open && !s->broken &&
		mrq->cmd->opcode == MMC_READ_MULTIPLE_BLOCK &&
		mrq->cmd->arg == s->next_arg && mrq->data &&
		sd_can_stream(host, mrq->data);
	if (more) {
		/* Its blocks are coming in already: no command to send. */
		mrq->cmd->resp[0] = 0;
		mrq->cmd->error = 0;
		status = 0;
	} else {
		sd_stream_close(host);
		status = sd_command(host, mrq->cmd, mrq->data != NULL);
		host->timing.commands++;
	}
	sd_charge(host, SD_T_COMMAND, &t);
	if (status == 0 && mrq->data) {
		sd_data(host, mrq->data, more);
		t = sd_clock(host);

		/*
		 * The odd CRC error is recovered from by stopping and
		 * repeating the command, as mmc_spi does.
		 */
		if (mrq->data->error == -EILSEQ && crc_retry--) {
			struct mmc_command stop = {
				.opcode = MMC_STOP_TRANSMISSION,
				.flags = MMC_RSP_SPI_R1B | MMC_RSP_R1B |
					MMC_CMD_AC,
			};

			sd_command(host, &stop, false);
			mrq->data->error = 0;
			mrq->data->bytes_xfered = 0;
			goto retry;
		}

		if (s->open && mrq->stop) {
			/* Answered for, and sent when the stream ends. */
			s->next_arg = sd_next_arg(host, mrq);
			mrq->stop->resp[0] = 0;
			mrq->stop->error = 0;
		} else if (mrq->stop) {
			sd_command(host, mrq->stop, false);
		} else {
			sd_deselect(host);
		}
	}
	sd_stream_leave(host);
	sd_charge(host, SD_T_COMMAND, &t);
	mmc_request_done(mmc, mrq);
}

/****************************************************************************/
/* Power and clock */

/*
 * See 6.4.1 in the simplified SD physical layer specification 2.0.  mmc_spi
 * first waits for the card to stop signalling busy; this runs only just
 * after power is applied, when the card drives nothing and its data line
 * reads zeros on this board, so that wait always ran out: 3 s of every
 * boot.  Instead, once it has had its clocks, the card is reset until it
 * answers that it is idle.  A card fresh from power-up takes its time
 * (longer than the 10 ms the platform gives it), and the core sends its own
 * reset only once, then gives up on a card it cannot rescan.
 */
static void sd_initsequence(struct s1c33_sd *host)
{
	struct mmc_command cmd = {
		.opcode = MMC_GO_IDLE_STATE,
		.flags = MMC_RSP_SPI_R1 | MMC_RSP_NONE | MMC_CMD_BC,
	};
	unsigned long start = jiffies;
	unsigned int i;
	u32 in;

	/* Skip what any earlier command left behind. */
	sd_select(host);
	for (i = 0; i < 3; i++)
		sd_word(host, ~0U, &in);
	/* At least 74 clocks with the card deselected, before CMD0. */
	gpiod_set_value(host->cs, 0);
	for (i = 0; i < 5; i++)
		sd_word(host, ~0U, &in);
	sd_rx_drop(host);

	for (;;) {
		cmd.error = 0;
		cmd.resp[0] = 0;
		if (!sd_command(host, &cmd, false) &&
		    (cmd.resp[0] & 0xff) == R1_SPI_IDLE)
			break;
		if (time_after(jiffies, start + msecs_to_jiffies(SD_READY_MS))) {
			dev_warn(host->dev, "card not answering after %u ms\n",
				 SD_READY_MS);
			return;
		}
		msleep(10);
	}
	dev_info(host->dev, "card answered after %u ms\n",
		 jiffies_to_msecs(jiffies - start));
}

static void sd_setpower(struct s1c33_sd *host, unsigned short vdd)
{
	struct mmc_host *mmc = host->mmc;

	if (vdd) {
		mmc_regulator_set_ocr(mmc, mmc->supply.vmmc, vdd);
		mmc_regulator_enable_vqmmc(mmc);
	} else {
		mmc_regulator_disable_vqmmc(mmc);
		mmc_regulator_set_ocr(mmc, mmc->supply.vmmc, 0);
	}
}

static void sd_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct s1c33_sd *host = mmc_priv(mmc);
	u32 in;

	hrtimer_cancel(&host->stream_timer);
	sd_stream_close(host);
	if (ios->clock) {
		unsigned int divider = 0;

		while (divider < 7 && host->clock >> (divider + 2) > ios->clock)
			divider++;
		host->divider = divider;
	}

	if (host->power_mode == ios->power_mode)
		return;
	switch (ios->power_mode) {
	case MMC_POWER_OFF:
		sd_setpower(host, 0);
		/* Leave the card's inputs low: MOSI by sending zeroes. */
		gpiod_set_value(host->cs, 0);
		sd_word(host, 0, &in);
		msleep(10);
		break;
	case MMC_POWER_UP:
		sd_setpower(host, ios->vdd);
		msleep(host->pdata->powerup_msecs);
		break;
	case MMC_POWER_ON:
		sd_initsequence(host);
		break;
	}
	host->power_mode = ios->power_mode;
}

static const struct mmc_host_ops sd_ops = {
	.request	= sd_request,
	.set_ios	= sd_set_ios,
	.get_ro		= mmc_gpio_get_ro,
	.get_cd		= mmc_gpio_get_cd,
};

/****************************************************************************/
/* Probe */

/*
 * HSDMA 2 and 3 on the port's transmit and receive requests, and a block
 * of all-ones for the transmitter.  Without them, reads go through the
 * byte queue.
 */
static void sd_unmap_ones(struct s1c33_sd *host, struct device *dma_dev)
{
	if (host->ones_phys)
		dma_unmap_resource(dma_dev, host->ones_dma, SD_BLOCKSIZE,
				   DMA_TO_DEVICE, 0);
	else
		dma_unmap_single(dma_dev, host->ones_dma, SD_BLOCKSIZE,
				 DMA_TO_DEVICE);
}

static void sd_free_stream(struct s1c33_sd *host, struct device *dma_dev)
{
	if (host->stream.words)
		dma_free_coherent(dma_dev, SD_STREAM_SIZE, host->stream.words,
				  host->stream_dma);
	host->stream.words = NULL;
}

static int sd_dma_init(struct s1c33_sd *host)
{
	struct dma_slave_config config = {
		.src_addr = host->base_phys + SPI_RXD,
		.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
		.dst_addr = host->base_phys + SPI_TXD,
		.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
	};
	struct device *dev = host->dev;
	struct device *dma_dev;
	unsigned int i;
	int ret;

	host->rx_chan = dma_request_chan(dev, "rx");
	if (IS_ERR(host->rx_chan)) {
		ret = PTR_ERR(host->rx_chan);
		host->rx_chan = NULL;
		return ret == -ENODEV ? 0 : ret;
	}
	host->tx_chan = dma_request_chan(dev, "tx");
	if (IS_ERR(host->tx_chan)) {
		ret = PTR_ERR(host->tx_chan);
		host->tx_chan = NULL;
		goto err;
	}
	sg_init_table(&host->rx_sg, 1);
	config.direction = DMA_DEV_TO_MEM;
	ret = dmaengine_slave_config(host->rx_chan, &config);
	config.direction = DMA_MEM_TO_DEV;
	if (!ret)
		ret = dmaengine_slave_config(host->tx_chan, &config);
	if (ret)
		goto err;

	dma_dev = dmaengine_get_dma_device(host->tx_chan);
	if (host->ones_phys) {
		host->ones_dma = dma_map_resource(dma_dev, host->ones_phys,
						  SD_BLOCKSIZE, DMA_TO_DEVICE, 0);
	} else {
		host->ones = devm_kmalloc(dev, SD_BLOCKSIZE, GFP_KERNEL);
		if (!host->ones) {
			ret = -ENOMEM;
			goto err;
		}
		memset(host->ones, 0xff, SD_BLOCKSIZE);
		host->ones_dma = dma_map_single(dma_dev, host->ones,
						SD_BLOCKSIZE, DMA_TO_DEVICE);
	}
	if (dma_mapping_error(dma_dev, host->ones_dma)) {
		ret = -ENOMEM;
		goto err;
	}
	/* Streamed reads are an optimisation: go without if there is no room. */
	host->stream.words = dma_alloc_coherent(dma_dev, SD_STREAM_SIZE,
						&host->stream_dma, GFP_KERNEL);
	for (i = 0; i < ARRAY_SIZE(host->ones_txd); i++) {
		host->ones_txd[i] = dmaengine_prep_slave_single(host->tx_chan,
				host->ones_dma, SD_BLOCKSIZE - 4,
				DMA_MEM_TO_DEV, 0);
		if (!host->ones_txd[i] ||
		    dmaengine_desc_set_reuse(host->ones_txd[i])) {
			ret = -ENOMEM;
			goto err_unmap;
		}
	}
	return 0;

err_unmap:
	for (i = 0; i < ARRAY_SIZE(host->ones_txd); i++)
		if (host->ones_txd[i])
			dmaengine_desc_free(host->ones_txd[i]);
	sd_free_stream(host, dma_dev);
	sd_unmap_ones(host, dma_dev);

err:
	if (host->tx_chan)
		dma_release_channel(host->tx_chan);
	dma_release_channel(host->rx_chan);
	host->rx_chan = host->tx_chan = NULL;
	return ret;
}

static void sd_dma_release(struct s1c33_sd *host)
{
	if (!host->rx_chan)
		return;
	hrtimer_cancel(&host->stream_timer);
	sd_stream_halt(host);
	/* The last transmit transfer is never asked after; retire it. */
	dmaengine_terminate_sync(host->tx_chan);
	dmaengine_desc_free(host->ones_txd[0]);
	dmaengine_desc_free(host->ones_txd[1]);
	sd_free_stream(host, dmaengine_get_dma_device(host->tx_chan));
	sd_unmap_ones(host, dmaengine_get_dma_device(host->tx_chan));
	dma_release_channel(host->tx_chan);
	dma_release_channel(host->rx_chan);
}

/* s1c33_sd.timing=1 on the command line counts from boot, as read_timing's 1. */
static bool timing_from_boot;
module_param_named(timing, timing_from_boot, bool, 0444);

static int sd_probe(struct platform_device *pdev)
{
	const struct s1c33_sd_platform_data *pdata =
		dev_get_platdata(&pdev->dev);
	struct device *dev = &pdev->dev;
	struct s1c33_sd *host;
	struct mmc_host *mmc;
	struct resource *res;
	struct clk *clk;
	int ret;

	if (!pdata)
		return -EINVAL;
	mmc = devm_mmc_alloc_host(dev, sizeof(*host));
	if (!mmc)
		return -ENOMEM;
	host = mmc_priv(mmc);
	host->mmc = mmc;
	host->dev = dev;
	host->pdata = pdata;
	host->control = ~0U;
	host->power_mode = MMC_POWER_OFF;
	host->timing.on = timing_from_boot;
	hrtimer_setup(&host->stream_timer, sd_stream_timer, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL_SOFT);

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "cannot enable input clock\n");
	/* Nothing changes MCLK while Linux runs. */
	host->clock = clk_get_rate(clk);
	if (!host->clock)
		return dev_err_probe(dev, -EINVAL, "input clock has no rate\n");

	host->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(host->base))
		return PTR_ERR(host->base);
	host->base_phys = res->start;
	host->spi_flags = devm_platform_ioremap_resource_byname(pdev,
								"spi-flags");
	if (IS_ERR(host->spi_flags))
		return PTR_ERR(host->spi_flags);
	host->idma = devm_platform_ioremap_resource_byname(pdev, "idma");
	if (IS_ERR(host->idma))
		return PTR_ERR(host->idma);
	host->crc_tables = c33_iram_alloc(SD_CRC_TABLES_SIZE);
	if (!host->crc_tables)
		host->crc_tables = devm_kmalloc(dev, SD_CRC_TABLES_SIZE,
						GFP_KERNEL);
	if (!host->crc_tables)
		return -ENOMEM;
	sd_crc_tables_init(host->crc_tables);
	host->unpack_crc = c33_iram_func(sd_unpack_crc);
	/* The all-ones for the transmitter, in internal RAM if there is some. */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sram");
	if (res) {
		void __iomem *sram = devm_ioremap_resource(dev, res);

		if (IS_ERR(sram))
			return PTR_ERR(sram);
		if (resource_size(res) < SD_BLOCKSIZE)
			return dev_err_probe(dev, -EINVAL, "sram too small\n");
		memset_io(sram, 0xff, SD_BLOCKSIZE);
		host->ones = (__force u8 *)sram;
		host->ones_phys = res->start;
	}

	host->cs = devm_gpiod_get(dev, "cs", GPIOD_OUT_LOW);
	if (IS_ERR(host->cs))
		return dev_err_probe(dev, PTR_ERR(host->cs),
				     "cannot claim chip select\n");

	/* The driver core has applied the default state already. */
	host->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(host->pinctrl))
		return dev_err_probe(dev, PTR_ERR(host->pinctrl),
				     "cannot get pin control\n");
	host->pins_default = pinctrl_lookup_state(host->pinctrl,
						  PINCTRL_STATE_DEFAULT);
	host->pins_hold = pinctrl_lookup_state(host->pinctrl, "hold");
	if (IS_ERR(host->pins_default) || IS_ERR(host->pins_hold)) {
		dev_warn(dev, "no SCLK hold state: resizing characters may clock the card\n");
		host->pins_hold = NULL;
	}

	/*
	 * The loader that ran before Linux may have left the intelligent DMA
	 * controller asking this port for service, or its causes raised.
	 */
	writeb(readb(host->idma + ITC_IDMA_ENABLE) & ~ITC_IDMA_SPI_BIT,
	       host->idma + ITC_IDMA_ENABLE);
	writeb(readb(host->idma + ITC_IDMA_REQ) & ~ITC_IDMA_SPI_BIT,
	       host->idma + ITC_IDMA_REQ);
	writeb(ITC_SPI_DMA_FLAGS, host->spi_flags);
	writel(0, host->base + SPI_CTL2);
	writel(0, host->base + SPI_WAIT);
	writel(0, host->base + SPI_INT);
	readl(host->base + SPI_RXD);

	mmc->ops = &sd_ops;
	mmc->caps = MMC_CAP_SPI | MMC_CAP_NONREMOVABLE;
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;
	mmc->f_min = max(host->clock >> 9, 400000UL);
	mmc->f_max = host->clock >> 2;
	mmc->max_blk_size = SD_BLOCKSIZE;
	mmc->max_segs = SD_BLOCKSATONCE;
	mmc->max_blk_count = SD_BLOCKSATONCE;
	mmc->max_req_size = SD_BLOCKSATONCE * SD_BLOCKSIZE;
	mmc->max_seg_size = mmc->max_req_size;
	ret = mmc_regulator_get_supply(mmc);
	if (ret)
		return ret;
	ret = sd_dma_init(host);
	if (ret)
		return dev_err_probe(dev, ret, "cannot get DMA channels\n");
	platform_set_drvdata(pdev, host);

	ret = mmc_add_host(mmc);
	if (ret) {
		sd_dma_release(host);
		return ret;
	}
	dev_info(dev, "SD host %s at up to %u Hz, %s block reads\n",
		 mmc_hostname(mmc), mmc->f_max,
		 host->stream.words ? "streamed HSDMA" :
		 host->rx_chan ? "HSDMA" : "programmed I/O");
	return 0;
}

static void sd_remove(struct platform_device *pdev)
{
	struct s1c33_sd *host = platform_get_drvdata(pdev);

	mmc_remove_host(host->mmc);
	sd_dma_release(host);
}

static const char *const sd_phase_names[SD_T_PHASES] = {
	[SD_T_TOKEN] = "token", [SD_T_SETUP] = "setup", [SD_T_AHEAD] = "ahead",
	[SD_T_CHECK] = "check", [SD_T_POLL] = "poll",
	[SD_T_STATUS] = "status", [SD_T_TAIL] = "tail",
	[SD_T_REQUEST] = "request", [SD_T_COMMAND] = "command",
	[SD_T_WRITE] = "write",
};

/*
 * Cycles a block by phase since timing was last turned on, "1" to turn it
 * on (and clear it), "0" off.  Each figure includes one reading of the
 * clock, the overhead line.
 */
static ssize_t read_timing_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct s1c33_sd *host = dev_get_drvdata(dev);
	struct sd_timing *tm = &host->timing;
	u32 blocks = tm->blocks ? tm->blocks : 1, t0, t1;
	u64 total = 0;
	int len, i;

	t0 = get_cycles();
	t1 = get_cycles();
	len = sysfs_emit(buf, "%s, %u blocks in %u requests, clock read %u cycles\n"
			 "%u commands, %u blocks written\n"
			 "streamed: %u transfers, %u gap bytes, %u errors; %u requests carried on, %u KB in hand\n",
			 tm->on ? "on" : "off", tm->blocks, tm->requests,
			 t1 - t0, tm->commands, tm->written,
			 tm->transfers, tm->gap_bytes, tm->errors,
			 tm->carried, tm->in_hand >> 10);
	for (i = 0; i < SD_T_PHASES; i++) {
		total += tm->cycles[i];
		len += sysfs_emit_at(buf, len, "%-8s %7llu cycles a block\n",
				     sd_phase_names[i],
				     div_u64(tm->cycles[i], blocks));
	}
	len += sysfs_emit_at(buf, len, "%-8s %7llu cycles a block\n", "total",
			     div_u64(total, blocks));
	return len;
}

static ssize_t read_timing_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct s1c33_sd *host = dev_get_drvdata(dev);
	bool on;
	int ret = kstrtobool(buf, &on);

	if (ret)
		return ret;
	/* A read in flight at the switch is counted in part: harmless. */
	if (on)
		memset(&host->timing, 0, sizeof(host->timing));
	WRITE_ONCE(host->timing.on, on);
	return count;
}
static DEVICE_ATTR_RW(read_timing);

static struct attribute *sd_attrs[] = {
	&dev_attr_read_timing.attr,
	NULL
};
ATTRIBUTE_GROUPS(sd);

static struct platform_driver s1c33_sd_driver = {
	.driver.name	= "s1c33-sd",
	.driver.dev_groups = sd_groups,
	.probe		= sd_probe,
	.remove		= sd_remove,
};
module_platform_driver(s1c33_sd_driver);

MODULE_DESCRIPTION("Epson S1C33 SD card host, SPI mode");
MODULE_LICENSE("GPL");
