#ifndef PORT_H
#define PORT_H

#include <stdint.h>
#include <stdbool.h>

#include "mem.h"

/* I/O port block, REG_BASE+0x380..0x3bf. */
#define PORT_BASE 0x0380u
#define PORT_LEN  0x0040u

#define OFF_P5D   (0x38au - PORT_BASE)

/* Chip selects on this board (samo-lib/include/samo.h, boards/samo_a1.h).
   Both active low, both on port 5. */
#define CS_SDCARD_BIT 0
#define CS_EEPROM_BIT 2

/* Port 6 bits 0..2 are the three front buttons, read by the boot menu. */
#define OFF_P6D   (0x38cu - PORT_BASE)
#define BUTTON_MASK 0x07

struct port {
	uint8_t reg[PORT_LEN];
	unsigned long writes;
};

void port_attach(struct mem *m, struct port *p);
/* True while the given port-5 chip select is asserted (driven low). */
bool port_cs_low(const struct port *p, unsigned bit);

#endif /* PORT_H */
