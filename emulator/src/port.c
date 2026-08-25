/*
 * I/O ports (REG_BASE+0x380).
 *
 * Only the data registers matter here, and only because two of the bits are
 * SPI chip selects: port 5 bit 0 is the SD card and bit 2 is the serial
 * EEPROM (samo.h and boards/samo_a1.h). Without these the emulator cannot
 * tell which device the SPI controller is talking to.
 *
 * They have to read back what was written, for the same reason the interrupt
 * priority and clock registers do: the drivers set and clear these bits with
 * read-modify-write, so a register that reads as zero would drop every other
 * bit in the port on each access.
 *
 * Reset state is board-specific -- the manual lists the port data registers
 * as "Ext.", meaning they read the external pin. Here that means the two
 * chip selects idle high (deasserted) and everything else low. In
 * particular the three buttons on port 6 bits 0..2 must read zero: the boot
 * menu treats any non-zero value there as a button press and stops to show
 * its menu instead of booting on.
 */

#include <string.h>

#include "port.h"

static bool port_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
		      bool is_write)
{
	struct port *p = ctx;
	uint32_t i = off - PORT_BASE;

	if (i + size > PORT_LEN)
		return false;

	if (is_write) {
		for (unsigned k = 0; k < size; k++)
			p->reg[i + k] = (uint8_t)(*val >> (8 * k));
		p->writes++;
		return true;
	}
	*val = 0;
	for (unsigned k = 0; k < size; k++)
		*val |= (uint32_t)p->reg[i + k] << (8 * k);
	return true;
}

bool port_cs_low(const struct port *p, unsigned bit)
{
	return (p->reg[OFF_P5D] & (1u << bit)) == 0;
}

void port_attach(struct mem *m, struct port *p)
{
	memset(p, 0, sizeof *p);
	p->reg[OFF_P5D] = (1u << CS_SDCARD_BIT) | (1u << CS_EEPROM_BIT);
	p->reg[OFF_P6D] = 0;                   /* no buttons held */
	mem_add_mmio(m, "ports", PORT_BASE, PORT_LEN, port_mmio, p);
}
