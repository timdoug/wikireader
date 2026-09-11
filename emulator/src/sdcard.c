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
 * In this byte mode each TXD store clocks one byte in both directions; the
 * DMA backend also uses 32-bit characters. The card remains a byte-stream
 * state machine. Port 5 bit 0 is its active-low chip
 * select; while deselected, MISO is released and reads back as 0xff.
 *
 * We report an SDHC card (OCR CCS set), so command arguments are block
 * numbers rather than byte offsets.
 */

#include <string.h>

#include "model.h"
#include "sdcard.h"

#define SPI_BASE   0x1700u
#define SPI_LEN    0x0020u

static unsigned spi_divider(const struct sdcard *sd);

#define OFF_RXD    0x00
#define OFF_TXD    0x04
#define OFF_CTL1   0x08
#define OFF_CTL2   0x0c
#define OFF_WAIT   0x10
#define OFF_STAT   0x14
#define OFF_INT    0x18
#define OFF_RXMASK 0x1c

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
	sd->token_ready = 0;
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

/*
 * A real card takes time to find a block after a read command; the host
 * sees 0xff until the data token.  The fitted model holds the token of the
 * block a command asked for until sd_read_latency cycles have passed.
 * Subsequent blocks use a separate gap: shortening command latency must
 * not make a long sequential transfer unrealistically faster as well.
 */
static void delay_token(struct sdcard *sd, unsigned long cycles)
{
	if (!sd->clock)
		return;
	sd->token_pos = sd->block_first_pos - 1;
	sd->token_ready = *sd->clock + cycles;
}

static bool initialization_ready(struct sdcard *sd)
{
	if (!sd->clock || !model.sd_init_latency)
		return true;
	if (!sd->initializing) {
		sd->initializing = true;
		sd->init_ready = *sd->clock + model.sd_init_latency;
	}
	return *sd->clock >= sd->init_ready;
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
		if (sd->idle && !initialization_ready(sd)) {
			respond(sd, R1_IDLE);
			return;
		}
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
		if (sd->idle && !initialization_ready(sd)) {
			respond(sd, R1_IDLE);
			break;
		}
		sd->idle = false;
		sd->byte_addressed = true;
		respond(sd, 0x00);
		break;

	case 0:                                  /* GO_IDLE_STATE */
		sd->idle = true;
		sd->initializing = false;
		sd->init_ready = 0;
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
			/*
			 * CSD v2.0: capacity = (C_SIZE+1) * 512 KiB, and every
			 * other field fixed by the spec at the value a real
			 * SDHC card reports. grifo reads only C_SIZE, but a
			 * host that believes TRAN_SPEED or READ_BL_LEN gets a
			 * zero clock and a 1-byte sector out of a CSD that is
			 * blank everywhere else.
			 */
			uint32_t csize = (uint32_t)(sd->blocks / 1024) - 1;
			static const uint8_t fixed[16] = {
				0x40, 0x0e, 0x00, 0x32, 0x5b, 0x59, 0x00, 0x00,
				0x00, 0x00, 0x7f, 0x80, 0x0a, 0x40, 0x00, 0x01,
			};
			memcpy(reg, fixed, sizeof reg);
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
		delay_token(sd, model.sd_read_latency);
		break;
	}

	case 18: {                               /* READ_MULTIPLE_BLOCK */
		uint32_t blk = sd->byte_addressed ? arg / 512 : arg;
		respond(sd, 0x00);
		queue_block(sd, blk);
		delay_token(sd, model.sd_read_latency);
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
	sd->write_ready = wrote && sd->clock ? *sd->clock + model.sd_write_latency : 0;

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
			sd->block_start = sd->byte_deadline - 8u * spi_divider(sd);
		if (pos == sd->block_last_pos) {
			uint64_t elapsed = sd->byte_deadline - sd->block_start;
			sd->payload_cycles += elapsed;
			if (!sd->payloads_timed || elapsed < sd->payload_min)
				sd->payload_min = elapsed;
			if (elapsed > sd->payload_max)
				sd->payload_max = elapsed;
			sd->payloads_timed++;
			sd->block_timing = false;
		}
	}
	uint8_t value = sd->resp[pos];
	if (sd->resp_bit) {
		uint8_t next = sd->resp_pos < sd->resp_len ? sd->resp[sd->resp_pos] : 0xff;
		value = (uint8_t)((value << sd->resp_bit) | (next >> (8 - sd->resp_bit)));
	}
	return value;
}

