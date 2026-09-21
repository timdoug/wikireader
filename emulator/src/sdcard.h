#ifndef SDCARD_H
#define SDCARD_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "mem.h"
#include "port.h"
#include "eeprom.h"

#define SD_RESP_MAX 600      /* token + 512 data + CRC, with headroom */

#define SPI_DMA_RX 0x10u
#define SPI_DMA_TX 0x20u
typedef void (*sd_dma_event_fn)(void *ctx, unsigned requests);

struct sdcard {
	int token_pos;            /* response index of a delayed data token */
	uint64_t token_ready;     /* MCLK time it may be delivered; 0 = none */
	uint64_t init_ready, write_ready;
	bool initializing;
	/*
	 * The SPI controller is shared. Chip select decides which device a
	 * byte goes to: port 5 bit 0 is this card, bit 2 the serial FLASH.
	 * Both optional -- with neither attached the bus reads back 0xff.
	 */
	const struct port *port;
	struct eeprom     *eeprom;
	bool               eeprom_selected;   /* to detect deselect edges */
	bool               card_powered;

	FILE     *img;
	uint64_t  blocks;

	/* command assembly: 6 bytes, 0x40|cmd then 4-byte arg then CRC */
	uint8_t   cmd[6];
	int       cmdlen;
	bool      collecting;

	/* response queue clocked out one byte per SPI exchange */
	uint8_t   resp[SD_RESP_MAX];
	int       resp_len, resp_pos;
	unsigned  resp_bit;       /* response bit offset after an unclamped ENA cycle */

	bool      idle;          /* still in IDLE (pre-ACMD41) */
	bool      expect_acmd;   /* previous command was CMD55 */
	bool      byte_addressed; /* CMD1-initialised cards address by byte */
	bool      streaming;     /* CMD18 multi-block read in progress */
	uint32_t  stream_blk;

	/*
	 * Write data phase. After CMD24/CMD25 the host sends a start token,
	 * 512 bytes and two CRC bytes, and the card answers with a data
	 * response. Block data is arbitrary, so these states have to be
	 * checked before the command sniffing further down: half of a
	 * compressed article looks exactly like a command frame.
	 */
	bool      awaiting_token; /* command accepted, waiting for 0xfe/0xfc/0xfd */
	bool      receiving;      /* inside the 512 data bytes plus CRC */
	bool      write_multi;    /* CMD25: more blocks may follow */
	uint32_t  write_blk;
	uint8_t   wbuf[512];
	int       wlen;
	int       crc_seen;
	bool      readonly;       /* image opened without write access */

	/* SPI controller state */
	uint32_t  spi_ctl1;
	uint32_t  spi_int;          /* stores enables; CPU IRQ delivery not modeled */
	uint32_t  spi_rxmask;
	unsigned long busy_control_accesses;
	unsigned long unsafe_disables;
	unsigned long unclamped_disables;
	uint32_t  spi_wait;
	uint32_t  txd;
	uint32_t  tx_buffer;
	bool      tx_full;
	bool      shifting;
	bool      polling;
	uint32_t  rxd;
	bool      busy;
	bool      rdff;
	bool      rdof;             /* receive data overflow, D3 of SPI_STAT */
	uint64_t *clock;             /* MCLK-cycle timeline, optional in unit tests */
	uint64_t  deadline;          /* next event: shift start or completion */
	uint64_t  next_start;        /* end of the mandatory inter-character wait */
	uint64_t  character_cycles;
	uint64_t  byte_deadline;     /* current wire byte within an SPI character */
	unsigned long overflows;
	unsigned long long shift_cycles;
	unsigned long long wait_cycles;

	/* Exact first-data-bit through last-data-bit timing for queued blocks. */
	bool      block_timing;
	int       block_first_pos, block_last_pos;
	uint64_t  block_start;
	unsigned long payloads_timed;
	unsigned long long payload_cycles;
	unsigned long long payload_min, payload_max;

	unsigned long commands, blocks_read, blocks_written;
	bool trace;
	bool trace_bytes;   /* per-byte SPI log; very verbose */
	unsigned long xfers;

	/* TX request at shift start; RX request at character completion. */
	sd_dma_event_fn dma_event;
	void            *dma_ctx;
};

bool sd_attach(struct mem *m, struct sdcard *sd, const char *image_path,
	       const struct port *port, struct eeprom *eeprom, bool readonly);
void sd_close(struct sdcard *sd);
/* Return the card to its just-powered state, keeping the image open. */
void sd_reset(struct sdcard *sd);
void sd_set_dma_event(struct sdcard *sd, sd_dma_event_fn fn, void *ctx);
/* Attach the guest MCLK timeline and complete transfers that have come due. */
void sd_set_clock(struct sdcard *sd, uint64_t *clock);
void sd_poll(struct sdcard *sd);

#endif /* SDCARD_H */
