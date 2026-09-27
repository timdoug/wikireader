// SPDX-License-Identifier: GPL-2.0-only
/* Legacy board description for devices not yet described by a device tree. */
#include <linux/bitops.h>
#include <linux/gpio/machine.h>
#include <linux/gpio/property.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/irqchip/s1c33-itc.h>
#include <linux/mmc/host.h>
#include <linux/pinctrl/machine.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/pwm.h>
#include <linux/regulator/fixed.h>
#include <linux/regulator/machine.h>

#include <linux/platform_data/dma-s1c33-hsdma.h>
#include <linux/platform_data/s1c33-sd.h>

#include <asm/irq.h>
#include <asm/page.h>
#include <asm/wikireader.h>

#define WR_REG_BASE       0x00300000UL
#define WR_T16_CHANNEL(n) (WR_REG_BASE + 0x780 + (n) * 8)
#define WR_T16_CLKCTL(n)  (WR_REG_BASE + 0x7e0 + (n) * 2)
#define WR_CONTRAST_TIMER 1
#define WR_SERIAL_PRIORITY (WR_REG_BASE + 0x26a)
#define WR_SERIAL_FLAGS    (WR_REG_BASE + 0x286)

#define WR_TOUCH_IRQS     (BIT(3) | BIT(4) | BIT(5))
#define WR_UART0_IRQS     (BIT(0) | BIT(1) | BIT(2))

static void wr_modify8(unsigned long address, u8 clear, u8 set)
{
	u8 value = readb((void __iomem *)address);

	writeb((value & ~clear) | set, (void __iomem *)address);
}

/*
 * Pin functions, per device, as the firmware sets them up.  GPIOs need no
 * entry: requesting a line selects its port function.
 */
static unsigned long wr_sclk_low[] = {
	PIN_CONF_PACKED(PIN_CONFIG_LEVEL, 0),
};

#define WR_PIN(device, pin, function) \
	PIN_MAP_MUX_GROUP_DEFAULT(device, "s1c33-pinctrl", pin, function)

static const struct pinctrl_map wr_pin_map[] = {
	WR_PIN("s1c33-uart.0", "P00", "sin0"),
	WR_PIN("s1c33-uart.0", "P01", "sout0"),
	/* The touch panel only talks. */
	WR_PIN("s1c33-uart.1", "P04", "sin1"),
	WR_PIN("s1c33-pwm", "P11", "tm1"),
	WR_PIN("s1c33-sd", "P65", "sdi"),
	WR_PIN("s1c33-sd", "P66", "sdo"),
	WR_PIN("s1c33-sd", "P67", "spi_clk"),
	/* SCLK as a port at its idle level while the controller restarts. */
	PIN_MAP_MUX_GROUP("s1c33-sd", "hold", "s1c33-pinctrl", "P67", "gpio"),
	PIN_MAP_CONFIGS_PIN("s1c33-sd", "hold", "s1c33-pinctrl", "P67",
			    wr_sclk_low),
	WR_PIN("s1c33-adc", "P70", "ain0"),
	WR_PIN("s1c33-adc", "P71", "ain1"),
	WR_PIN("s1c33-adc", "P72", "ain2"),
	WR_PIN("s1c33-fb", "P80", "fpframe"),
	WR_PIN("s1c33-fb", "P81", "fpline"),
	WR_PIN("s1c33-fb", "P82", "fpshift"),
	WR_PIN("s1c33-fb", "P83", "fpdrdy"),
	WR_PIN("s1c33-fb", "P94", "fpdat4"),
	WR_PIN("s1c33-fb", "P95", "fpdat5"),
	WR_PIN("s1c33-fb", "P96", "fpdat6"),
	WR_PIN("s1c33-fb", "P97", "fpdat7"),
};

/*
 * The card's 3.3 V rail is switched by P32 and the level buffer between the
 * card and the S1C33 by P33.  The rail needs a millisecond to settle before
 * the buffer may drive, and ten microseconds off before it may come back on;
 * both are constraints the regulator core enforces on its own.
 */
