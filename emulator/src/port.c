/*
 * I/O ports (REG_BASE+0x380).
 *
 * The data registers provide the SPI chip selects and modeled board inputs.
 * Control registers retain their documented writable bits and configure the
 * key and power-switch interrupt paths.
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

#include <stdlib.h>
#include <string.h>

#include <stdio.h>

#include "mem.h"
#include "port.h"

/* Writable bits from the GPIO register tables; holes and reserved bits are 0. */
static uint8_t port_reg_mask(uint32_t i)
{
	if (i <= 0x13) {
		if (i == 0x0f)
			return 0;
		if (i == 0x06 || i == 0x07)
			return 0x7f;
		if (i == 0x0e)
			return 0x1f;
		if (i == 0x10 || i == 0x11)
			return 0x3f;
		return 0xff;
	}
	if (i >= 0x20 && i <= 0x33) {
		if (i == 0x27)
			return 0x3f;
		if (i == 0x2f)
			return 0x03;
		if (i == 0x31)
			return 0x0f;
		return 0xff;
	}
	if (i >= 0x40 && i <= 0x47)
		return 0xff;
	switch (i) {
	case 0x50: return 0x77;
	case 0x52: case 0x54: return 0x1f;
	case 0x53: case 0x55: return 0x0f;
	default: return 0;
	}
}

