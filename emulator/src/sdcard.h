#ifndef SDCARD_H
#define SDCARD_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "mem.h"
#include "port.h"
#include "eeprom.h"

#define SD_RESP_MAX 600      /* token + 512 data + CRC, with headroom */

struct sdcard {
	/*
	 * The SPI controller is shared. Chip select decides which device a
	 * byte goes to: port 5 bit 0 is this card, bit 2 the serial FLASH.
	 * Both optional -- with neither attached the bus reads back 0xff.
	 */
	const struct port *port;
	struct eeprom     *eeprom;
	bool               eeprom_selected;   /* to detect deselect edges */

	FILE     *img;
	uint64_t  blocks;

	/* command assembly: 6 bytes, 0x40|cmd then 4-byte arg then CRC */
	uint8_t   cmd[6];
	int       cmdlen;
	bool      collecting;

	/* response queue clocked out one byte per SPI exchange */
	uint8_t   resp[SD_RESP_MAX];
	int       resp_len, resp_pos;

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
	uint8_t   rxd;
	bool      rdff;
	bool      rdof;             /* receive data overflow, D3 of SPI_STAT */
	unsigned long overflows;

	unsigned long commands, blocks_read, blocks_written;
	bool trace;
	bool trace_bytes;   /* per-byte SPI log; very verbose */
	unsigned long xfers;
};

bool sd_attach(struct mem *m, struct sdcard *sd, const char *image_path,
	       const struct port *port, struct eeprom *eeprom, bool readonly);
void sd_close(struct sdcard *sd);
/* Return the card to its just-powered state, keeping the image open. */
void sd_reset(struct sdcard *sd);

#endif /* SDCARD_H */
