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
#include <linux/crc-itu-t.h>
#include <linux/crc7.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/gpio/consumer.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/slot-gpio.h>
#include <linux/module.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/scatterlist.h>
#include <linux/swab.h>
#include <linux/unaligned.h>

#include <linux/platform_data/s1c33-sd.h>

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
#define SD_BLOCKSATONCE		128
#define SD_R1B_TIMEOUT_MS	3000
#define SD_INIT_TIMEOUT_MS	3000

struct s1c33_sd {
	struct mmc_host *mmc;
	struct device *dev;
	void __iomem *base;
	phys_addr_t base_phys;
	void __iomem *spi_flags;	/* the port's ITC cause flags */
	void __iomem *idma;		/* its IDMA request and enable */
	struct dma_chan *rx_chan;	/* HSDMA 3 on SPI receive */
	struct dma_chan *tx_chan;	/* HSDMA 2 on SPI transmit */
	u8 *ones;			/* the all-ones HSDMA 2 sends */
	dma_addr_t ones_dma;
	/* Sends them for a block, resubmitted for every one. */
	struct dma_async_tx_descriptor *ones_txd;
	const struct s1c33_sd_platform_data *pdata;
	struct gpio_desc *cs;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins_default;
	struct pinctrl_state *pins_hold;	/* SCLK parked low, or NULL */
	unsigned long clock;		/* MCLK, read once at probe */
	unsigned int divider;		/* SCLK = MCLK >> (divider + 2) */
	u32 control;			/* CTL1 as programmed */
	unsigned char power_mode;
	u8 rx[8];			/* received, not yet read */
	unsigned int rx_len, rx_pos;
	unsigned long dma_blocks;
};

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
 * Read @len bytes, a multiple of four, as words into @rx, which is word
 * aligned: HSDMA 3 moves them to memory while HSDMA 2 feeds all-ones to
 * the transmitter, whose first word the CPU writes to start the exchange.
 * The CPU polls the receive channel's completion rather than sleeping for
 * an interrupt; the block takes a fraction of a millisecond, less than a
 * sleep and wakeup would, and a polling CPU does not halt, which on this
 * part would stop the DMA's request pipeline.  The words are left in wire
 * order, first byte on top.  -EAGAIN: this buffer cannot be mapped.
 */
static int sd_read_dma(struct s1c33_sd *host, dma_addr_t address,
		       unsigned int len)
{
	struct dma_async_tx_descriptor *rxd, *txd = host->ones_txd;
	dma_cookie_t rx_cookie, tx_cookie;
	int ret = 0;

	rxd = dmaengine_prep_slave_single(host->rx_chan, address, len,
					  DMA_DEV_TO_MEM, 0);
	if (len != SD_BLOCKSIZE)
		txd = dmaengine_prep_slave_single(host->tx_chan,
						  host->ones_dma, len - 4,
						  DMA_MEM_TO_DEV, 0);
	if (!rxd || !txd) {
		ret = -ENOMEM;
		goto out;
	}
	rx_cookie = dmaengine_submit(rxd);
	tx_cookie = dmaengine_submit(txd);
	writeb(ITC_SPI_DMA_FLAGS, host->spi_flags);
	dma_async_issue_pending(host->rx_chan);
	dma_async_issue_pending(host->tx_chan);
	writel(~0U, host->base + SPI_TXD);

	/*
	 * The port stays busy until the last word is in: HSDMA 2 refills the
	 * transmitter long before a word has shifted out.  Wait for that on a
	 * register, in a loop that runs from the fetch buffer, and only then
	 * ask the channels: each status poll runs hundreds of instructions
	 * from SDRAM, and those fetches slowed the transfer itself by half.
	 */
	sd_wait(host, SPI_BUSY, false);
	ret = sd_dma_wait(host->rx_chan, rx_cookie);
	if (!ret)
		ret = sd_dma_wait(host->tx_chan, tx_cookie);
	if (!ret && sd_wait(host, SPI_BUSY, false))
		ret = -ETIMEDOUT;
	if (!ret)
		host->dma_blocks++;
out:
	if (ret) {
		dmaengine_terminate_sync(host->rx_chan);
		dmaengine_terminate_sync(host->tx_chan);
	}
	return ret;
}

/*
 * Put DMA'd words, wire order with the first byte on top, back into memory
 * order and @lead bytes further on, behind the @lead bytes that came before
 * them; the @lead bytes pushed off the end go to @tail.  One pass: each
 * word is swapped and shifted in registers, with no second copy.
 */
static void sd_unpack(u32 *words, unsigned int n, const u8 *first,
		      unsigned int lead, u8 *tail)
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
	for (i = 0; i < lead; i++)
		tail[i] = carry >> (8 * i);
}