static struct regulator_consumer_supply wr_sd_vcc_consumer =
	REGULATOR_SUPPLY("vmmc", "s1c33-sd");

static struct regulator_init_data wr_sd_vcc_init = {
	.constraints = {
		.name = "sd-vcc",
		.min_uV = 3300000,
		.max_uV = 3300000,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies = 1,
	.consumer_supplies = &wr_sd_vcc_consumer,
};

static struct fixed_voltage_config wr_sd_vcc_config = {
	.supply_name = "sd-vcc",
	.microvolts = 3300000,
	.startup_delay = 1000,
	.off_on_delay = 10,
	.init_data = &wr_sd_vcc_init,
};

static struct regulator_consumer_supply wr_sd_buffer_consumer =
	REGULATOR_SUPPLY("vqmmc", "s1c33-sd");

static struct regulator_init_data wr_sd_buffer_init = {
	.constraints = {
		.name = "sd-buffer",
		.min_uV = 3300000,
		.max_uV = 3300000,
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
	.num_consumer_supplies = 1,
	.consumer_supplies = &wr_sd_buffer_consumer,
};

static struct fixed_voltage_config wr_sd_buffer_config = {
	.supply_name = "sd-buffer",
	.microvolts = 3300000,
	.init_data = &wr_sd_buffer_init,
};

/*
 * The enable lines come from a lookup table rather than a firmware node: the
 * fixed-voltage driver asks its node for an under-voltage interrupt first,
 * and a software node answers that question with an error it treats as fatal.
 */
static struct gpiod_lookup_table wr_sd_vcc_gpios = {
	.dev_id = "reg-fixed-voltage.0",
	.table = {
		GPIO_LOOKUP("s1c33-gpio", 3 * 8 + 2, NULL, GPIO_ACTIVE_LOW),
		{ }
	},
};

static struct gpiod_lookup_table wr_sd_buffer_gpios = {
	.dev_id = "reg-fixed-voltage.1",
	.table = {
		GPIO_LOOKUP("s1c33-gpio", 3 * 8 + 3, NULL, GPIO_ACTIVE_HIGH),
		{ }
	},
};

/*
 * AVDD is the converter's reference.  It is the 3.3 V rail itself, which
 * also feeds the thermistor's pull-up, so temperatures are ratiometric.
 */
static struct regulator_consumer_supply wr_avdd_consumer =
	REGULATOR_SUPPLY("vref", "s1c33-adc");

static struct regulator_init_data wr_avdd_init = {
	.constraints = {
		.name = "avdd",
		.min_uV = 3300000,
		.max_uV = 3300000,
		.always_on = 1,
	},
	.num_consumer_supplies = 1,
	.consumer_supplies = &wr_avdd_consumer,
};

static struct fixed_voltage_config wr_avdd_config = {
	.supply_name = "avdd",
	.microvolts = 3300000,
	.init_data = &wr_avdd_init,
};

/*
 * The card is the only device on the synchronous serial interface, and
 * s1c33-sd drives it directly as an MMC host in SPI mode.  It runs the card
 * at MCLK/4, as Grifo and the original firmware do.
 */
static struct s1c33_sd_platform_data wr_sd_pdata = {
	.powerup_msecs = 10,
};

/* The port block, its interrupt selection registers, and the two causes the
 * buttons and the power switch raise. */
static struct resource wr_gpio_resources[] __initdata = {
	DEFINE_RES_MEM(WR_REG_BASE + 0x380, 14),
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x3a0, 0x14, "function"),
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x3c0, 0x16, "interrupt"),
	DEFINE_RES_IRQ_NAMED(C33_IRQ_KEY0, "key0"),
	DEFINE_RES_IRQ_NAMED(C33_IRQ_PORT3, "port3"),
};

/*
 * The top of IVRAM, above the framebuffer, holds the host's CRC table:
 * internal RAM, so looking it up does not close the SDRAM row the block
 * being checked is read from.
 */
