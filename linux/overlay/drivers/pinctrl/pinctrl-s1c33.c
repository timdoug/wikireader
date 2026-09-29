// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pin controller and GPIO for the Epson S1C33E07 port block.
 *
 * Every pin of ports 0 to 9 has up to four functions, chosen by two bits in
 * the port function select registers; one of them is usually the pin as an
 * I/O port, and which one differs from pin to pin (P50's function 0 is the
 * bus chip select #CE4, so its port is function 1).  Each pin is a group of
 * its own, and each function is named after the manual's signal, so a board
 * maps "P65" to "sdi".  Requesting a line as a GPIO selects its port
 * function.  The generic "output-high/low" configuration drives a pin as a
 * port output, which is how a peripheral pin is parked at a level.
 *
 * Ports 0 to 6 are GPIOs 0 to 55, pin numbers the same.
 *
 * Two of the block's interrupt sources are wired to lines here.  Key input 0
 * compares P60..P62 with a stored pattern and interrupts on a mismatch, so
 * the handler reports every line that changed and stores the new state.
 * Port input 3 watches P03 for one edge, chosen by its polarity bit; both
 * edges are had by pointing it at the opposite of the level after each one.
 * Each source has its own ITC vector, chained here.
 */
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#include "pinctrl-utils.h"

#define S1C33_GPIO_PORTS	7
#define S1C33_GPIO_PER_PORT	8
#define S1C33_GPIO_DATA(port)	((port) * 2)
#define S1C33_GPIO_DIRECTION(port) (S1C33_GPIO_DATA(port) + 1)

#define S1C33_GPIO_P03		(0 * S1C33_GPIO_PER_PORT + 3)
#define S1C33_GPIO_P60		(6 * S1C33_GPIO_PER_PORT + 0)
#define S1C33_GPIO_KEY_LINES	3	/* P60..P62 */
#define S1C33_GPIO_KEY_MASK	GENMASK(S1C33_GPIO_KEY_LINES - 1, 0)

/* The interrupt registers, the second memory resource. */
#define S1C33_SPT03		0x00	/* port inputs 0-3: pin select */
#define S1C33_SPT3_MASK		0xc0	/* port input 3: 0 selects P03 */
#define S1C33_SPP07		0x02	/* polarity: set is high/rising */
#define S1C33_SEPT07		0x03	/* set is edge, clear is level */
#define S1C33_PORT3		BIT(3)
#define S1C33_SPPK01		0x10	/* key inputs: port group select */
#define S1C33_SPPK0_MASK	0x07
#define S1C33_SPPK0_P60		0x04
#define S1C33_SCPK0		0x12	/* key input 0: comparison pattern */
#define S1C33_SMPK0		0x14	/* key input 0: bits compared */

/*
 * The pin table, straight from the manual's list of pin function select
 * bits.  PORT marks a pin's I/O port function; "gpio" selects it.
 */
#define S1C33_FUNCTIONS(F) \
	F(a11) \
	F(a18) \
	F(a19) \
	F(a20) \
	F(a21) \
	F(a22) \
	F(a23) \
	F(a24) \
	F(adtrg) \
	F(ain0) \
	F(ain1) \
	F(ain2) \
	F(ain3) \
	F(ain4) \
	F(bclk) \
	F(card0) \
	F(card1) \
	F(card2) \
	F(card3) \
	F(card4) \
	F(card5) \
	F(ce4) \
	F(ce5) \
	F(ce6) \
	F(ce7) \
	F(ce8) \
	F(ce9) \
	F(ce10) \
	F(ce11) \
	F(cmu_clk) \
	F(dclk) \
	F(dcsio0) \
	F(dcsio1) \
	F(dmaack0) \
	F(dmaack1) \
	F(dmaack2) \
	F(dmaack3) \
	F(dmaend0) \
	F(dmaend1) \
	F(dmaend2) \
	F(dmaend3) \
	F(dmareq0) \
	F(dmareq1) \
	F(dmareq2) \
	F(dmareq3) \
	F(dqmh) \
	F(dqml) \
	F(dsio) \
	F(dst2) \
	F(excl0) \
	F(excl1) \
	F(excl2) \
	F(excl3) \
	F(excl4) \
	F(excl5) \
	F(fpdat0) \
	F(fpdat1) \
	F(fpdat2) \
	F(fpdat3) \
	F(fpdat4) \
	F(fpdat5) \
	F(fpdat6) \
	F(fpdat7) \
	F(fpdat8) \
	F(fpdat9) \
	F(fpdat10) \
	F(fpdat11) \
	F(fpdrdy) \
	F(fpframe) \
	F(fpline) \
	F(fpshift) \
	F(i2s_mclk) \
	F(i2s_sck) \
	F(i2s_sdo) \
	F(i2s_ws) \
	F(sclk0) \
	F(sclk1) \
	F(sclk2) \
	F(sda10) \
	F(sdcas) \
	F(sdcke) \
	F(sdclk) \
	F(sdcs) \
	F(sdi) \
	F(sdo) \
	F(sdras) \
	F(sdwe) \
	F(sin0) \
	F(sin1) \
	F(sin2) \
	F(sout0) \
	F(sout1) \
	F(sout2) \
	F(spi_clk) \
	F(srdy0) \
	F(srdy1) \
	F(srdy2) \
	F(tft_ctl0) \
	F(tft_ctl1) \
	F(tft_ctl2) \
	F(tft_ctl3) \
	F(tm0) \
	F(tm1) \
	F(tm2) \
	F(tm3) \
	F(tm4) \
	F(tm5) \
	F(wait) \
	F(wdt_clk) \
	F(wdt_nmi)

enum s1c33_function_id {
	S1C33_FN_GPIO,
#define S1C33_FN_ENUM(name) S1C33_FN_##name,
	S1C33_FUNCTIONS(S1C33_FN_ENUM)
	S1C33_FN_COUNT,
	S1C33_FN_NONE = S1C33_FN_COUNT,
	S1C33_FN_PORT,
};

static const char * const s1c33_function_names[S1C33_FN_COUNT] = {
	[S1C33_FN_GPIO] = "gpio",
#define S1C33_FN_NAME(name) [S1C33_FN_##name] = #name,
	S1C33_FUNCTIONS(S1C33_FN_NAME)
};

struct s1c33_pin {
	unsigned int number;
	const char *name;
	u8 function[4];
};

#define S1C33_PIN(port, bit, f0, f1, f2, f3)				\
	{ (port) * 8 + (bit), "P" #port #bit,				\
	  { S1C33_FN_##f0, S1C33_FN_##f1, S1C33_FN_##f2, S1C33_FN_##f3 } }

static const struct s1c33_pin s1c33_pins[] = {
	S1C33_PIN(0, 0, PORT, sin0, dmaack2, NONE),
	S1C33_PIN(0, 1, PORT, sout0, dmaack3, NONE),
	S1C33_PIN(0, 2, PORT, sclk0, dmaend2, NONE),
	S1C33_PIN(0, 3, PORT, srdy0, dmaend3, NONE),
	S1C33_PIN(0, 4, PORT, sin1, i2s_sdo, NONE),
	S1C33_PIN(0, 5, PORT, sout1, i2s_ws, NONE),
	S1C33_PIN(0, 6, PORT, sclk1, i2s_sck, NONE),
	S1C33_PIN(0, 7, PORT, srdy1, i2s_mclk, NONE),
	S1C33_PIN(1, 0, PORT, tm0, sin0, dmaend0),
	S1C33_PIN(1, 1, PORT, tm1, sout0, dmaend1),
	S1C33_PIN(1, 2, PORT, tm2, sclk0, dmaack0),
	S1C33_PIN(1, 3, PORT, tm3, srdy0, dmaack1),
	S1C33_PIN(1, 4, PORT, tm4, sin1, NONE),
	S1C33_PIN(1, 5, PORT, tm5, sout1, tft_ctl0),
	S1C33_PIN(1, 6, PORT, dcsio0, sclk1, tft_ctl3),
	S1C33_PIN(1, 7, PORT, dcsio1, srdy1, tft_ctl2),
	S1C33_PIN(2, 0, PORT, sdcke, NONE, NONE),
	S1C33_PIN(2, 1, PORT, sdclk, NONE, NONE),
	S1C33_PIN(2, 2, PORT, sdcs, NONE, NONE),
	S1C33_PIN(2, 3, PORT, sdras, tft_ctl1, NONE),
	S1C33_PIN(2, 4, PORT, sdcas, NONE, NONE),
	S1C33_PIN(2, 5, PORT, sdwe, NONE, NONE),
	S1C33_PIN(2, 6, PORT, dqml, NONE, NONE),
	S1C33_PIN(2, 7, PORT, dqmh, NONE, NONE),
	S1C33_PIN(3, 0, PORT, card2, dmareq0, NONE),
	S1C33_PIN(3, 1, PORT, card3, dmareq1, NONE),
	S1C33_PIN(3, 2, PORT, card4, dmareq2, NONE),
	S1C33_PIN(3, 3, PORT, card5, dmareq3, NONE),
	S1C33_PIN(3, 4, dsio, PORT, NONE, NONE),
	S1C33_PIN(3, 5, dclk, PORT, NONE, NONE),
	S1C33_PIN(3, 6, dst2, PORT, NONE, NONE),
	S1C33_PIN(4, 0, a24, PORT, sdcas, excl4),
	S1C33_PIN(4, 1, a23, PORT, sdras, excl3),
	S1C33_PIN(4, 2, a22, PORT, fpdat8, NONE),
	S1C33_PIN(4, 3, a21, PORT, fpdat9, NONE),
	S1C33_PIN(4, 4, a20, PORT, fpdat10, NONE),
	S1C33_PIN(4, 5, a19, PORT, fpdat11, NONE),
	S1C33_PIN(4, 6, a18, PORT, tft_ctl2, NONE),
	S1C33_PIN(4, 7, a11, PORT, NONE, NONE),
	S1C33_PIN(5, 0, ce4, PORT, card0, NONE),
	S1C33_PIN(5, 1, ce5, PORT, card1, NONE),
	S1C33_PIN(5, 2, PORT, bclk, ce6, cmu_clk),
	S1C33_PIN(5, 3, ce7, PORT, sda10, NONE),
	S1C33_PIN(5, 4, ce8, PORT, card1, NONE),
	S1C33_PIN(5, 5, ce9, PORT, card0, NONE),
	S1C33_PIN(5, 6, ce11, PORT, NONE, NONE),
	S1C33_PIN(5, 7, ce10, PORT, NONE, NONE),
	S1C33_PIN(6, 0, PORT, sin2, dcsio0, excl0),
	S1C33_PIN(6, 1, PORT, sout2, dcsio1, excl1),
	S1C33_PIN(6, 2, PORT, sclk2, adtrg, cmu_clk),
	S1C33_PIN(6, 3, PORT, srdy2, wdt_clk, wdt_nmi),
	S1C33_PIN(6, 4, PORT, wait, excl2, NONE),
	S1C33_PIN(6, 5, PORT, sdi, fpdat8, NONE),
	S1C33_PIN(6, 6, PORT, sdo, fpdat9, NONE),
	S1C33_PIN(6, 7, PORT, spi_clk, fpdat10, NONE),
	S1C33_PIN(7, 0, PORT, ain0, NONE, NONE),
	S1C33_PIN(7, 1, PORT, ain1, NONE, NONE),
	S1C33_PIN(7, 2, PORT, ain2, NONE, NONE),
	S1C33_PIN(7, 3, PORT, ain3, NONE, NONE),
	S1C33_PIN(7, 4, PORT, ain4, excl5, NONE),
	S1C33_PIN(8, 0, PORT, fpframe, NONE, NONE),
	S1C33_PIN(8, 1, PORT, fpline, NONE, NONE),
	S1C33_PIN(8, 2, PORT, fpshift, NONE, NONE),
	S1C33_PIN(8, 3, PORT, fpdrdy, tft_ctl1, bclk),
	S1C33_PIN(8, 4, PORT, dcsio0, fpdat11, NONE),
	S1C33_PIN(8, 5, PORT, dcsio1, NONE, NONE),
	S1C33_PIN(9, 0, PORT, fpdat0, NONE, NONE),
	S1C33_PIN(9, 1, PORT, fpdat1, NONE, NONE),
	S1C33_PIN(9, 2, PORT, fpdat2, NONE, NONE),
	S1C33_PIN(9, 3, PORT, fpdat3, NONE, NONE),
	S1C33_PIN(9, 4, PORT, fpdat4, NONE, NONE),
	S1C33_PIN(9, 5, PORT, fpdat5, NONE, NONE),
	S1C33_PIN(9, 6, PORT, fpdat6, NONE, NONE),
	S1C33_PIN(9, 7, PORT, fpdat7, NONE, NONE),
};

/* Two bits a pin, four pins a register, two registers a port. */
#define S1C33_FUNCTION_SELECT(number) \
	(((number) / 8) * 2 + ((number) % 8) / 4)
#define S1C33_FUNCTION_SHIFT(number)	(((number) % 4) * 2)

struct s1c33_function {
	const char **groups;
	unsigned int ngroups;
};

struct s1c33_gpio {
	void __iomem *base;
	void __iomem *irq_base;
	void __iomem *function_base;
	/* Protects data, direction and function select register RMW. */
	spinlock_t lock;
	struct gpio_chip chip;
	struct pinctrl_dev *pctl;
	struct pinctrl_desc desc;
	struct pinctrl_pin_desc pin_descs[ARRAY_SIZE(s1c33_pins)];
	struct s1c33_function functions[S1C33_FN_COUNT];
	int key_irq;
	int port3_irq;
	/* Under lock: key input 0's last state and unmasked lines, P03's. */
	u8 key_state;
	u8 key_enabled;
	bool port3_enabled;
	u8 port3_level;
	/* Edges wanted, per line: bit 0 rising, bit 1 falling. */
	u8 key_edges[S1C33_GPIO_KEY_LINES];
	u8 port3_edges;
};

static void __iomem *s1c33_gpio_register(struct s1c33_gpio *gpio,
					 unsigned int offset, bool direction)
{
	unsigned int port = offset / S1C33_GPIO_PER_PORT;

	return gpio->base + (direction ? S1C33_GPIO_DIRECTION(port) :
					  S1C33_GPIO_DATA(port));
}

static u8 s1c33_gpio_mask(unsigned int offset)
{
	return BIT(offset % S1C33_GPIO_PER_PORT);
}

static int s1c33_gpio_get_direction(struct gpio_chip *chip,
				    unsigned int offset)
{
	struct s1c33_gpio *gpio = gpiochip_get_data(chip);

	return readb(s1c33_gpio_register(gpio, offset, true)) &
		s1c33_gpio_mask(offset) ? GPIO_LINE_DIRECTION_OUT :
		GPIO_LINE_DIRECTION_IN;
}

static int s1c33_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct s1c33_gpio *gpio = gpiochip_get_data(chip);

	return !!(readb(s1c33_gpio_register(gpio, offset, false)) &
		  s1c33_gpio_mask(offset));
}

static void s1c33_gpio_set_locked(struct s1c33_gpio *gpio,
				  unsigned int offset, int value)
{
	void __iomem *reg = s1c33_gpio_register(gpio, offset, false);
	u8 mask = s1c33_gpio_mask(offset);
	u8 data = readb(reg);

	writeb(value ? data | mask : data & ~mask, reg);
}

static int s1c33_gpio_set(struct gpio_chip *chip, unsigned int offset,
			  int value)
{
	struct s1c33_gpio *gpio = gpiochip_get_data(chip);
	unsigned long flags;

	spin_lock_irqsave(&gpio->lock, flags);
	s1c33_gpio_set_locked(gpio, offset, value);
	spin_unlock_irqrestore(&gpio->lock, flags);
	return 0;
}

static int s1c33_gpio_direction_input(struct gpio_chip *chip,
				      unsigned int offset)
{
	struct s1c33_gpio *gpio = gpiochip_get_data(chip);
	void __iomem *reg = s1c33_gpio_register(gpio, offset, true);
	unsigned long flags;

	spin_lock_irqsave(&gpio->lock, flags);
	writeb(readb(reg) & ~s1c33_gpio_mask(offset), reg);
	spin_unlock_irqrestore(&gpio->lock, flags);
	return 0;
}

static int s1c33_gpio_direction_output(struct gpio_chip *chip,
				       unsigned int offset, int value)
{
	struct s1c33_gpio *gpio = gpiochip_get_data(chip);
	void __iomem *reg = s1c33_gpio_register(gpio, offset, true);
	unsigned long flags;

	spin_lock_irqsave(&gpio->lock, flags);
	s1c33_gpio_set_locked(gpio, offset, value);
	writeb(readb(reg) | s1c33_gpio_mask(offset), reg);
	spin_unlock_irqrestore(&gpio->lock, flags);
	return 0;
}

/****************************************************************************/
/* Pin control */

/*
 * A peripheral's pins at once: a device tree names the group and a function
 * of the same name ("groups", "function"), and each pin takes its signal.
 * Every pin is also a group of its own, named for the pin, with the
 * manual's signals as its functions.
 */
struct s1c33_group {
	const char *name;
	const unsigned int *pins;
	const u8 *signals;
	unsigned int npins;
};

#define S1C33_P(port, bit)	((port) * 8 + (bit))
#define S1C33_GROUP(group)						\
	{ #group, s1c33_##group##_pins, s1c33_##group##_signals,	\
	  ARRAY_SIZE(s1c33_##group##_pins) }

static const unsigned int s1c33_uart0_pins[] = { S1C33_P(0, 0), S1C33_P(0, 1) };
static const u8 s1c33_uart0_signals[] = { S1C33_FN_sin0, S1C33_FN_sout0 };
static const unsigned int s1c33_spi_pins[] = {
	S1C33_P(6, 5), S1C33_P(6, 6), S1C33_P(6, 7),
};
static const u8 s1c33_spi_signals[] = {
	S1C33_FN_sdi, S1C33_FN_sdo, S1C33_FN_spi_clk,
};
static const unsigned int s1c33_adc_pins[] = {
	S1C33_P(7, 0), S1C33_P(7, 1), S1C33_P(7, 2),
};
static const u8 s1c33_adc_signals[] = {
	S1C33_FN_ain0, S1C33_FN_ain1, S1C33_FN_ain2,
};
/* The panel's 4-bit interface: frame, line, shift, ready, data 4..7. */
static const unsigned int s1c33_lcd_pins[] = {
	S1C33_P(8, 0), S1C33_P(8, 1), S1C33_P(8, 2), S1C33_P(8, 3),
	S1C33_P(9, 4), S1C33_P(9, 5), S1C33_P(9, 6), S1C33_P(9, 7),
};
static const u8 s1c33_lcd_signals[] = {
	S1C33_FN_fpframe, S1C33_FN_fpline, S1C33_FN_fpshift, S1C33_FN_fpdrdy,
	S1C33_FN_fpdat4, S1C33_FN_fpdat5, S1C33_FN_fpdat6, S1C33_FN_fpdat7,
};

static const struct s1c33_group s1c33_groups[] = {
	S1C33_GROUP(uart0),
	S1C33_GROUP(spi),
	S1C33_GROUP(adc),
	S1C33_GROUP(lcd),
};

/* Group numbers: the pins' own first, then the named groups; a named
 * group's function is numbered after the signals. */
#define S1C33_NPINS		ARRAY_SIZE(s1c33_pins)
#define S1C33_FN_GROUP(i)	(S1C33_FN_PORT + 1 + (i))

/* The select value for a function on a pin; "gpio" is its port. */
static int s1c33_select_value(const struct s1c33_pin *pin, unsigned int id)
{
	unsigned int f;

	if (id == S1C33_FN_GPIO)
		id = S1C33_FN_PORT;
	for (f = 0; f < ARRAY_SIZE(pin->function); f++)
		if (pin->function[f] == id)
			return f;
	return -EINVAL;
}

static const struct s1c33_pin *s1c33_find_pin(unsigned int number)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(s1c33_pins); i++)
		if (s1c33_pins[i].number == number)
			return &s1c33_pins[i];
	return NULL;
}

static void s1c33_select(struct s1c33_gpio *gpio, const struct s1c33_pin *pin,
			 unsigned int f)
{
	void __iomem *reg = gpio->function_base +
		S1C33_FUNCTION_SELECT(pin->number);
	unsigned int shift = S1C33_FUNCTION_SHIFT(pin->number);
	unsigned long flags;

	spin_lock_irqsave(&gpio->lock, flags);
	writeb((readb(reg) & ~(3 << shift)) | f << shift, reg);
	spin_unlock_irqrestore(&gpio->lock, flags);
}

static int s1c33_get_groups_count(struct pinctrl_dev *pctl)
{
	return S1C33_NPINS + ARRAY_SIZE(s1c33_groups);
}

static const char *s1c33_get_group_name(struct pinctrl_dev *pctl,
					unsigned int group)
{
	if (group >= S1C33_NPINS)
		return s1c33_groups[group - S1C33_NPINS].name;
	return s1c33_pins[group].name;
}

static int s1c33_get_group_pins(struct pinctrl_dev *pctl, unsigned int group,
				const unsigned int **pins,
				unsigned int *npins)
{
	if (group >= S1C33_NPINS) {
		*pins = s1c33_groups[group - S1C33_NPINS].pins;
		*npins = s1c33_groups[group - S1C33_NPINS].npins;
		return 0;
	}
	*pins = &s1c33_pins[group].number;
	*npins = 1;
	return 0;
}

/*
 * A device-tree state names a group or pins, a function, and any generic
 * configuration ("groups" or "pins", "function", "output-low").
 */
static const struct pinctrl_ops s1c33_pinctrl_ops = {
	.get_groups_count = s1c33_get_groups_count,
	.get_group_name = s1c33_get_group_name,
	.get_group_pins = s1c33_get_group_pins,
	.dt_node_to_map = pinconf_generic_dt_node_to_map_all,
	.dt_free_map = pinctrl_utils_free_map,
};

/*
 * The core numbers functions from zero: the signals, then one for each
 * named group, which S1C33_FN_GROUP() places past the pin table's markers.
 */
static unsigned int s1c33_function_id(unsigned int function)
{
	return function < S1C33_FN_COUNT ? function :
		S1C33_FN_GROUP(function - S1C33_FN_COUNT);
}

static int s1c33_get_functions_count(struct pinctrl_dev *pctl)
{
	return S1C33_FN_COUNT + ARRAY_SIZE(s1c33_groups);
}

static const char *s1c33_get_function_name(struct pinctrl_dev *pctl,
					   unsigned int function)
{
	if (function >= S1C33_FN_COUNT)
		return s1c33_groups[function - S1C33_FN_COUNT].name;
	return s1c33_function_names[function];
}

static int s1c33_get_function_groups(struct pinctrl_dev *pctl,
				     unsigned int function,
				     const char * const **groups,
				     unsigned int *ngroups)
{
	struct s1c33_gpio *gpio = pinctrl_dev_get_drvdata(pctl);

	if (function >= S1C33_FN_COUNT) {
		*groups = &s1c33_groups[function - S1C33_FN_COUNT].name;
		*ngroups = 1;
		return 0;
	}
	*groups = gpio->functions[function].groups;
	*ngroups = gpio->functions[function].ngroups;
	return 0;
}

static bool s1c33_function_is_gpio(struct pinctrl_dev *pctl,
				   unsigned int function)
{
	return function == S1C33_FN_GPIO;
}

static int s1c33_set_mux(struct pinctrl_dev *pctl, unsigned int function,
			 unsigned int group)
{
	struct s1c33_gpio *gpio = pinctrl_dev_get_drvdata(pctl);
	const struct s1c33_group *named;
	const struct s1c33_pin *pin;
	unsigned int i;
	int value;

	if (group < S1C33_NPINS) {
		pin = &s1c33_pins[group];
		value = s1c33_select_value(pin, s1c33_function_id(function));
		if (value < 0)
			return value;
		s1c33_select(gpio, pin, value);
		return 0;
	}
	named = &s1c33_groups[group - S1C33_NPINS];
	for (i = 0; i < named->npins; i++) {
		pin = s1c33_find_pin(named->pins[i]);
		value = pin ? s1c33_select_value(pin, named->signals[i]) :
			-EINVAL;
		if (value < 0)
			return value;
	}
	for (i = 0; i < named->npins; i++) {
		pin = s1c33_find_pin(named->pins[i]);
		s1c33_select(gpio, pin, s1c33_select_value(pin,
							  named->signals[i]));
	}
	return 0;
}

static int s1c33_gpio_request_enable(struct pinctrl_dev *pctl,
				     struct pinctrl_gpio_range *range,
				     unsigned int number)
{
	struct s1c33_gpio *gpio = pinctrl_dev_get_drvdata(pctl);
	const struct s1c33_pin *pin = s1c33_find_pin(number);
	int value = pin ? s1c33_select_value(pin, S1C33_FN_GPIO) : -EINVAL;

	if (value < 0)
		return value;
	s1c33_select(gpio, pin, value);
	return 0;
}

static const struct pinmux_ops s1c33_pinmux_ops = {
	.get_functions_count = s1c33_get_functions_count,
	.get_function_name = s1c33_get_function_name,
	.get_function_groups = s1c33_get_function_groups,
	.function_is_gpio = s1c33_function_is_gpio,
	.set_mux = s1c33_set_mux,
	.gpio_request_enable = s1c33_gpio_request_enable,
};

static int s1c33_pin_config_get(struct pinctrl_dev *pctl, unsigned int pin,
				unsigned long *config)
{
	struct s1c33_gpio *gpio = pinctrl_dev_get_drvdata(pctl);

	if (pin >= gpio->chip.ngpio)
		return -ENOTSUPP;
	switch (pinconf_to_config_param(*config)) {
	case PIN_CONFIG_LEVEL:
		if (gpio->chip.get_direction(&gpio->chip, pin) !=
		    GPIO_LINE_DIRECTION_OUT)
			return -EINVAL;
		*config = pinconf_to_config_packed(PIN_CONFIG_LEVEL,
				gpio->chip.get(&gpio->chip, pin));
		return 0;
	default:
		return -ENOTSUPP;
	}
}

static int s1c33_pin_config_set(struct pinctrl_dev *pctl, unsigned int pin,
				unsigned long *configs, unsigned int nconfigs)
{
	struct s1c33_gpio *gpio = pinctrl_dev_get_drvdata(pctl);
	unsigned int i;

	if (pin >= gpio->chip.ngpio)
		return -ENOTSUPP;
	for (i = 0; i < nconfigs; i++) {
		switch (pinconf_to_config_param(configs[i])) {
		case PIN_CONFIG_LEVEL:
			s1c33_gpio_direction_output(&gpio->chip, pin,
				pinconf_to_config_argument(configs[i]));
			break;
		default:
			return -ENOTSUPP;
		}
	}
	return 0;
}

static const struct pinconf_ops s1c33_pinconf_ops = {
	.is_generic = true,
	.pin_config_get = s1c33_pin_config_get,
	.pin_config_set = s1c33_pin_config_set,
};

/* Each function's pins, the groups, from one count and one fill pass. */
static int s1c33_build_functions(struct device *dev, struct s1c33_gpio *gpio)
{
	unsigned int i, f, id;

	for (i = 0; i < ARRAY_SIZE(s1c33_pins); i++)
		for (f = 0; f < 4; f++) {
			id = s1c33_pins[i].function[f];
			if (id == S1C33_FN_PORT)
				id = S1C33_FN_GPIO;
			if (id < S1C33_FN_COUNT)
				gpio->functions[id].ngroups++;
		}
	for (id = 0; id < S1C33_FN_COUNT; id++) {
		gpio->functions[id].groups =
			devm_kcalloc(dev, gpio->functions[id].ngroups,
				     sizeof(*gpio->functions[id].groups),
				     GFP_KERNEL);
		if (!gpio->functions[id].groups)
			return -ENOMEM;
		gpio->functions[id].ngroups = 0;
	}
	for (i = 0; i < ARRAY_SIZE(s1c33_pins); i++)
		for (f = 0; f < 4; f++) {
			id = s1c33_pins[i].function[f];
			if (id == S1C33_FN_PORT)
				id = S1C33_FN_GPIO;
			if (id < S1C33_FN_COUNT)
				gpio->functions[id].groups
					[gpio->functions[id].ngroups++] =
					s1c33_pins[i].name;
		}
	return 0;
}

static int s1c33_pinctrl_init(struct platform_device *pdev,
			      struct s1c33_gpio *gpio)
{
	struct device *dev = &pdev->dev;
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(s1c33_pins); i++) {
		gpio->pin_descs[i].number = s1c33_pins[i].number;
		gpio->pin_descs[i].name = s1c33_pins[i].name;
	}
	ret = s1c33_build_functions(dev, gpio);
	if (ret)
		return ret;

	gpio->desc.name = dev_name(dev);
	gpio->desc.pins = gpio->pin_descs;
	gpio->desc.npins = ARRAY_SIZE(s1c33_pins);
	gpio->desc.pctlops = &s1c33_pinctrl_ops;
	gpio->desc.pmxops = &s1c33_pinmux_ops;
	gpio->desc.confops = &s1c33_pinconf_ops;
	gpio->desc.owner = THIS_MODULE;
	ret = devm_pinctrl_register_and_init(dev, &gpio->desc, gpio,
					     &gpio->pctl);
	if (ret)
		return ret;
	return pinctrl_enable(gpio->pctl);
}

/****************************************************************************/
/* Interrupts */

#define S1C33_EDGE_RISING	BIT(0)
#define S1C33_EDGE_FALLING	BIT(1)

static u8 s1c33_gpio_key_bits(struct s1c33_gpio *gpio)
{
	return readb(gpio->base + S1C33_GPIO_DATA(6)) & S1C33_GPIO_KEY_MASK;
}

static u8 s1c33_gpio_port3_bit(struct s1c33_gpio *gpio)
{
	return !!(readb(gpio->base + S1C33_GPIO_DATA(0)) & S1C33_PORT3);
}

static void s1c33_gpio_update8(void __iomem *reg, u8 clear, u8 set)
{
	writeb((readb(reg) & ~clear) | set, reg);
}

/* Watch P03 for the next edge wanted from the level it has now. */
static void s1c33_gpio_arm_port3(struct s1c33_gpio *gpio)
{
	bool rising;

	gpio->port3_level = s1c33_gpio_port3_bit(gpio);
	if (gpio->port3_edges == S1C33_EDGE_RISING)
		rising = true;
	else if (gpio->port3_edges == S1C33_EDGE_FALLING)
		rising = false;
	else
		rising = !gpio->port3_level;
	s1c33_gpio_update8(gpio->irq_base + S1C33_SPP07, S1C33_PORT3,
			   rising ? S1C33_PORT3 : 0);
}

static void s1c33_gpio_key_interrupt(struct s1c33_gpio *gpio)
{
	unsigned long rose = 0, fell = 0;
	unsigned int i;
	u8 bits;

	spin_lock(&gpio->lock);
	/* Store what was read as the new pattern, and go round again if a
	 * line moved in between: the mismatch would never interrupt. */
	do {
		bits = s1c33_gpio_key_bits(gpio);
		rose |= bits & ~gpio->key_state;
		fell |= ~bits & gpio->key_state & S1C33_GPIO_KEY_MASK;
		gpio->key_state = bits;
		writeb(bits, gpio->irq_base + S1C33_SCPK0);
	} while (s1c33_gpio_key_bits(gpio) != bits);
	rose &= gpio->key_enabled;
	fell &= gpio->key_enabled;
	spin_unlock(&gpio->lock);

	for (i = 0; i < S1C33_GPIO_KEY_LINES; i++)
		if (((rose & BIT(i)) &&
		     (gpio->key_edges[i] & S1C33_EDGE_RISING)) ||
		    ((fell & BIT(i)) &&
		     (gpio->key_edges[i] & S1C33_EDGE_FALLING)))
			generic_handle_domain_irq(gpio->chip.irq.domain,
						  S1C33_GPIO_P60 + i);
}

static void s1c33_gpio_port3_interrupt(struct s1c33_gpio *gpio)
{
	bool report;
	u8 before;

	spin_lock(&gpio->lock);
	before = gpio->port3_level;
	s1c33_gpio_arm_port3(gpio);
	/* Re-armed for the next edge; one that came first is already lost
	 * to the hardware, so check the level once more. */
	if (s1c33_gpio_port3_bit(gpio) != gpio->port3_level)
		s1c33_gpio_arm_port3(gpio);
	report = gpio->port3_enabled &&
		(gpio->port3_level != before ||
		 gpio->port3_edges != (S1C33_EDGE_RISING | S1C33_EDGE_FALLING));
	spin_unlock(&gpio->lock);

	if (report)
		generic_handle_domain_irq(gpio->chip.irq.domain,
					  S1C33_GPIO_P03);
}

static void s1c33_gpio_irq_handler(struct irq_desc *desc)
{
	struct gpio_chip *chip = irq_desc_get_handler_data(desc);
	struct s1c33_gpio *gpio = gpiochip_get_data(chip);
	struct irq_chip *parent = irq_desc_get_chip(desc);

	chained_irq_enter(parent, desc);
	if (irq_desc_get_irq(desc) == gpio->key_irq)
		s1c33_gpio_key_interrupt(gpio);
	else
		s1c33_gpio_port3_interrupt(gpio);
	chained_irq_exit(parent, desc);
}

static void s1c33_gpio_irq_mask(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct s1c33_gpio *gpio = gpiochip_get_data(chip);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);
	unsigned long flags;

	spin_lock_irqsave(&gpio->lock, flags);
	if (hwirq == S1C33_GPIO_P03) {
		gpio->port3_enabled = false;
	} else {
		gpio->key_enabled &= ~BIT(hwirq - S1C33_GPIO_P60);
		writeb(gpio->key_enabled, gpio->irq_base + S1C33_SMPK0);
	}
	spin_unlock_irqrestore(&gpio->lock, flags);
	gpiochip_disable_irq(chip, hwirq);
}

