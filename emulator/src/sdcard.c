/*
 * SPI controller (REG_BASE+0x1700) plus an SD card in SPI mode.
 *
 * The driver side is samo-lib/drivers/src/SPI.c:
 *
 *     uint8_t SPI_exchange(uint8_t out) {
 *         REG_SPI_TXD = out;
 *         do {} while (~REG_SPI_STAT & RDFF);
 *         return REG_SPI_RXD;
 *     }
 *
 * so each TXD store clocks exactly one byte in both directions. We model the
 * card as a byte-stream state machine: framing comes from the command bytes
 * themselves (bit7 clear, bit6 set) rather than from chip select, which is
 * enough for the FatFs-style driver in samo-lib/drivers/src/mmc.c.
 *
 * We report an SDHC card (OCR CCS set), so command arguments are block
 * numbers rather than byte offsets.
 */

#include <string.h>

#include "sdcard.h"

#define SPI_BASE   0x1700u
#define SPI_LEN    0x0020u

#define OFF_RXD    0x00
#define OFF_TXD    0x04
#define OFF_CTL1   0x08
#define OFF_WAIT   0x10
#define OFF_STAT   0x14

/*
 * SPI status register, 0x301714 (S1C33E07 Technical Manual, V.2):
 *
 *   D6 BSYF  transfer busy
 *   D5 MFEF  mode fault error
 *   D4 TDEF  transmit data empty
 *   D3 RDOF  receive data overflow
 *   D2 RDFF  receive data full
 *
 * MFEF cannot occur -- this is the only master on the bus.
 */
#define BSYF       (1u << 6)   /* transfer busy */
#define RDFF       (1u << 2)   /* receive data full  */
#define RDOF       (1u << 3)   /* receive data overflow */
#define TDEF       (1u << 4)   /* transmit data empty */

#define R1_IDLE    0x01
#define TOKEN_DATA 0xFE

static void push(struct sdcard *sd, uint8_t b)
{
	if (sd->resp_len < SD_RESP_MAX)
		sd->resp[sd->resp_len++] = b;
}

static void respond(struct sdcard *sd, uint8_t r1)
{
	sd->resp_len = sd->resp_pos = 0;
	sd->block_timing = false;
	push(sd, 0xFF);          /* Ncr: at least one idle byte before R1 */
	push(sd, r1);
}

static bool read_block(struct sdcard *sd, uint32_t blk, uint8_t *out)
{
	if (!sd->img)
		return false;
	if (fseeko(sd->img, (off_t)blk * 512, SEEK_SET) != 0)
		return false;
	size_t n = fread(out, 1, 512, sd->img);
	if (n < 512)
		memset(out + n, 0, 512 - n);   /* past end of image reads zero */
	sd->blocks_read++;
	return true;
}

static void queue_block(struct sdcard *sd, uint32_t blk)
{
	uint8_t buf[512];
	read_block(sd, blk, buf);
	/*
	 * Idle filler before the data token. A real card holds the line at
	 * 0xff while fetching the next block, and the driver depends on it:
	 * send_cmd() calls wait_ready(), which spins until it reads 0xff, so
	 * without a gap the CMD12 ending a CMD18 transfer can never be sent.
	 * rcvr_datablock() already skips leading 0xff while hunting the token.
	 */
	push(sd, 0xFF);
	push(sd, 0xFF);
	if (sd->trace)
		fprintf(stderr, "   blk %u: %02x %02x %02x .. %02x %02x  (sig %02x%02x)\n",
			blk, buf[0], buf[1], buf[2], buf[510], buf[511],
			buf[510], buf[511]);
	push(sd, TOKEN_DATA);
	sd->block_first_pos = sd->resp_len;
	for (int i = 0; i < 512; i++)
		push(sd, buf[i]);
	sd->block_last_pos = sd->resp_len - 1;
	sd->block_timing = true;
	push(sd, 0xFF);          /* CRC16, unchecked by the driver */
	push(sd, 0xFF);
}