static struct resource wr_sd_resources[] __initdata = {
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x1700, 0x20, "spi"),
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x289, 1, "spi-flags"),
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x29b, 2, "idma"),
	DEFINE_RES_MEM_NAMED(0x00082e00, 0x200, "sram"),
};

/*
 * The DMA controller, and the interrupt controller's HSDMA priority,
 * transfer-count flags and trigger selects.  The card reads through
 * channels 2 and 3 on the SPI requests, trigger 9 on both.
 */
static const struct resource wr_hsdma_resources[] = {
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x1100, 0xa0, "dma"),
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x264, 1, "priority"),
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x281, 1, "flags"),
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x298, 2, "triggers"),
};

static const struct dma_slave_map wr_hsdma_map[] = {
	{ "s1c33-sd", "tx", HSDMA_REQUEST(2, 9) },
	{ "s1c33-sd", "rx", HSDMA_REQUEST(3, 9) },
};

static const struct s1c33_hsdma_platform_data wr_hsdma_pdata = {
	.slave_map = wr_hsdma_map,
	.slavecnt = ARRAY_SIZE(wr_hsdma_map),
};

static const struct resource wr_wdt_resources[] = {
	DEFINE_RES_MEM(WR_REG_BASE + 0x660, 0x10),
};

static const struct resource wr_adc_resources[] = {
	DEFINE_RES_MEM(WR_REG_BASE + 0x520, 0x40),
};

static const struct resource wr_lcd_resources[] = {
	DEFINE_RES_MEM_NAMED(0x00080000, 32 * 208, "vram"),
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x1a00, 0x100, "lcdc"),
};

static struct resource wr_uart0_resources[] __initdata = {
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x0b00, 8, "uart"),
	DEFINE_RES_IRQ_NAMED(C33_IRQ_UART0_RX, "rx"),
};

static struct resource wr_uart1_resources[] __initdata = {
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x0b10, 8, "uart"),
	DEFINE_RES_IRQ_NAMED(C33_IRQ_UART1_ERROR, "error"),
	DEFINE_RES_IRQ_NAMED(C33_IRQ_UART1_RX, "rx"),
};

static const struct software_node wr_gpio_node = {
	.name = "s1c33-gpio",
};

/* The SPI flash shares the card's bus; its chip select is held high. */
static const u32 wr_flash_cs_gpios[] = { 5 * 8 + 2, 0 };

static const struct property_entry wr_flash_cs_properties[] = {
	PROPERTY_ENTRY_BOOL("gpio-hog"),
	PROPERTY_ENTRY_U32_ARRAY("gpios", wr_flash_cs_gpios),
	PROPERTY_ENTRY_STRING("line-name", "flash-cs"),
	PROPERTY_ENTRY_BOOL("output-high"),
	{ }
};

static const struct software_node wr_flash_cs_node = {
	.name = "flash-cs",
	.parent = &wr_gpio_node,
	.properties = wr_flash_cs_properties,
};

static const struct property_entry wr_sd_properties[] = {
	PROPERTY_ENTRY_GPIO("cs-gpios", &wr_gpio_node, 5 * 8,
			    GPIO_ACTIVE_LOW),
	{ }
};

static const struct software_node wr_sd_node = {
	.name = "sd",
	.properties = wr_sd_properties,
};

static const struct property_entry wr_uart0_properties[] = {
	PROPERTY_ENTRY_U32("current-speed", 115200),
	{ }
};

static const struct property_entry wr_uart1_properties[] = {
	PROPERTY_ENTRY_U32("current-speed", 9600),
	PROPERTY_ENTRY_BOOL("epson,rx-only"),
	PROPERTY_ENTRY_BOOL("wakeup-source"),
	{ }
};

static const struct property_entry wr_touch_properties[] = {
	PROPERTY_ENTRY_STRING("compatible", "openmoko,wikireader-touchscreen"),
	PROPERTY_ENTRY_GPIO("reset-gpios", &wr_gpio_node, 0 * 8 + 7,
			    GPIO_ACTIVE_HIGH),
	PROPERTY_ENTRY_U32("current-speed", 9600),
	PROPERTY_ENTRY_U32("touchscreen-size-x", 240),
	PROPERTY_ENTRY_U32("touchscreen-size-y", 208),
	{ }
};