/*
 * Read a data block and its CRC, which follow a token some bytes into a
 * word: @lead of the block's bytes already came with it and wait in the
 * receive queue.  HSDMA writes only whole aligned words, so it reads the
 * rest onto the block where it lies, and sd_unpack() moves it up.  Bytes
 * past the CRC stay queued for the next token.  Buffers HSDMA cannot reach
 * go through the queue a byte at a time.
 */
static int sd_read_data(struct s1c33_sd *host, u8 *buf, dma_addr_t dma,
			unsigned int len, u8 *crc)
{
	unsigned int lead = host->rx_len - host->rx_pos;
	u8 first[4], tail[4];
	int ret;

	if (dma == DMA_MAPPING_ERROR || dma & 3) {
		ret = sd_rx_bytes(host, buf, len);
		return ret ? ret : sd_rx_bytes(host, crc, 2);
	}
	memcpy(first, host->rx + host->rx_pos, lead);
	sd_rx_drop(host);
	ret = sd_read_dma(host, dma, len);
	if (ret)
		return ret;
	sd_unpack((u32 *)buf, len / 4, first, lead, tail);
	sd_rx_queue(host, tail, lead);
	return sd_rx_bytes(host, crc, 2);
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
 * crc_itu_t(), compiled here so that its loop is aligned (see the
 * Makefile).  The loop is 26 bytes, which the C33 runs from its fetch
 * buffer only when it starts a 16-byte line; otherwise every byte of every
 * block fetches it again from SDRAM, about 70 cycles a byte.
 */
static u16 sd_crc(const u8 *buf, unsigned int len)
{
	u16 crc = 0;

	while (len--)
		crc = crc_itu_t_byte(crc, *buf++);
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
 * Read one block: skip the all-ones gap, find the start token, then the
 * data and its CRC.  A card may shift the token, and so everything after
 * it, by a few bits; that is undone here, as mmc_spi does.
 */
static int sd_read_block(struct s1c33_sd *host, u8 *buf, dma_addr_t dma,
			 unsigned int len, unsigned long timeout)
{
	unsigned int bitshift;
	u8 crc[2];
	u8 leftover;
	int status;

	/* At least one card sends a zero byte before the all-ones. */
	status = sd_rx_byte(host);
	if (status == 0xff || status == 0)
		status = sd_skip(host, timeout, 0xff);
	if (status < 0)
		return status;
	/* A data error token: 0000xxxx */
	if (!(status & 0xf0))
		return -EIO;

	bitshift = 7;
	while (status & 0x80) {
		status <<= 1;
		bitshift--;
	}
	leftover = status << 1;

	status = sd_read_data(host, buf, dma, len, crc);
	if (status)
		return status;

	if (bitshift) {
		unsigned int bitright = 8 - bitshift;
		unsigned int i;
		u8 temp;

		for (i = 0; i < len; i++) {
			temp = buf[i];
			buf[i] = leftover | (temp >> bitshift);
			leftover = temp << bitright;
		}
		for (i = 0; i < sizeof(crc); i++) {
			temp = crc[i];
			crc[i] = leftover | (temp >> bitshift);
			leftover = temp << bitright;
		}
	}

	if (host->mmc->use_spi_crc &&
	    get_unaligned_be16(crc) != sd_crc(buf, len))
		return -EILSEQ;
	return 0;
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
		crc = sd_crc(buf, len);

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

static void sd_data(struct s1c33_sd *host, struct mmc_data *data)
{
	bool multiple = data->blocks > 1;
	bool write = data->flags & MMC_DATA_WRITE;
	struct device *dma_dev = NULL;
	struct scatterlist *sg;
	unsigned long timeout;
	unsigned int n_sg;
	int status = 0;

	/* Reads map the request once, for HSDMA to fill block by block. */
	if (!write && host->rx_chan) {
		dma_dev = dmaengine_get_dma_device(host->rx_chan);
		if (dma_map_sg(dma_dev, data->sg, data->sg_len,
			       DMA_FROM_DEVICE) != data->sg_len)
			dma_dev = NULL;
	}

	timeout = data->timeout_ns / 1000 + data->timeout_clks * 1000000 /
		(host->clock >> (host->divider + 2));
	timeout = usecs_to_jiffies(timeout) + 1;

	for_each_sg(data->sg, sg, data->sg_len, n_sg) {
		u8 *buf = kmap(sg_page(sg)) + sg->offset;
		dma_addr_t dma = dma_dev ? sg_dma_address(sg) :
			DMA_MAPPING_ERROR;
		unsigned int length = sg->length;

		while (length) {
			unsigned int len = min(length, data->blksz);

			if (write)
				status = sd_write_block(host, buf, len,
							multiple, timeout);
			else
				status = sd_read_block(host, buf, dma, len,
						       timeout);
			if (status)
				break;
			data->bytes_xfered += len;
			buf += len;
			if (dma != DMA_MAPPING_ERROR)
				dma += len;
			length -= len;
			if (!multiple)
				break;
		}
		if (!write)
			flush_dcache_page(sg_page(sg));
		kunmap(sg_page(sg));
		if (status) {
			data->error = status;
			break;
		}
	}

	if (dma_dev)
		dma_unmap_sg(dma_dev, data->sg, data->sg_len, DMA_FROM_DEVICE);

	/* A multiple-block write ends with its own token, then busy. */
	if (write && multiple) {
		u8 stop[2] = { SPI_TOKEN_STOP_TRAN, 0xff };

		status = sd_tx(host, stop, sizeof(stop));
		if (!status)
			status = sd_wait_unbusy(host, timeout);
		if (status && !data->error)
			data->error = status;
	}
}

static void sd_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct s1c33_sd *host = mmc_priv(mmc);
	int crc_retry = 5;
	int status;

retry:
	status = sd_command(host, mrq->cmd, mrq->data != NULL);
	if (status == 0 && mrq->data) {
		sd_data(host, mrq->data);

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

		if (mrq->stop)
			sd_command(host, mrq->stop, false);
		else
			sd_deselect(host);
	}
	mmc_request_done(mmc, mrq);
}

/****************************************************************************/
/* Power and clock */

/* See 6.4.1 in the simplified SD physical layer specification 2.0. */
static void sd_initsequence(struct s1c33_sd *host)
{
	unsigned int i;
	u32 in;

	/* Let any earlier command finish, and skip what it left behind. */
	sd_select(host);
	sd_wait_unbusy(host, msecs_to_jiffies(SD_INIT_TIMEOUT_MS));
	for (i = 0; i < 3; i++)
		sd_word(host, ~0U, &in);
	/* At least 74 clocks with the card deselected, before CMD0. */
	gpiod_set_value(host->cs, 0);
	for (i = 0; i < 5; i++)
		sd_word(host, ~0U, &in);
	sd_rx_drop(host);
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
	config.direction = DMA_DEV_TO_MEM;
	ret = dmaengine_slave_config(host->rx_chan, &config);
	config.direction = DMA_MEM_TO_DEV;
	if (!ret)
		ret = dmaengine_slave_config(host->tx_chan, &config);
	if (ret)
		goto err;

	host->ones = devm_kmalloc(dev, SD_BLOCKSIZE, GFP_KERNEL);
	if (!host->ones) {
		ret = -ENOMEM;
		goto err;
	}
	memset(host->ones, 0xff, SD_BLOCKSIZE);
	dma_dev = dmaengine_get_dma_device(host->tx_chan);
	host->ones_dma = dma_map_single(dma_dev, host->ones, SD_BLOCKSIZE,
					DMA_TO_DEVICE);
	if (dma_mapping_error(dma_dev, host->ones_dma)) {
		ret = -ENOMEM;
		goto err;
	}
	host->ones_txd = dmaengine_prep_slave_single(host->tx_chan,
			host->ones_dma, SD_BLOCKSIZE - 4, DMA_MEM_TO_DEV, 0);
	if (!host->ones_txd || dmaengine_desc_set_reuse(host->ones_txd)) {
		ret = -ENOMEM;
		goto err_unmap;
	}
	return 0;

err_unmap:
	if (host->ones_txd)
		dmaengine_desc_free(host->ones_txd);
	dma_unmap_single(dma_dev, host->ones_dma, SD_BLOCKSIZE,
			 DMA_TO_DEVICE);

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
	dmaengine_desc_free(host->ones_txd);
	dma_unmap_single(dmaengine_get_dma_device(host->tx_chan),
			 host->ones_dma, SD_BLOCKSIZE, DMA_TO_DEVICE);
	dma_release_channel(host->tx_chan);
	dma_release_channel(host->rx_chan);
}

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
		 host->rx_chan ? "HSDMA" : "programmed I/O");
	return 0;
}

static void sd_remove(struct platform_device *pdev)
{
	struct s1c33_sd *host = platform_get_drvdata(pdev);

	mmc_remove_host(host->mmc);
	sd_dma_release(host);
}

static struct platform_driver s1c33_sd_driver = {
	.driver.name	= "s1c33-sd",
	.probe		= sd_probe,
	.remove		= sd_remove,
};
module_platform_driver(s1c33_sd_driver);

MODULE_DESCRIPTION("Epson S1C33 SD card host, SPI mode");
MODULE_LICENSE("GPL");