static void execute(struct sdcard *sd)
{
	uint8_t idx = sd->cmd[0] & 0x3F;
	uint32_t arg = ((uint32_t)sd->cmd[1] << 24) | ((uint32_t)sd->cmd[2] << 16) |
		       ((uint32_t)sd->cmd[3] << 8) | sd->cmd[4];
	bool app = sd->expect_acmd;

	sd->expect_acmd = false;
	sd->commands++;
	sd->streaming = false;

	if (sd->trace && sd->commands <= 400)
		fprintf(stderr, "  SD %s%u arg=%#010x\n", app ? "ACMD" : "CMD",
			idx, arg);

	if (app && idx == 41) {                 /* ACMD41: initialise */
		sd->idle = false;
		respond(sd, 0x00);
		return;
	}
	if (app && idx == 13) {                 /* ACMD13: SD_STATUS */
		respond(sd, 0x00);
		return;
	}

	switch (idx) {
	case 1:                                  /* SEND_OP_COND (MMC) */
		/*
		 * The driver falls back to CMD1 even though our CMD8/R7 reply
		 * satisfies its SDv2 test; answering it keeps initialisation
		 * moving. A card initialised via CMD1 is byte-addressed.
		 */
		sd->idle = false;
		sd->byte_addressed = true;
		respond(sd, 0x00);
		break;

	case 0:                                  /* GO_IDLE_STATE */
		sd->idle = true;
		respond(sd, R1_IDLE);
		break;

	case 8:                                  /* SEND_IF_COND -> R7 */
		respond(sd, R1_IDLE);
		push(sd, 0x00); push(sd, 0x00);
		push(sd, 0x01); push(sd, 0xAA);      /* echoes 2.7-3.6V, 0xAA */
		break;

	case 55:                                 /* APP_CMD */
		sd->expect_acmd = true;
		respond(sd, sd->idle ? R1_IDLE : 0x00);
		break;

	case 58:                                 /* READ_OCR -> R3 */
		respond(sd, 0x00);
		push(sd, 0xC0); push(sd, 0xFF);      /* powered up, CCS=1 */
		push(sd, 0x80); push(sd, 0x00);
		break;

	case 16:                                 /* SET_BLOCKLEN */
	case 12:                                 /* STOP_TRANSMISSION */
		respond(sd, 0x00);
		break;

	case 9:                                  /* SEND_CSD */
	case 10: {                               /* SEND_CID */
		respond(sd, 0x00);
		push(sd, TOKEN_DATA);
		uint8_t reg[16];
		memset(reg, 0, sizeof reg);
		if (idx == 9) {
			/* CSD v2.0: capacity = (C_SIZE+1) * 512 KiB */
			uint32_t csize = (uint32_t)(sd->blocks / 1024) - 1;
			reg[0] = 0x40;
			reg[7] = (csize >> 16) & 0x3F;
			reg[8] = (csize >> 8) & 0xFF;
			reg[9] = csize & 0xFF;
		}
		for (int i = 0; i < 16; i++)
			push(sd, reg[i]);
		push(sd, 0xFF); push(sd, 0xFF);
		break;
	}

	case 17: {                               /* READ_SINGLE_BLOCK */
		uint32_t blk = sd->byte_addressed ? arg / 512 : arg;
		respond(sd, 0x00);
		queue_block(sd, blk);
		break;
	}

	case 18: {                               /* READ_MULTIPLE_BLOCK */
		uint32_t blk = sd->byte_addressed ? arg / 512 : arg;
		respond(sd, 0x00);
		queue_block(sd, blk);
		sd->streaming = true;
		sd->stream_blk = blk + 1;
		break;
	}

	case 24:                                 /* WRITE_BLOCK */
	case 25: {                               /* WRITE_MULTIPLE_BLOCK */
		uint32_t blk = sd->byte_addressed ? arg / 512 : arg;
		respond(sd, 0x00);
		sd->awaiting_token = true;
		sd->write_multi = (idx == 25);
		sd->write_blk = blk;
		sd->wlen = 0;
		break;
	}

	default:
		respond(sd, 0x04);                   /* illegal command */
		break;
	}
}

