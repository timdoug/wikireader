/*
 * SPI serial FLASH (PM25LV512).
 *
 * Command set from samo-lib/drivers/src/FLASH.c, as copied into
 * samo-lib/mbr/menu.c. Only the read paths matter for booting, but the
 * others are modelled because the boot menu reads status and can reflash.
 *
 * Framing comes from chip select, unlike the SD card model in sdcard.c: the
 * driver brackets every transaction with EEPROM_CS_LO/HI, and a command
 * runs until the select is released. That is why the emulator has to track
 * the port bits at all.
 */

#include <string.h>

#include "eeprom.h"

enum {
	CMD_WRITE_STATUS  = 0x01,
	CMD_PAGE_PROGRAM  = 0x02,
	CMD_READ_DATA     = 0x03,
	CMD_WRITE_DISABLE = 0x04,
	CMD_READ_STATUS   = 0x05,
	CMD_WRITE_ENABLE  = 0x06,
	CMD_FAST_READ     = 0x0b,
	CMD_SECTOR_ERASE  = 0x20,
	CMD_CHIP_ERASE    = 0xc7,
};

/* Status register: bit 0 WIP (never busy here), bit 1 WEL. */
#define ST_WEL 0x02

bool eeprom_load(struct eeprom *e, const char *path, FILE *log)
{
	memset(e, 0, sizeof *e);
	memset(e->data, 0xff, sizeof e->data);     /* erased FLASH reads 0xff */

	FILE *fp = fopen(path, "rb");
	if (!fp)
		return false;
	size_t n = fread(e->data, 1, sizeof e->data, fp);
	fclose(fp);
	e->present = true;
	if (log)
		fprintf(log, "eeprom: %s (%zu bytes)\n", path, n);
	return true;
}

void eeprom_deselect(struct eeprom *e)
{
	e->cmd = 0;
	e->phase = 0;
}

uint8_t eeprom_exchange(struct eeprom *e, uint8_t out)
{
	if (!e->present)
		return 0xff;

	if (e->cmd == 0) {
		e->cmd = out;
		e->phase = 0;
		e->addr = 0;
		e->commands++;
		switch (out) {
		case CMD_WRITE_ENABLE:  e->write_enabled = true;  break;
		case CMD_WRITE_DISABLE: e->write_enabled = false; break;
		case CMD_CHIP_ERASE:
			if (e->write_enabled)
				memset(e->data, 0xff, sizeof e->data);
			break;
		default: break;
		}
		return 0xff;
	}

	e->phase++;

	switch (e->cmd) {
	case CMD_READ_STATUS:
		return e->write_enabled ? ST_WEL : 0x00;

	case CMD_READ_DATA:
	case CMD_FAST_READ:
	case CMD_PAGE_PROGRAM:
	case CMD_SECTOR_ERASE: {
		/* three address bytes, then a dummy byte for fast read */
		unsigned addr_bytes = 3;
		unsigned dummy = (e->cmd == CMD_FAST_READ) ? 1 : 0;

		if (e->phase <= addr_bytes) {
			e->addr = (e->addr << 8) | out;
			if (e->phase == addr_bytes && e->cmd == CMD_SECTOR_ERASE &&
			    e->write_enabled) {
				uint32_t base = (e->addr & ~(EEPROM_SECTOR - 1))
						% EEPROM_SIZE;
				memset(e->data + base, 0xff, EEPROM_SECTOR);
			}
			return 0xff;
		}
		if (e->phase <= addr_bytes + dummy)
			return 0xff;

		if (e->cmd == CMD_PAGE_PROGRAM) {
			if (e->write_enabled) {
				/* real FLASH can only clear bits */
				e->data[e->addr % EEPROM_SIZE] &= out;
				e->bytes_written++;
			}
			e->addr++;
			return 0xff;
		}
		e->bytes_read++;
		return e->data[e->addr++ % EEPROM_SIZE];
	}

	default:
		return 0xff;
	}
}