/* One SPI byte exchange: host sends `out`, card returns a byte. */
static uint8_t sd_xfer(struct sdcard *sd, uint8_t out)
{
	/* Return the data-response token first, then remain busy even across
	 * deselection. A new command or write token cannot bypass programming. */
	if (sd->write_ready && sd->resp_pos >= sd->resp_len) {
		if (sd->clock && *sd->clock < sd->write_ready)
			return 0x00;
		sd->write_ready = 0;
	}
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

	if (sd->resp_pos < sd->resp_len) {
		if (sd->token_ready && sd->resp_pos == sd->token_pos) {
			if (sd->clock && *sd->clock < sd->token_ready)
				return 0xFF;         /* card still seeking */
			sd->token_ready = 0;
		}
		return pop_response(sd);
	}

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
		delay_token(sd, model.sd_read_gap);
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

static void start_queued_spi(struct sdcard *sd);

static void dma_event(struct sdcard *sd, unsigned request)
{
	/* V.2.7: TXDE/RXDE independently gate the corresponding ITC cause. */
	unsigned enable = request == SPI_DMA_TX ? 8u : 4u;
	if (sd->dma_event && (sd->spi_ctl1 & enable))
		sd->dma_event(sd->dma_ctx, request);
}

static void complete_spi(struct sdcard *sd)
{
	uint32_t out = sd->txd;
	unsigned bits = spi_bits(sd);
	/* The attached SD/EEPROM models exchange whole bytes. Larger SPI
	 * characters carry consecutive bytes, MSB first (manual V.2.5). */
	unsigned bytes = bits == 32 ? 4 : bits == 16 ? 2 : 1;

	/* Overrun occurs when a newly shifted character replaces unread RXD. */
	if (sd->rdff) {
		sd->rdof = true;
		sd->overflows++;
	}

	/* Route the shared SPI bus by the two active-low chip selects. */
	bool ee = sd->eeprom && sd->port &&
		  port_cs_low(sd->port, CS_EEPROM_BIT);
	bool card = !sd->port || port_cs_low(sd->port, CS_SDCARD_BIT);
	if (!ee && sd->eeprom_selected && sd->eeprom)
		eeprom_deselect(sd->eeprom);
	sd->eeprom_selected = ee;

	if (!ee && !card) {
		/*
		 * Deselect aborts a queued response or data phase. In particular,
		 * release_spi() raises CS and clocks one byte before CMD12; a real
		 * card then releases MISO. Keeping a CMD18 stream alive here made
		 * wait_ready() drain an unrequested sector using CPU PIO.
		 */
		sd->cmdlen = 0;
		sd->collecting = false;
		sd->resp_len = sd->resp_pos = 0;
		sd->resp_bit = 0;
		sd->streaming = false;
		sd->awaiting_token = false;
		sd->receiving = false;
		sd->write_multi = false;
		sd->block_timing = false;
	}
	sd->rxd = 0;
	for (unsigned i = 0; i < bytes; i++) {
		unsigned shift = 8 * (bytes - i - 1);
		uint8_t tx = (uint8_t)(out >> shift);
		sd->byte_deadline = sd->deadline -
			(uint64_t)(bytes - i - 1) * 8 * spi_divider(sd);
		uint8_t rx = ee ? eeprom_exchange(sd->eeprom, tx) :
			card ? sd_xfer(sd, tx) : 0xff;
		sd->rxd = (sd->rxd << 8) | rx;
	}
	if (sd->spi_rxmask & 2u) {
		unsigned valid = ((sd->spi_rxmask >> 10) & 31u) + 1u;
		if (valid < 32)
			sd->rxd &= (1u << valid) - 1u;
	}
	sd->busy = false;
	sd->shifting = false;
	sd->rdff = true;
	if (sd->trace_bytes)
		fprintf(stderr, "   spi[%02lu] -> %02x  <- %02x%s\n",
			sd->xfers, out, sd->rxd,
			sd->collecting ? " (cmd)" : "");
	sd->xfers++;
	/* A buffered word enters the inter-character wait independently of
	 * RX DMA bus traffic. Its shift-start event remains on the wire clock. */
	if (sd->clock && sd->tx_full)
		start_queued_spi(sd);
	dma_event(sd, SPI_DMA_RX);
	if (!sd->clock && !sd->busy && sd->tx_full)
		start_queued_spi(sd);
}

static void load_spi_shift(struct sdcard *sd)
{
	uint64_t duration = (uint64_t)spi_bits(sd) * spi_divider(sd);
	sd->txd = sd->tx_buffer;
	sd->tx_full = false;
	sd->busy = sd->shifting = true;
	sd->shift_cycles += duration;
	sd->character_cycles = duration;
	sd->deadline = (sd->clock ? *sd->clock : 0) + duration;
	dma_event(sd, SPI_DMA_TX);
	if (!sd->clock)
		complete_spi(sd);
}

static void start_queued_spi(struct sdcard *sd)
{
	sd->busy = true;
	if (sd->clock && *sd->clock < sd->next_start) {
		sd->shifting = false;
		sd->wait_cycles += sd->next_start - *sd->clock;
		sd->deadline = sd->next_start;
	} else {
		load_spi_shift(sd);
	}
}

void sd_poll(struct sdcard *sd)
{
	if (!sd->clock || sd->polling)
		return;
	sd->polling = true;
	while (sd->busy && *sd->clock >= sd->deadline) {
		uint64_t observed = *sd->clock;
		*sd->clock = sd->deadline;
		if (!sd->shifting) {
			load_spi_shift(sd);
		} else {
			/* V.2.5: TXD is consumed only AFTER SPI_WAIT expires. */
			sd->next_start = sd->deadline +
				((uint64_t)sd->spi_wait + 1u) * spi_divider(sd);
			complete_spi(sd);
		}
		if (*sd->clock < observed)
			*sd->clock = observed;
	}
	sd->polling = false;
}

static bool spi_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
		     bool is_write)
{
	struct sdcard *sd = ctx;
	uint32_t reg = off - SPI_BASE;
	sd_poll(sd);
	/* Diagnose violations of V.2.8, without inventing a particular chip
	 * malfunction for operations the manual leaves undefined. */
	if (sd->busy && (reg == OFF_CTL1 || reg == OFF_CTL2 || reg == OFF_WAIT))
		sd->busy_control_accesses++;

	if (is_write) {
		if (reg == OFF_TXD) {
			/* V.2.5: one TXD word can wait behind the shift register. */
			if (!sd->tx_full) {
				sd->tx_buffer = *val;
				sd->tx_full = true;
				if (!sd->busy)
					start_queued_spi(sd);
			}
		} else if (reg == OFF_CTL1) {
			if ((sd->spi_ctl1 & 1u) && !(*val & 1u) && sd->spi_int)
				sd->unsafe_disables++;
			/* Hardware probe, 2026-09-08: an ENA cycle while CS is
			 * low loses one bit of the queued SD response, even at BPT=8.
			 * Represent the observed net effect at disable, without
			 * claiming which physical edge causes it. Holding P67 as
			 * GPIO keeps this transition off the card's clock pin.
			 * Command/write bit assembly and electrical mux transients
			 * remain outside this read-response model. */
			if ((sd->spi_ctl1 & 3u) == 3u && !(*val & 1u) &&
			    !(sd->spi_ctl1 & (1u << 8)) && sd->port &&
			    port_cs_low(sd->port, CS_SDCARD_BIT) &&
			    (sd->port->reg[0x3ad - PORT_BASE] & 0xc0) == 0x40) {
				sd->unclamped_disables++;
				if (sd->resp_pos < sd->resp_len && ++sd->resp_bit == 8) {
					sd->resp_bit = 0;
					(void)pop_response(sd);
				}
			}
			/* Implemented fields are D14:8 and D6:0; D7 is reserved. */
			sd->spi_ctl1 = *val & 0x7f7fu;
		} else if (reg == OFF_WAIT) {
			/* The manual defines 1..65536 SPI clocks via value + 1. */
			sd->spi_wait = *val & 0xffffu;
		} else if (reg == OFF_INT) {
			sd->spi_int = *val & 0x1fu;
		} else if (reg == OFF_RXMASK) {
			sd->spi_rxmask = *val & 0x7c02u;
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
		*val = (sd->busy ? BSYF : 0) | (sd->tx_full ? 0 : TDEF) |
		       (sd->rdff ? RDFF : 0) | (sd->rdof ? RDOF : 0);
		return true;
	case OFF_CTL1:
		*val = sd->spi_ctl1;
		return true;
	case OFF_WAIT:
		*val = sd->spi_wait;
		return true;
	case OFF_INT:
		*val = sd->spi_int;
		return true;
	case OFF_RXMASK:
		*val = sd->spi_rxmask;
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
	sd->token_ready = sd->init_ready = sd->write_ready = 0;
	sd->initializing = false;
	sd->cmdlen = 0;
	sd->collecting = false;
	sd->resp_len = sd->resp_pos = 0;
	sd->idle = true;
	sd->spi_ctl1 = 0;
	sd->spi_int = sd->spi_rxmask = 0;
	sd->busy_control_accesses = sd->unsafe_disables = 0;
	sd->unclamped_disables = sd->resp_bit = 0;
	sd->spi_wait = 0;
	sd->busy = false;
	sd->tx_full = sd->shifting = sd->polling = false;
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