/*
 * A completed data block. The response byte is the card's verdict: 0x05
 * accepted, 0x0d rejected on a write error, which is what a write-protected
 * image reports so the guest sees a failure rather than losing the data
 * quietly. The zero after it is the card holding the line busy while it
 * programs, which is what the driver's wait_ready() is looking for.
 */
static void finish_block(struct sdcard *sd)
{
	bool wrote = false;

	if (sd->img && !sd->readonly &&
	    sd->write_blk < sd->blocks &&
	    fseeko(sd->img, (off_t)sd->write_blk * 512, SEEK_SET) == 0 &&
	    fwrite(sd->wbuf, 1, 512, sd->img) == 512) {
		fflush(sd->img);
		sd->blocks_written++;
		wrote = true;
	}
	if (sd->trace)
		fprintf(stderr, "  SD write block %u %s\n", sd->write_blk,
			wrote ? "ok" : "REJECTED");

	sd->resp_len = sd->resp_pos = 0;
	push(sd, wrote ? 0x05 : 0x0d);
	push(sd, 0x00);                          /* busy while programming */

	sd->receiving = false;
	sd->write_blk++;
	sd->wlen = 0;
	if (!sd->write_multi)
		sd->awaiting_token = false;
}

static uint8_t pop_response(struct sdcard *sd)
{
	int pos = sd->resp_pos++;

	if (sd->clock && sd->block_timing) {
		if (pos == sd->block_first_pos)
			sd->block_start = sd->deadline - sd->character_cycles;
		if (pos == sd->block_last_pos) {
			uint64_t elapsed = sd->deadline - sd->block_start;
			sd->payload_cycles += elapsed;
			if (!sd->payloads_timed || elapsed < sd->payload_min)
				sd->payload_min = elapsed;
			if (elapsed > sd->payload_max)
				sd->payload_max = elapsed;
			sd->payloads_timed++;
			sd->block_timing = false;
		}
	}
	return sd->resp[pos];
}

/* One SPI byte exchange: host sends `out`, card returns a byte. */
static uint8_t sd_xfer(struct sdcard *sd, uint8_t out)
{
	if (sd->collecting) {
		sd->cmd[sd->cmdlen++] = out;
		if (sd->cmdlen == 6) {
			sd->collecting = false;
			execute(sd);
		}
		return 0xFF;
	}

	/*
	 * Data being written. Nothing here may be mistaken for a command:
	 * these bytes are file contents.
	 */
	if (sd->receiving) {
		if (sd->wlen < 512)
			sd->wbuf[sd->wlen++] = out;
		else if (++sd->crc_seen == 2)
			finish_block(sd);
		return 0xFF;
	}

	if (sd->awaiting_token) {
		if (sd->resp_pos < sd->resp_len)
			return pop_response(sd);
		if (out == 0xFE || out == 0xFC) {    /* single / multi start */
			sd->receiving = true;
			sd->wlen = 0;
			sd->crc_seen = 0;
			return 0xFF;
		}
		if (out == 0xFD) {                   /* STOP_TRAN */
			sd->awaiting_token = false;
			sd->write_multi = false;
		}
		return 0xFF;                         /* host is polling ready */
	}

	/*
	 * During a multi-block transfer the host interrupts with CMD12 at a
	 * point of its choosing, so commands must be recognised even with
	 * queued data still pending -- the host only ever clocks 0xff while
	 * receiving, so a command-shaped byte really is a command. Any
	 * remaining stream data is discarded when the command starts.
	 */
	if (sd->streaming && (out & 0xC0) == 0x40) {
		sd->resp_len = sd->resp_pos = 0;
		sd->block_timing = false;
		sd->streaming = false;
		sd->collecting = true;
		sd->cmdlen = 0;
		sd->cmd[sd->cmdlen++] = out;
		return 0xFF;
	}

	if (sd->resp_pos < sd->resp_len)
		return pop_response(sd);

	/*
	 * A command frame starts with bit7 clear and bit6 set. This must be
	 * tested BEFORE refilling a multi-block stream: the host clocks 0xff
	 * while receiving data, so a byte that looks like a command really is
	 * one -- in particular the CMD12 that stops a CMD18 transfer. Checking
	 * the stream first swallowed CMD12 and streamed forever.
	 */
	if ((out & 0xC0) == 0x40) {
		sd->collecting = true;
		sd->cmdlen = 0;
		sd->cmd[sd->cmdlen++] = out;
		return 0xFF;
	}

	if (sd->streaming) {
		sd->resp_len = sd->resp_pos = 0;
		queue_block(sd, sd->stream_blk++);
		return pop_response(sd);
	}
	return 0xFF;
}