/* The panel's display-enable line is P30; the controller is on its own. */
static const struct property_entry wr_lcd_properties[] = {
	PROPERTY_ENTRY_GPIO("enable-gpios", &wr_gpio_node, 3 * 8,
			    GPIO_ACTIVE_HIGH),
	{ }
};

static const struct software_node wr_lcd_node = {
	.name = "lcd",
	.properties = wr_lcd_properties,
};

static const struct software_node wr_uart0_node = {
	.name = "uart0",
	.properties = wr_uart0_properties,
};

static const struct software_node wr_uart1_node = {
	.name = "uart1",
	.properties = wr_uart1_properties,
};

static const struct software_node wr_touch_node = {
	.name = "touchscreen",
	.parent = &wr_uart1_node,
	.properties = wr_touch_properties,
};

/*
 * The three front buttons are P60..P62 and the power switch is P03, all
 * pressed high: on the board P03 reads low with nobody near the switch,
 * which the firmware's power_switch_pressed() helper also assumes, and an
 * active-low description made every resume report a fresh press.  All four
 * interrupt through the GPIO chip (key input 0 and port input 3), and
 * gpio-keys debounces them.
 */
static const struct property_entry wr_buttons_properties[] = {
	PROPERTY_ENTRY_STRING("label", "WikiReader buttons"),
	{ }
};

static const struct software_node wr_buttons_node = {
	.name = "buttons",
	.properties = wr_buttons_properties,
};

#define WR_BUTTON(symbol, text, keycode, line, polarity)		\
	static const struct property_entry symbol##_properties[] = {	\
		PROPERTY_ENTRY_STRING("label", text),			\
		PROPERTY_ENTRY_U32("linux,code", keycode),		\
		PROPERTY_ENTRY_GPIO("gpios", &wr_gpio_node, line, polarity), \
		{ }							\
	};								\
	static const struct software_node symbol = {			\
		.name = text,						\
		.parent = &wr_buttons_node,				\
		.properties = symbol##_properties,			\
	}

WR_BUTTON(wr_button_random, "random", KEY_F1, 6 * 8 + 0, GPIO_ACTIVE_HIGH);
WR_BUTTON(wr_button_search, "search", KEY_SEARCH, 6 * 8 + 1, GPIO_ACTIVE_HIGH);
WR_BUTTON(wr_button_history, "history", KEY_BACK, 6 * 8 + 2, GPIO_ACTIVE_HIGH);
WR_BUTTON(wr_button_power, "power", KEY_POWER, 0 * 8 + 3, GPIO_ACTIVE_HIGH);

/*
 * The converter's inputs: AIN0 is the battery through a 150k/1M divider,
 * AIN1 a 100k NTC thermistor under a 120k pull-up to the 3.3 V rail, and
 * AIN2 the panel's V4 bias.  The thermistor is a TCT6GJ104H410, which
 * ntc_thermistor does not list; the Murata NCP03WF104 it does is the same
 * 100k at 25 C with a B constant of 4250 K.
 */
static const struct property_entry wr_adc_properties[] = {
	PROPERTY_ENTRY_U32("#io-channel-cells", 1),
	{ }
};

static const struct software_node wr_adc_node = {
	.name = "adc",
	.properties = wr_adc_properties,
};

static const struct property_entry wr_battery_divider_properties[] = {
	PROPERTY_ENTRY_REF("io-channels", &wr_adc_node, 0),
	PROPERTY_ENTRY_U32("output-ohms", 1000000),
	PROPERTY_ENTRY_U32("full-ohms", 1150000),
	PROPERTY_ENTRY_U32("#io-channel-cells", 1),
	{ }
};

static const struct software_node wr_battery_divider_node = {
	.name = "battery-divider",
	.properties = wr_battery_divider_properties,
};

