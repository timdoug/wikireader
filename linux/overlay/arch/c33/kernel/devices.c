// SPDX-License-Identifier: GPL-2.0-only
/* Legacy board description for devices not yet described by a device tree. */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/gpio/machine.h>
#include <linux/gpio/property.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/irqchip/s1c33-itc.h>
#include <linux/mmc/host.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/pwm.h>
#include <linux/regulator/fixed.h>
#include <linux/regulator/machine.h>

#include <linux/platform_data/s1c33-sd.h>

#include <asm/irq.h>
#include <asm/page.h>
#include <asm/wikireader.h>

#define WR_REG_BASE       0x00300000UL
#define WR_P0_DATA        (WR_REG_BASE + 0x380)
#define WR_P0_DIR         (WR_REG_BASE + 0x381)
#define WR_P3_DATA        (WR_REG_BASE + 0x386)
#define WR_P3_DIR         (WR_REG_BASE + 0x387)
#define WR_P5_DATA        (WR_REG_BASE + 0x38a)
#define WR_P5_DIR         (WR_REG_BASE + 0x38b)
#define WR_P6_DATA        (WR_REG_BASE + 0x38c)
#define WR_P6_DIR         (WR_REG_BASE + 0x38d)
#define WR_P5_FUNC03      (WR_REG_BASE + 0x3aa)
#define WR_P0_FUNC03      (WR_REG_BASE + 0x3a0)
#define WR_P0_FUNC47      (WR_REG_BASE + 0x3a1)
#define WR_P1_FUNC03      (WR_REG_BASE + 0x3a2)
#define WR_P6_FUNC03      (WR_REG_BASE + 0x3ac)
#define WR_P6_FUNC47      (WR_REG_BASE + 0x3ad)
#define WR_P7_FUNC03      (WR_REG_BASE + 0x3ae)
#define WR_T16_CHANNEL(n) (WR_REG_BASE + 0x780 + (n) * 8)
#define WR_T16_CLKCTL(n)  (WR_REG_BASE + 0x7e0 + (n) * 2)
#define WR_CONTRAST_TIMER 1
#define WR_SERIAL_PRIORITY (WR_REG_BASE + 0x26a)
#define WR_SERIAL_FLAGS    (WR_REG_BASE + 0x286)

#define WR_SD_CS          BIT(0)
#define WR_EEPROM_CS      BIT(2)
#define WR_CS_OUTPUTS     (WR_SD_CS | BIT(1) | WR_EEPROM_CS)
#define WR_TOUCH_IRQS     (BIT(3) | BIT(4) | BIT(5))
#define WR_UART0_IRQS     (BIT(0) | BIT(1) | BIT(2))

static void wr_modify8(unsigned long address, u8 clear, u8 set)
{
	u8 value = readb((void __iomem *)address);

	writeb((value & ~clear) | set, (void __iomem *)address);
}

static void wr_spi_hold_clock(bool hold, bool high)
{
	static u8 saved_mux;
	static u8 saved_dir;
	static u8 saved_data;

	if (hold) {
		saved_mux = readb((void __iomem *)WR_P6_FUNC47);
		saved_dir = readb((void __iomem *)WR_P6_DIR);
		saved_data = readb((void __iomem *)WR_P6_DATA);
		writeb((saved_data & ~BIT(7)) | (high ? BIT(7) : 0),
		       (void __iomem *)WR_P6_DATA);
		writeb(saved_dir | BIT(7), (void __iomem *)WR_P6_DIR);
		writeb(saved_mux & ~0xc0, (void __iomem *)WR_P6_FUNC47);
		return;
	}

	writeb(saved_mux, (void __iomem *)WR_P6_FUNC47);
	writeb(saved_dir, (void __iomem *)WR_P6_DIR);
	writeb(saved_data, (void __iomem *)WR_P6_DATA);
}

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
	.hold_clock = wr_spi_hold_clock,
	.dma_memory_start = CONFIG_PHYSICAL_START,
	.powerup_msecs = 10,
};

/* The port block, its interrupt selection registers, and the two causes the
 * buttons and the power switch raise. */
static struct resource wr_gpio_resources[] __initdata = {
	DEFINE_RES_MEM(WR_REG_BASE + 0x380, 14),
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x3c0, 0x16, "interrupt"),
	DEFINE_RES_IRQ_NAMED(C33_IRQ_KEY0, "key0"),
	DEFINE_RES_IRQ_NAMED(C33_IRQ_PORT3, "port3"),
};

static struct resource wr_sd_resources[] __initdata = {
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x1700, 0x20, "spi"),
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x1100, 0xa0, "dma"),
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x263, 0x3a, "itc"),
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

/* The battery, thermistor and panel bias, and what reads them. */
static int __init wr_analog_init(void)
{
	int ret;

	wr_modify8(WR_P7_FUNC03, 0x3f, 0x15);
	ret = wr_register("reg-fixed-voltage", 2, NULL, 0, NULL,
			  &wr_avdd_config, sizeof(wr_avdd_config));
	if (!ret)
		ret = wr_register("s1c33-adc", -1, wr_adc_resources,
				  ARRAY_SIZE(wr_adc_resources), &wr_adc_node,
				  NULL, 0);
	if (!ret)
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

static void __init wr_touch_prepare(void)
{
	/* UART1 pin mux plus the panel reset connected to P07. */
	wr_modify8(WR_P0_FUNC47, 3, 1);
	wr_modify8(WR_P0_DIR, 0, BIT(7));
	wr_modify8(WR_P0_DATA, 0, BIT(7));
	fsleep(20);
	wr_modify8(WR_P0_DATA, BIT(7), 0);
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

	/* SDRAM is the only memory HSDMA may reach; its size is probed. */
	wr_sd_pdata.dma_memory_end = memory_end;

	ret = software_node_register_node_group(wr_nodes);
	if (ret) {
		pr_err("C33 devices: firmware nodes failed: %d\n", ret);
		return ret;
	}

	wr_map_irqs(wr_gpio_resources, ARRAY_SIZE(wr_gpio_resources));
	wr_map_irqs(wr_uart0_resources, ARRAY_SIZE(wr_uart0_resources));
	wr_map_irqs(wr_uart1_resources, ARRAY_SIZE(wr_uart1_resources));

	/* WikiReader SPI pins, inactive chip selects, and SD power controls. */
	wr_modify8(WR_P6_FUNC47, 0xfc, 0x54);
	wr_modify8(WR_P5_FUNC03, 0x3f, 0x01);
	wr_modify8(WR_P5_DATA, 0, WR_CS_OUTPUTS);
	wr_modify8(WR_P5_DIR, 0, WR_CS_OUTPUTS);

	gpio_info.name = "s1c33-gpio";
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
	wr_modify8(WR_P1_FUNC03, 0x0c, 0x04);
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
	/* Buttons and the power switch as plain inputs. */
	wr_modify8(WR_P6_FUNC03, 0x3f, 0);
	wr_modify8(WR_P0_FUNC03, 0xc0, 0);
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
	pr_info("C33 devices: registered SD, UART, framebuffer, touchscreen, contrast, buttons, and battery\n");
	return 0;
}
arch_initcall(c33_devices_init);