static bool port_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
		      bool is_write)
{
	struct port *p = ctx;
	uint32_t i = off - PORT_BASE;

	if (i + size > PORT_LEN)
		return false;

	if (is_write) {
		uint8_t p6_before = p->reg[OFF_P6D];
		uint8_t p5cfp_before = p->reg[OFF_P5CFP03];
		for (unsigned k = 0; k < size; k++) {
			uint8_t mask = port_reg_mask(i + k);
			uint8_t v = (uint8_t)(*val >> (8 * k));
			p->reg[i + k] = (p->reg[i + k] & (uint8_t)~mask)
					    | (v & mask);
		}
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
		/*
		 * P53 is SDA10, an address line of the SDRAM the program is
		 * running from. Taking it away stops the machine where it
		 * stands: no exception, no output, and the panel keeps its
		 * last contents, because the framebuffer is in internal RAM
		 * and needs nothing from the SDRAM to be scanned out. It
		 * looks exactly like a peripheral that will not answer, and
		 * it cost a day of looking in the wrong place.
		 *
		 * Modelling the consequence would mean corrupting every
		 * SDRAM access from here on. Saying what happened and
		 * stopping is the same outcome and a far better answer.
		 *
		 * The condition is the one that actually holds: the pin has
		 * gone and the program counter is in the memory it was
		 * addressing. Board setup runs from flash and sets this
		 * register as a whole byte on its way past -- before
		 * init_ram() puts SDA10 back -- and that is fine, because
		 * nothing is executing from the SDRAM yet.
		 */
		if (i <= OFF_P5CFP03 && i + size > OFF_P5CFP03 && p->cpu &&
		    (p5cfp_before & P53_FUNC_MASK) == P53_FUNC_SDA10 &&
		    (p->reg[OFF_P5CFP03] & P53_FUNC_MASK) != P53_FUNC_SDA10 &&
		    p->cpu->cur_pc - SDRAM_BASE < SDRAM_SIZE) {
			fprintf(stderr,
				"P53 taken off SDA10 while the SDRAM is running: "
				"the address line the program is executing over "
				"is gone. Write only the pins you own -- this "
				"register holds four of them.\n");
			c33_fault(p->cpu, "SDA10 disabled while SDRAM in use");
		}

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
		*val |= (uint32_t)(p->reg[i + k] & port_reg_mask(i + k))
			<< (8 * k);
	return true;
}

bool port_cs_low(const struct port *p, unsigned bit)
{
	return (p->reg[OFF_P5D] & (1u << bit)) == 0;
}

bool port_sd_powered(const struct port *p)
{
	return (p->reg[OFF_P3D] & (1u << 2)) == 0;
}

/*
 * Raise KINT0 if the buttons no longer match what the comparator was armed
 * with. grifo enables it with EK0 and re-arms from its handler by writing
 * the current state back to SCPK0, so this stays quiet until something
 * actually changes.
 */
static bool kint0_matches(const struct port *p)
{
	uint8_t mask = p->reg[OFF_SMPK0];
	return (p->reg[OFF_P6D] & mask) == (p->reg[OFF_SCPK0] & mask);
}

static void raise_input(struct port *p, struct c33 *cpu, unsigned vector)
{
	if (p->itc)
		itc_set_flag((struct itc *)p->itc, vector);
	c33_raise_irq(cpu, vector, p->itc ? itc_priority(p->itc, vector) : 7);
}

void port_button(struct port *p, struct c33 *cpu, unsigned n, bool pressed)
{
	if (n > 2)
		return;
	bool selected = (p->reg[OFF_KSEL] & 0x07) == 0x04;
	bool was_match = kint0_matches(p);
	if (pressed)
		p->reg[OFF_P6D] |= (uint8_t)(1u << n);
	else
		p->reg[OFF_P6D] &= (uint8_t)~(1u << n);
	p->button_events++;
	if (selected && was_match && !kint0_matches(p))
		raise_input(p, cpu, VECTOR_KEY_INPUT_0);
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
	bool was_high = (p->reg[OFF_P0D] & (1u << POWER_BIT)) != 0;
	if (pressed)
		p->reg[OFF_P0D] &= (uint8_t)~(1u << POWER_BIT);   /* active low */
	else
		p->reg[OFF_P0D] |= (uint8_t)(1u << POWER_BIT);
	p->button_events++;

	bool is_high = !pressed;
	bool rising = !was_high && is_high;
	bool falling = was_high && !is_high;
	bool polarity_high = (p->reg[OFF_PPOL] & (1u << 3)) != 0;
	bool edge = (p->reg[OFF_PEL] & (1u << 3)) != 0;
	bool selected = (p->reg[OFF_PSEL] & 0xc0) == 0;
	bool triggered = edge ? (polarity_high ? rising : falling)
			      : (is_high == polarity_high);

	if (selected && triggered)
		raise_input(p, cpu, VECTOR_PORT_INPUT_3);
}

/* Reset state without re-registering the device. */
void port_reset(struct port *p)
{
	const struct itc *keep = p->itc;
	struct c33 *cpu = p->cpu;

	memset(p, 0, sizeof *p);
	p->itc = keep;

	/* A reset restarts the machine; it does not rewire the board. */

	p->cpu = cpu;
	p->reg[OFF_P5D] = (1u << CS_SDCARD_BIT) | (1u << CS_EEPROM_BIT);
	/* Pull-ups per REG_MISC_PUP6. In particular P64 is high while no CTP
	 * receive sequence is in progress; leaving it low makes Suspend()
	 * return immediately and turns the event wait into a busy loop. */
	p->reg[OFF_P6D] = (1u << 5) | (1u << 4) | (1u << 3);
	p->reg[OFF_P0D] = (1u << POWER_BIT);   /* power switch idles high */

	/*
	 * SDA10 is already on P53. The emulator starts where the boot
	 * loader finished -- an image loaded straight in finds its SDRAM
	 * working without having initialised the controller, and the pin
	 * that addresses it should be no different.
	 */

	p->reg[OFF_P5CFP03] = P53_FUNC_SDA10;
	/* Port input interrupts reset to rising-edge selection. */
	p->reg[OFF_PPOL] = p->reg[OFF_PEL] = 0xff;
	p->reg[OFF_PPOL + 4] = p->reg[OFF_PEL + 4] = 0xff;
}

/* Port A holds the board revision in its low nibble, inverted: the boot
   code computes 8 ^ (PA & 0x0f). Revision 8 (the default here) and 6 are
   the 16 MB boards; the early 32 MB boards report anything else, so
   WREMU_BOARD_REV=7 emulates one of those. */
static uint8_t porta_value;

static bool porta_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
		       bool is_write)
{
	(void)ctx; (void)size;
	if (is_write)
		return true;
	*val = (off == PORTA_BASE + 1) ? porta_value : 0;
	return true;
}

void port_watch_sdram(struct port *p, struct c33 *cpu)
{
	p->cpu = cpu;
}

void port_attach(struct mem *m, struct port *p, const struct itc *itc)
{
	const char *rev = getenv("WREMU_BOARD_REV");

	p->itc = itc;
	port_reset(p);
	porta_value = (uint8_t)(0x08u ^ (rev ? strtoul(rev, NULL, 0) & 0x0fu : 0x08u));
	mem_add_mmio(m, "ports", PORT_BASE, PORT_LEN, port_mmio, p);
	mem_add_mmio(m, "porta", PORTA_BASE, PORTA_LEN, porta_mmio, p);
}