static const struct property_entry wr_battery_properties[] = {
	PROPERTY_ENTRY_REF("io-channels", &wr_battery_divider_node, 0),
	PROPERTY_ENTRY_STRING("io-channel-names", "voltage"),
	{ }
};

static const struct software_node wr_battery_node = {
	.name = "battery",
	.properties = wr_battery_properties,
};

static const struct property_entry wr_thermistor_properties[] = {
	PROPERTY_ENTRY_REF("io-channels", &wr_adc_node, 1),
	PROPERTY_ENTRY_U32("pullup-uv", 3300000),
	PROPERTY_ENTRY_U32("pullup-ohm", 120000),
	PROPERTY_ENTRY_U32("pulldown-ohm", 0),
	{ }
};

static const struct software_node wr_thermistor_node = {
	.name = "thermistor",
	.properties = wr_thermistor_properties,
};

/* Timer 1 is the panel's contrast PWM; its output pin is P11. */
static const struct resource wr_pwm_resources[] = {
	DEFINE_RES_MEM_NAMED(WR_T16_CHANNEL(WR_CONTRAST_TIMER), 8, "timer"),
	DEFINE_RES_MEM_NAMED(WR_T16_CLKCTL(WR_CONTRAST_TIMER), 2, "clock"),
};

/* 4096 ticks of a 60 MHz MCLK, only used if the firmware left it stopped. */
static struct pwm_lookup wr_pwm_lookup[] = {
	PWM_LOOKUP("s1c33-pwm", 0, "wikireader-lcd", NULL, 68267,
		   PWM_POLARITY_NORMAL),
};

static const struct software_node *wr_nodes[] = {
	&wr_gpio_node,
	&wr_flash_cs_node,
	&wr_sd_node,
	&wr_lcd_node,
	&wr_uart0_node,
	&wr_uart1_node,
	&wr_touch_node,
	&wr_buttons_node,
	&wr_button_random,
	&wr_button_search,
	&wr_button_history,
	&wr_button_power,
	&wr_adc_node,
	&wr_battery_divider_node,
	&wr_battery_node,
	&wr_thermistor_node,
	NULL,
};

/* IRQ resources are written as trap vectors; the ITC domain names them. */
static void __init wr_map_irqs(struct resource *resources, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		int irq;

		if (!(resources[i].flags & IORESOURCE_IRQ))
			continue;
		irq = s1c33_itc_irq(resources[i].start);
		if (irq < 0)
			pr_err("C33 devices: no interrupt for vector %llu: %d\n",
			       (unsigned long long)resources[i].start, irq);
		resources[i].start = resources[i].end = irq;
	}
}

static int __init wr_register(const char *name, int id,
			      const struct resource *res, unsigned int num_res,
			      const struct software_node *node,
			      const void *data, size_t size_data)
{
	struct platform_device_info info = {
		.name = name,
		.id = id,
		.res = res,
		.num_res = num_res,
		.fwnode = node ? software_node_fwnode(node) : NULL,
		.data = data,
		.size_data = size_data,
	};
	struct platform_device *device = platform_device_register_full(&info);

	if (IS_ERR(device)) {
		pr_err("C33 devices: %s registration failed: %ld\n", name,
		       PTR_ERR(device));
		return PTR_ERR(device);
	}
	return 0;
}

/* The converter that reads the battery, thermistor and panel bias. */
static int __init wr_analog_init(void)
{
	int ret;

	ret = wr_register("reg-fixed-voltage", 2, NULL, 0, NULL,
			  &wr_avdd_config, sizeof(wr_avdd_config));
	if (!ret)
		ret = wr_register("s1c33-adc", -1, wr_adc_resources,
				  ARRAY_SIZE(wr_adc_resources), &wr_adc_node,
				  NULL, 0);
	return ret;
}

/*
 * What reads the converter, once every driver is registered and the
 * converter bound: registered any earlier, each consumer's probe would
 * defer and be retried every time some other device bound.
 */
