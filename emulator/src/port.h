#ifndef PORT_H
#define PORT_H

#include <stdint.h>
#include <stdbool.h>

#include "mem.h"
#include "c33.h"
#include "itc.h"

/* I/O port block, REG_BASE+0x380..0x3bf. */
#define PORT_BASE 0x0380u
#define PORT_LEN  0x0060u    /* through the key-input comparator at 0x3d4 */

#define OFF_P5D   (0x38au - PORT_BASE)

/* Chip selects on this board (samo-lib/include/samo.h, boards/samo_a1.h).
   Both active low, both on port 5. */
#define CS_SDCARD_BIT 0
#define CS_EEPROM_BIT 2

/* Port 6 bits 0..2 are the three front buttons, read by the boot menu. */
#define OFF_P6D   (0x38cu - PORT_BASE)
#define BUTTON_MASK 0x07

/*
 * Key-input interrupt comparator. The three buttons are P60..P62
 * (REG_KINTSEL_SPPK01 = 0x04 in grifo's Button_initialise); the controller
 * raises KINT0 when the selected port bits stop matching SCPK0, and the
 * handler re-arms by writing the new state back.
 */
#define OFF_P0D   (0x380u - PORT_BASE)
#define OFF_IOC6  (0x38du - PORT_BASE)

/*
 * The power switch is P03, with its own port interrupt rather than the key
 * comparator the three front buttons share.
 */
#define POWER_BIT 3
#define VECTOR_PORT_INPUT_3 19

/*
 * Shutdown is signalled on P63. power_off() in boards/samo_a1.h drives it
 * as an output and toggles it forever; circuitry outside the chip watches
 * for that and cuts the rails. Nothing in software ever returns from it.
 */
#define POWEROFF_BIT 3

#define OFF_SCPK0 (0x3d2u - PORT_BASE)
#define OFF_SMPK0 (0x3d4u - PORT_BASE)
#define VECTOR_KEY_INPUT_0 20

struct port {
	uint8_t reg[PORT_LEN];
	unsigned long writes;
	const struct itc *itc;
	unsigned long button_events;
	bool     power_off_requested;
	unsigned power_off_toggles;
};

void port_attach(struct mem *m, struct port *p, const struct itc *itc);
/* Press or release one of the three front buttons: 0 random, 1 search,
   2 history. Raises the key-input interrupt if the controller wants it. */
void port_button(struct port *p, struct c33 *cpu, unsigned n, bool pressed);
/* The power switch, which is a separate pin and a separate interrupt. */
void port_power_button(struct port *p, struct c33 *cpu, bool pressed);
/* True while the given port-5 chip select is asserted (driven low). */
bool port_cs_low(const struct port *p, unsigned bit);

#endif /* PORT_H */