static void s1c33_gpio_irq_unmask(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct s1c33_gpio *gpio = gpiochip_get_data(chip);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);
	unsigned long flags;

	gpiochip_enable_irq(chip, hwirq);
	spin_lock_irqsave(&gpio->lock, flags);
	if (hwirq == S1C33_GPIO_P03) {
		gpio->port3_enabled = true;
		s1c33_gpio_arm_port3(gpio);
	} else {
		/* Compare from the level the line has now. */
		gpio->key_state = s1c33_gpio_key_bits(gpio);
		gpio->key_enabled |= BIT(hwirq - S1C33_GPIO_P60);
		writeb(gpio->key_state, gpio->irq_base + S1C33_SCPK0);
		writeb(gpio->key_enabled, gpio->irq_base + S1C33_SMPK0);
	}
	spin_unlock_irqrestore(&gpio->lock, flags);
}

static int s1c33_gpio_irq_set_type(struct irq_data *data, unsigned int type)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct s1c33_gpio *gpio = gpiochip_get_data(chip);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);
	unsigned long flags;
	u8 edges = 0;

	if (type & ~IRQ_TYPE_EDGE_BOTH || !type)
		return -EINVAL;
	if (type & IRQ_TYPE_EDGE_RISING)
		edges |= S1C33_EDGE_RISING;
	if (type & IRQ_TYPE_EDGE_FALLING)
		edges |= S1C33_EDGE_FALLING;

	spin_lock_irqsave(&gpio->lock, flags);
	if (hwirq == S1C33_GPIO_P03) {
		gpio->port3_edges = edges;
		s1c33_gpio_arm_port3(gpio);
	} else {
		gpio->key_edges[hwirq - S1C33_GPIO_P60] = edges;
	}
	spin_unlock_irqrestore(&gpio->lock, flags);
	return 0;
}

