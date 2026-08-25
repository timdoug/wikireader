#ifndef EEPROM_H
#define EEPROM_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/*
 * Serial FLASH on the SPI bus: a PM25LV512 on this board
 * (EEPROM_PM25LV512 in boards/samo_a1.h), 64 KB, 4 KB sectors.
 *
 * This is where the boot chain starts. The mask ROM reads the first block
 * into RAM and runs it; that is samo-lib/mbr, which then loads further
 * applications from here.
 */
#define EEPROM_SIZE   (64u * 1024)
#define EEPROM_SECTOR (4u * 1024)

struct eeprom {
	uint8_t  data[EEPROM_SIZE];
	bool     present;

	/* command state machine, reset by each chip-select assertion */
	uint8_t  cmd;
	unsigned phase;            /* bytes consumed since the command byte */
	uint32_t addr;
	bool     write_enabled;

	unsigned long commands, bytes_read, bytes_written;
};

bool eeprom_load(struct eeprom *e, const char *path, FILE *log);
void eeprom_deselect(struct eeprom *e);
uint8_t eeprom_exchange(struct eeprom *e, uint8_t out);

#endif /* EEPROM_H */