static unsigned spi_divider(const struct sdcard *sd)
{
	/* MCBR[2:0]: MCLK / (4 * 2^MCBR), manual table V.2.4.1. */
	return 4u << ((sd->spi_ctl1 >> 4) & 7u);
}

static unsigned spi_bits(const struct sdcard *sd)
{
	/* BPT[4:0] stores the number of bits minus one. */
	return ((sd->spi_ctl1 >> 10) & 0x1fu) + 1u;
}

static void complete_spi(struct sdcard *sd)
{
	uint8_t out = sd->txd;

	/* Overrun occurs when a newly shifted character replaces unread RXD. */
	if (sd->rdff) {
		sd->rdof = true;
		sd->overflows++;
	}

	/*
	 * Route by chip select. The FLASH driver frames a transaction with
	 * EEPROM_CS_LO/HI, so releasing the select resets its command state.
	 */
	bool ee = sd->eeprom && sd->port &&
		  port_cs_low(sd->port, CS_EEPROM_BIT);
	if (!ee && sd->eeprom_selected && sd->eeprom)
		eeprom_deselect(sd->eeprom);
	sd->eeprom_selected = ee;

	if (ee)
		sd->rxd = eeprom_exchange(sd->eeprom, out);
	else
		sd->rxd = sd_xfer(sd, out);
	sd->busy = false;
	sd->rdff = true;
	if (sd->trace_bytes)
		fprintf(stderr, "   spi[%02lu] -> %02x  <- %02x%s\n",
			sd->xfers, out, sd->rxd,
			sd->collecting ? " (cmd)" : "");
	sd->xfers++;
	if (sd->dma_event)
		sd->dma_event(sd->dma_ctx);
}

static void start_spi(struct sdcard *sd, uint8_t out)
{
	sd->txd = out;
	if (!sd->clock) {
		complete_spi(sd);
		return;
	}

	uint64_t now = *sd->clock;
	uint64_t start = now > sd->next_start ? now : sd->next_start;
	uint64_t duration = (uint64_t)spi_bits(sd) * spi_divider(sd);
	sd->wait_cycles += start - now;
	sd->shift_cycles += duration;
	sd->character_cycles = duration;
	sd->deadline = start + duration;
	sd->busy = true;
}

void sd_poll(struct sdcard *sd)
{
	if (!sd->clock || !sd->busy || *sd->clock < sd->deadline)
		return;
	uint64_t observed = *sd->clock;
	uint64_t completion = sd->deadline;

	/*
	 * SPI_WAIT stores wait clocks minus one. Its clock is the divided SPI
	 * clock, not MCLK. A sufficiently slow CPU or DMA response naturally
	 * hides this delay; start_spi() only charges the part still outstanding.
	 */
	uint64_t wait = ((uint64_t)(sd->spi_wait & 0xffffu) + 1u) *
			spi_divider(sd);
	sd->next_start = completion + wait;
	/*
	 * The shift completed at its deadline, not at the next CPU instruction
	 * boundary where we happened to poll it. Run the DMA callback on that
	 * exact event time, then merge any bus stall back into the CPU timeline.
	 */
	*sd->clock = completion;
	complete_spi(sd);
	if (*sd->clock < observed)
		*sd->clock = observed;
}