static int __init wr_analog_consumers_init(void)
{
	int ret;

	ret = wr_register("voltage-divider", -1, NULL, 0,
			  &wr_battery_divider_node, NULL, 0);
	if (!ret)
		ret = wr_register("generic-adc-battery", -1, NULL, 0,
				  &wr_battery_node, NULL, 0);
	if (!ret)
		ret = wr_register("ncp03wf104", -1, NULL, 0,
				  &wr_thermistor_node, NULL, 0);
	return ret;
}
late_initcall(wr_analog_consumers_init);

static void __init wr_touch_prepare(void)
{
	writeb(WR_TOUCH_IRQS, (void __iomem *)WR_SERIAL_FLAGS);
	wr_modify8(WR_SERIAL_PRIORITY, 7, 6);
}

static void __init wr_uart_prepare(void)
{
	writeb(WR_UART0_IRQS, (void __iomem *)WR_SERIAL_FLAGS);
	wr_modify8(WR_SERIAL_PRIORITY, 0x70, 0x50);
}

static int __init c33_devices_init(void)
{
	struct platform_device_info gpio_info = { };
	struct platform_device_info lcd_info = { };
	struct platform_device_info regulator_info = { };
	struct platform_device_info sd_info = { };
	struct platform_device_info uart_info = { };
	struct platform_device_info pwm_info = { };
	struct platform_device_info contrast_info = { };
	struct platform_device_info buttons_info = { };
	struct platform_device *device;
	int ret;

	ret = software_node_register_node_group(wr_nodes);
	if (ret) {
		pr_err("C33 devices: firmware nodes failed: %d\n", ret);
		return ret;
	}

	wr_map_irqs(wr_gpio_resources, ARRAY_SIZE(wr_gpio_resources));
	wr_map_irqs(wr_uart0_resources, ARRAY_SIZE(wr_uart0_resources));
	wr_map_irqs(wr_uart1_resources, ARRAY_SIZE(wr_uart1_resources));

	ret = pinctrl_register_mappings(wr_pin_map, ARRAY_SIZE(wr_pin_map));
	if (ret) {
		pr_err("C33 devices: pin map failed: %d\n", ret);
		return ret;
	}

	gpio_info.name = "s1c33-pinctrl";
	gpio_info.id = -1;
	gpio_info.res = wr_gpio_resources;
	gpio_info.num_res = ARRAY_SIZE(wr_gpio_resources);
	gpio_info.fwnode = software_node_fwnode(&wr_gpio_node);
	device = platform_device_register_full(&gpio_info);
	if (IS_ERR(device)) {
		pr_err("C33 devices: GPIO platform registration failed: %ld\n",
		       PTR_ERR(device));
		return PTR_ERR(device);
	}
	/*
	 * The card's supplies have to exist before the slot looks for them:
	 * without a device tree a missing supply is an absent one, not a
	 * reason to defer.
	 */
	gpiod_add_lookup_table(&wr_sd_vcc_gpios);
	gpiod_add_lookup_table(&wr_sd_buffer_gpios);
	regulator_info.name = "reg-fixed-voltage";
	regulator_info.id = 0;
	regulator_info.data = &wr_sd_vcc_config;
	regulator_info.size_data = sizeof(wr_sd_vcc_config);
	device = platform_device_register_full(&regulator_info);
	if (IS_ERR(device)) {
		pr_err("C33 devices: SD supply registration failed: %ld\n",
		       PTR_ERR(device));
		return PTR_ERR(device);
	}
	regulator_info.id = 1;
	regulator_info.data = &wr_sd_buffer_config;
	regulator_info.size_data = sizeof(wr_sd_buffer_config);
	device = platform_device_register_full(&regulator_info);
	if (IS_ERR(device)) {
		pr_err("C33 devices: SD buffer registration failed: %ld\n",
		       PTR_ERR(device));
		return PTR_ERR(device);
	}
	ret = wr_register("s1c33-hsdma", -1, wr_hsdma_resources,
			  ARRAY_SIZE(wr_hsdma_resources), NULL,
			  &wr_hsdma_pdata, sizeof(wr_hsdma_pdata));
	if (ret)
		return ret;
	sd_info.name = "s1c33-sd";
	sd_info.id = -1;
	sd_info.res = wr_sd_resources;
	sd_info.num_res = ARRAY_SIZE(wr_sd_resources);
	sd_info.data = &wr_sd_pdata;
	sd_info.size_data = sizeof(wr_sd_pdata);
	sd_info.fwnode = software_node_fwnode(&wr_sd_node);
	device = platform_device_register_full(&sd_info);
	if (IS_ERR(device)) {
		pr_err("C33 devices: SD platform registration failed: %ld\n",
		       PTR_ERR(device));
		return PTR_ERR(device);
	}
	wr_uart_prepare();
	uart_info.name = "s1c33-uart";
	uart_info.id = 0;
	uart_info.res = wr_uart0_resources;
	uart_info.num_res = ARRAY_SIZE(wr_uart0_resources);
	uart_info.fwnode = software_node_fwnode(&wr_uart0_node);
	device = platform_device_register_full(&uart_info);
	if (IS_ERR(device)) {
		pr_err("C33 devices: UART0 platform registration failed: %ld\n",
		       PTR_ERR(device));
		return PTR_ERR(device);
	}
	lcd_info.name = "s1c33-fb";
	lcd_info.id = -1;
	lcd_info.res = wr_lcd_resources;
	lcd_info.num_res = ARRAY_SIZE(wr_lcd_resources);
	lcd_info.fwnode = software_node_fwnode(&wr_lcd_node);
	device = platform_device_register_full(&lcd_info);
	if (IS_ERR(device)) {
		pr_err("C33 devices: framebuffer registration failed: %ld\n",
		       PTR_ERR(device));
		return PTR_ERR(device);
	}
	wr_touch_prepare();
	uart_info.id = 1;
	uart_info.res = wr_uart1_resources;
	uart_info.num_res = ARRAY_SIZE(wr_uart1_resources);
	uart_info.fwnode = software_node_fwnode(&wr_uart1_node);
	device = platform_device_register_full(&uart_info);
	if (IS_ERR(device)) {
		pr_err("C33 devices: UART1 platform registration failed: %ld\n",
		       PTR_ERR(device));
		return PTR_ERR(device);
	}
	/* Contrast: timer 1 on P11, then the panel that consumes it. */
	pwm_info.name = "s1c33-pwm";
	pwm_info.id = -1;
	pwm_info.res = wr_pwm_resources;
	pwm_info.num_res = ARRAY_SIZE(wr_pwm_resources);
	device = platform_device_register_full(&pwm_info);
	if (IS_ERR(device)) {
		pr_err("C33 devices: PWM registration failed: %ld\n",
		       PTR_ERR(device));
		return PTR_ERR(device);
	}
	pwm_add_table(wr_pwm_lookup, ARRAY_SIZE(wr_pwm_lookup));
	contrast_info.name = "wikireader-lcd";
	contrast_info.id = -1;
	device = platform_device_register_full(&contrast_info);
	if (IS_ERR(device)) {
		pr_err("C33 devices: contrast registration failed: %ld\n",
		       PTR_ERR(device));
		return PTR_ERR(device);
	}
	/* Buttons and the power switch, as GPIOs. */
	buttons_info.name = "gpio-keys";
	buttons_info.id = -1;
	buttons_info.fwnode = software_node_fwnode(&wr_buttons_node);
	device = platform_device_register_full(&buttons_info);
	if (IS_ERR(device)) {
		pr_err("C33 devices: button registration failed: %ld\n",
		       PTR_ERR(device));
		return PTR_ERR(device);
	}
	ret = wr_analog_init();
	if (ret)
		return ret;
	ret = wr_register("s1c33-wdt", -1, wr_wdt_resources,
			  ARRAY_SIZE(wr_wdt_resources), NULL, NULL, 0);
	if (ret)
		return ret;
	pr_info("C33 devices: registered SD, UART, framebuffer, touchscreen, contrast, buttons, battery, and watchdog\n");
	return 0;
}
arch_initcall(c33_devices_init);