static const struct irq_chip s1c33_gpio_irq_chip = {
	.name = "s1c33-gpio",
	.irq_mask = s1c33_gpio_irq_mask,
	.irq_unmask = s1c33_gpio_irq_unmask,
	.irq_set_type = s1c33_gpio_irq_set_type,
	.flags = IRQCHIP_IMMUTABLE | IRQCHIP_SKIP_SET_WAKE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static void s1c33_gpio_irq_valid_mask(struct gpio_chip *chip,
				      unsigned long *valid_mask,
				      unsigned int ngpios)
{
	unsigned int i;

	bitmap_zero(valid_mask, ngpios);
	set_bit(S1C33_GPIO_P03, valid_mask);
	for (i = 0; i < S1C33_GPIO_KEY_LINES; i++)
		set_bit(S1C33_GPIO_P60 + i, valid_mask);
}

/*
 * Key input 0 on P60..P62 comparing nothing yet, and port input 3 on P03
 * by edge.  The device tree supplies the registers and the two interrupts,
 * with their priorities; without them the chip is plain GPIO.
 */
static int s1c33_gpio_init_irq(struct platform_device *pdev,
			       struct s1c33_gpio *gpio)
{
	struct gpio_irq_chip *girq = &gpio->chip.irq;

	gpio->key_irq = platform_get_irq_byname_optional(pdev, "key0");
	gpio->port3_irq = platform_get_irq_byname_optional(pdev, "port3");
	if (gpio->key_irq <= 0 || gpio->port3_irq <= 0)
		return 0;
	gpio->irq_base = devm_platform_ioremap_resource_byname(pdev,
							       "interrupt");
	if (IS_ERR(gpio->irq_base))
		return PTR_ERR(gpio->irq_base);

	writeb(0, gpio->irq_base + S1C33_SMPK0);
	s1c33_gpio_update8(gpio->irq_base + S1C33_SPPK01, S1C33_SPPK0_MASK,
			   S1C33_SPPK0_P60);
	s1c33_gpio_update8(gpio->irq_base + S1C33_SPT03, S1C33_SPT3_MASK, 0);
	s1c33_gpio_update8(gpio->irq_base + S1C33_SEPT07, 0, S1C33_PORT3);
	gpio->port3_edges = S1C33_EDGE_RISING | S1C33_EDGE_FALLING;
	s1c33_gpio_arm_port3(gpio);

	gpio_irq_chip_set_chip(girq, &s1c33_gpio_irq_chip);
	girq->parent_handler = s1c33_gpio_irq_handler;
	girq->num_parents = 2;
	girq->parents = devm_kcalloc(&pdev->dev, 2, sizeof(*girq->parents),
				     GFP_KERNEL);
	if (!girq->parents)
		return -ENOMEM;
	girq->parents[0] = gpio->key_irq;
	girq->parents[1] = gpio->port3_irq;
	girq->default_type = IRQ_TYPE_NONE;
	girq->handler = handle_simple_irq;
	girq->init_valid_mask = s1c33_gpio_irq_valid_mask;
	return 1;
}

static int s1c33_gpio_probe(struct platform_device *pdev)
{
	struct s1c33_gpio *gpio;
	int interrupts;
	int ret;

	gpio = devm_kzalloc(&pdev->dev, sizeof(*gpio), GFP_KERNEL);
	if (!gpio)
		return -ENOMEM;
	gpio->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(gpio->base))
		return dev_err_probe(&pdev->dev, PTR_ERR(gpio->base),
				     "cannot map port registers\n");
	gpio->function_base = devm_platform_ioremap_resource_byname(pdev,
								    "function");
	if (IS_ERR(gpio->function_base))
		return dev_err_probe(&pdev->dev, PTR_ERR(gpio->function_base),
				     "cannot map function select registers\n");
	spin_lock_init(&gpio->lock);
	ret = s1c33_pinctrl_init(pdev, gpio);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot register pin controller\n");

	gpio->chip.label = "s1c33-gpio";
	gpio->chip.parent = &pdev->dev;
	gpio->chip.owner = THIS_MODULE;
	gpio->chip.get_direction = s1c33_gpio_get_direction;
	gpio->chip.direction_input = s1c33_gpio_direction_input;
	gpio->chip.direction_output = s1c33_gpio_direction_output;
	gpio->chip.get = s1c33_gpio_get;
	gpio->chip.set = s1c33_gpio_set;
	gpio->chip.request = gpiochip_generic_request;
	gpio->chip.free = gpiochip_generic_free;
	gpio->chip.base = -1;
	gpio->chip.ngpio = S1C33_GPIO_PORTS * S1C33_GPIO_PER_PORT;
	interrupts = s1c33_gpio_init_irq(pdev, gpio);
	if (interrupts < 0)
		return dev_err_probe(&pdev->dev, interrupts,
				     "cannot map interrupt registers\n");

	ret = devm_gpiochip_add_data(&pdev->dev, &gpio->chip, gpio);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot register GPIO controller\n");
	ret = gpiochip_add_pin_range(&gpio->chip, dev_name(&pdev->dev), 0, 0,
				     gpio->chip.ngpio);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot map GPIOs to pins\n");
	dev_info(&pdev->dev, "registered %zu pins, %u functions and 56 GPIOs%s\n",
		 ARRAY_SIZE(s1c33_pins), S1C33_FN_COUNT,
		 interrupts ? ", P03 and P60..P62 interrupting" : "");
	return 0;
}

static const struct of_device_id s1c33_gpio_of_match[] = {
	{ .compatible = "epson,s1c33-pinctrl" },
	{ }
};

static struct platform_driver s1c33_gpio_driver = {
	.probe = s1c33_gpio_probe,
	.driver = {
		.name = "s1c33-pinctrl",
		.of_match_table = s1c33_gpio_of_match,
	},
};
/*
 * Register early: nearly every device on the board takes pins or GPIOs from
 * this chip, so binding it first saves them a round of deferred probes.
 */
static int __init s1c33_gpio_init(void)
{
	return platform_driver_register(&s1c33_gpio_driver);
}
postcore_initcall(s1c33_gpio_init);

MODULE_DESCRIPTION("Epson S1C33E07 pin controller and GPIO");
MODULE_LICENSE("GPL");
