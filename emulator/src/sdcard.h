#ifndef SDCARD_H
#define SDCARD_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "mem.h"

#define SD_RESP_MAX 600      /* token + 512 data + CRC, with headroom */

struct sdcard {
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

	/* SPI controller state */
	uint8_t   rxd;
	bool      rdff;

	unsigned long commands, blocks_read;
	bool trace;
	unsigned long xfers;
};

bool sd_attach(struct mem *m, struct sdcard *sd, const char *image_path);
void sd_close(struct sdcard *sd);

#endif /* SDCARD_H */