static bool spi_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
		     bool is_write)
{
	struct sdcard *sd = ctx;
	uint32_t reg = off - SPI_BASE;
	sd_poll(sd);

	if (is_write) {
		if (reg == OFF_TXD) {
			uint8_t out = (uint8_t)(*val & 0xff);
			/* Firmware observes TDEF/RDFF before writing another byte. */
			if (!sd->busy)
				start_spi(sd, out);
		} else if (reg == OFF_CTL1) {
			/* Implemented fields are D14:8 and D6:0; D7 is reserved. */
			sd->spi_ctl1 = *val & 0x7f7fu;
		} else if (reg == OFF_WAIT) {
			/* The manual defines 1..65536 SPI clocks via value + 1. */
			sd->spi_wait = *val & 0xffffu;
		}
		return true;   /* control registers accepted silently */
	}

	switch (reg) {
	case OFF_RXD:
		*val = sd->rxd;
		/* "RDOF is reset to 0 by reading data from the SPI Receive
		 * Data Register", as is RDFF. */
		sd->rdff = false;
		sd->rdof = false;
		return true;
	case OFF_STAT:
		*val = (sd->busy ? BSYF : TDEF) |
		       (sd->rdff ? RDFF : 0) | (sd->rdof ? RDOF : 0);
		return true;
	case OFF_CTL1:
		*val = sd->spi_ctl1;
		return true;
	case OFF_WAIT:
		*val = sd->spi_wait;
		return true;
	default:
		*val = 0;
		return true;
	}
}

void sd_set_dma_event(struct sdcard *sd, sd_dma_event_fn fn, void *ctx)
{
	sd->dma_event = fn;
	sd->dma_ctx = ctx;
}

void sd_set_clock(struct sdcard *sd, uint64_t *clock)
{
	sd->clock = clock;
}

bool sd_attach(struct mem *m, struct sdcard *sd, const char *path,
	       const struct port *port, struct eeprom *eeprom, bool readonly)
{
	memset(sd, 0, sizeof *sd);
	sd->idle = true;
	sd->port = port;
	sd->eeprom = eeprom;
	sd->readonly = readonly;

	if (path) {
		/*
		 * A card keeps what is written to it, so the image is opened
		 * for update and the guest's history, bookmarks and settings
		 * survive the run. An image that cannot be opened that way is
		 * still usable, just write-protected.
		 */
		sd->img = readonly ? NULL : fopen(path, "r+b");
		if (!sd->img) {
			sd->img = fopen(path, "rb");
			sd->readonly = true;
		}
		if (!sd->img)
			return false;
		fseeko(sd->img, 0, SEEK_END);
		sd->blocks = (uint64_t)ftello(sd->img) / 512;
		rewind(sd->img);
	}
	mem_add_mmio(m, "spi/sd", SPI_BASE, SPI_LEN, spi_mmio, sd);
	return true;
}

void sd_reset(struct sdcard *sd)
{
	sd->cmdlen = 0;
	sd->collecting = false;
	sd->resp_len = sd->resp_pos = 0;
	sd->idle = true;
	sd->spi_ctl1 = 0;
	sd->spi_wait = 0;
	sd->busy = false;
	sd->deadline = sd->next_start = 0;
	sd->character_cycles = 0;
	sd->block_timing = false;
	sd->rdff = sd->rdof = false;
	sd->rxd = 0xff;
	sd->eeprom_selected = false;
	sd->byte_addressed = false;
	sd->streaming = false;
}

void sd_close(struct sdcard *sd)
{
	if (sd->img)
		fclose(sd->img);
	sd->img = NULL;
}
