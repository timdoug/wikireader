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
		uint8_t p6_before = p->reg[OFF_P6D];
		for (unsigned k = 0; k < size; k++)
			p->reg[i + k] = (uint8_t)(*val >> (8 * k));
		p->writes++;

		/*
		 * Watch for the shutdown signal. power_off() drives P63 as an
		 * output and toggles it forever, expecting the power supply
		 * outside the chip to notice and cut the rails. There is no
		 * way back from it in software, so a real device stops here
		 * and so should this one -- otherwise the emulator spins in
		 * that loop indefinitely, which is what it used to do after
		 * the 120 second idle timeout.
		 */
		if (i <= OFF_P6D && i + size > OFF_P6D &&
		    (p->reg[OFF_IOC6] & (1u << POWEROFF_BIT)) &&
		    ((p6_before ^ p->reg[OFF_P6D]) & (1u << POWEROFF_BIT))) {
			if (++p->power_off_toggles >= 2)
				p->power_off_requested = true;
		}
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

/*
 * Raise KINT0 if the buttons no longer match what the comparator was armed
 * with. grifo enables it with EK0 and re-arms from its handler by writing
 * the current state back to SCPK0, so this stays quiet until something
 * actually changes.
 */
static void kint_check(struct port *p, struct c33 *cpu)
{
	uint8_t mask = p->reg[OFF_SMPK0];
	if (!mask)
		return;
	if ((p->reg[OFF_P6D] & mask) == (p->reg[OFF_SCPK0] & mask))
		return;
	if (p->itc)
		itc_set_flag((struct itc *)p->itc, VECTOR_KEY_INPUT_0);
	c33_raise_irq(cpu, VECTOR_KEY_INPUT_0,
		      p->itc ? itc_priority(p->itc, VECTOR_KEY_INPUT_0) : 7);
}

void port_button(struct port *p, struct c33 *cpu, unsigned n, bool pressed)
{
	if (n > 2)
		return;
	if (pressed)
		p->reg[OFF_P6D] |= (uint8_t)(1u << n);
	else
		p->reg[OFF_P6D] &= (uint8_t)~(1u << n);
	p->button_events++;
	kint_check(p, cpu);
}

/*
 * The power switch is active low and edge triggered -- Button_initialise
 * clears SPPT3 in REG_PINTPOL_SPP07 and sets SEPT3 in REG_PINTEL_SEPT07 --
 * so the pin idles high and pressing pulls it down, and the interrupt is
 * the falling edge.
 *
 * How long it is held makes no difference. Button_PowerInterrupt queues a
 * BUTTON_DOWN and a BUTTON_UP together from that one edge, so the
 * application sees a complete press and release however briefly the switch
 * is touched. A tap powers the device off.
 */
void port_power_button(struct port *p, struct c33 *cpu, bool pressed)
{
	if (pressed)
		p->reg[OFF_P0D] &= (uint8_t)~(1u << POWER_BIT);   /* active low */
	else
		p->reg[OFF_P0D] |= (uint8_t)(1u << POWER_BIT);
	p->button_events++;

	if (!pressed)
		return;                       /* the edge is the press */
	if (p->itc)
		itc_set_flag((struct itc *)p->itc, VECTOR_PORT_INPUT_3);
	c33_raise_irq(cpu, VECTOR_PORT_INPUT_3,
		      p->itc ? itc_priority(p->itc, VECTOR_PORT_INPUT_3) : 7);
}

void port_attach(struct mem *m, struct port *p, const struct itc *itc)
{
	memset(p, 0, sizeof *p);
	p->itc = itc;
	p->reg[OFF_P5D] = (1u << CS_SDCARD_BIT) | (1u << CS_EEPROM_BIT);
	/*
	 * Port 6: the three buttons on bits 0..2 read 0 when not held, but
	 * bits 3..5 have pull-ups (REG_MISC_PUP6 in boards/samo_a1.h) and so
	 * idle high. Bit 4 matters more than it looks: grifo's Suspend()
	 * begins
	 *
	 *     if (0 == (REG_P6_P6D & 0x10)) return;   // in CTP receive
	 *
	 * so with it low the idle loop never suspends and spins at full
	 * speed, which is most of where a boot's instructions were going.
	 */
	/*
	 * Bits 3 and 5 idle high, bit 4 deliberately does not.
	 *
	 * All three have pull-ups per REG_MISC_PUP6, so the accurate reset
	 * value is 0x38. But bit 4 is what grifo's Suspend() tests:
	 *
	 *     if (0 == (REG_P6_P6D & 0x10)) return;   // in CTP receive
	 *
	 * Driving it high makes Suspend actually suspend, which is both more
	 * faithful and much faster -- it cut a boot from 8 billion
	 * instructions to 300 million, because the idle loop stops spinning.
	 * It also makes the machine unresponsive to touch, because the
	 * suspend path is not fully modelled yet: it halts with interrupts
	 * disabled and expects a 16-bit timer 2 underflow to wake it, and
	 * while HALT and the timer are now modelled, packets still are not
	 * delivered across a suspend/resume cycle. Until that works, an
	 * emulator that responds to input beats one that idles efficiently.
	 */
	p->reg[OFF_P6D] = (1u << 5) | (1u << 4) | (1u << 3);
	p->reg[OFF_P0D] = (1u << POWER_BIT);   /* power switch idles high */
	mem_add_mmio(m, "ports", PORT_BASE, PORT_LEN, port_mmio, p);
}
