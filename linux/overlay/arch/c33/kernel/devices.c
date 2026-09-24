// SPDX-License-Identifier: GPL-2.0-only
/* Legacy board description for devices not yet described by a device tree. */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/gpio/machine.h>
#include <linux/gpio/property.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mmc/host.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regulator/fixed.h>
#include <linux/regulator/machine.h>
#include <linux/spi/mmc_spi.h>
#include <linux/spi/spi.h>

#include <linux/platform_data/spi-s1c33.h>

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
#define WR_P0_FUNC47      (WR_REG_BASE + 0x3a1)
#define WR_P6_FUNC47      (WR_REG_BASE + 0x3ad)
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
	REGULATOR_SUPPLY("vmmc", "spi0.0");

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
	REGULATOR_SUPPLY("vqmmc", "spi0.0");

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

static struct mmc_spi_platform_data wr_mmc_pdata = {
	.caps = MMC_CAP_NEEDS_POLL,
	.ocr_mask = MMC_VDD_32_33 | MMC_VDD_33_34,
	.powerup_msecs = 10,
};

static struct spi_board_info wr_spi_devices[] = {
	{
		.modalias = "mmc-spi-slot",
		.platform_data = &wr_mmc_pdata,
		.max_speed_hz = 12000000,
		.bus_num = 0,
		.chip_select = 0,
		.mode = SPI_MODE_0,
	},
};

static struct s1c33_spi_platform_data wr_spi_pdata = {
	.hold_clock = wr_spi_hold_clock,
	.dma_memory_start = CONFIG_PHYSICAL_START,
};

static const struct resource wr_gpio_resource =
	DEFINE_RES_MEM(WR_REG_BASE + 0x380, 14);

static const struct resource wr_spi_resources[] = {
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x1700, 0x20, "spi"),
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x1100, 0xa0, "dma"),
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x263, 0x3a, "itc"),
	DEFINE_RES_IRQ_NAMED(C33_IRQ_HSDMA3, "rx-dma"),
};

static const struct resource wr_lcd_resources[] = {
	DEFINE_RES_MEM_NAMED(0x00080000, 32 * 208, "vram"),
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x1a00, 0x100, "lcdc"),
};

static const struct resource wr_uart0_resources[] = {
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x0b00, 8, "uart"),
	DEFINE_RES_IRQ_NAMED(C33_IRQ_UART0_RX, "rx"),
};

static const struct resource wr_uart1_resources[] = {
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x0b10, 8, "uart"),
	DEFINE_RES_IRQ_NAMED(C33_IRQ_UART1_ERROR, "error"),
	DEFINE_RES_IRQ_NAMED(C33_IRQ_UART1_RX, "rx"),
};

static const struct software_node wr_gpio_node = {
	.name = "s1c33-gpio",
};

static const struct property_entry wr_spi_properties[] = {
	PROPERTY_ENTRY_GPIO("cs-gpios", &wr_gpio_node, 5 * 8,
			    GPIO_ACTIVE_LOW),
	{ }
};

static const struct software_node wr_spi_node = {
	.name = "spi0",
	.properties = wr_spi_properties,
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

static const struct software_node *wr_nodes[] = {
	&wr_gpio_node,
	&wr_spi_node,
	&wr_lcd_node,
	&wr_uart0_node,
	&wr_uart1_node,
	&wr_touch_node,
	NULL,
};

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
	struct platform_device_info spi_info = { };
	struct platform_device_info uart_info = { };
	struct platform_device *device;
	int ret;

	/* SDRAM is the only memory HSDMA may reach; its size is probed. */
	wr_spi_pdata.dma_memory_end = memory_end;

	ret = software_node_register_node_group(wr_nodes);
	if (ret) {
		pr_err("C33 devices: firmware nodes failed: %d\n", ret);
		return ret;
	}

	/* The SPI core instantiates these once the controller claims bus 0. */
	ret = spi_register_board_info(wr_spi_devices, ARRAY_SIZE(wr_spi_devices));
	if (ret) {
		pr_err("C33 devices: SPI board info failed: %d\n", ret);
		return ret;
	}

	/* WikiReader SPI pins, inactive chip selects, and SD power controls. */
	wr_modify8(WR_P6_FUNC47, 0xfc, 0x54);
	wr_modify8(WR_P5_FUNC03, 0x3f, 0x01);
	wr_modify8(WR_P5_DATA, 0, WR_CS_OUTPUTS);
	wr_modify8(WR_P5_DIR, 0, WR_CS_OUTPUTS);

	gpio_info.name = "s1c33-gpio";
	gpio_info.id = -1;
	gpio_info.res = &wr_gpio_resource;
	gpio_info.num_res = 1;
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
	spi_info.name = "s1c33-spi";
	spi_info.id = -1;
	spi_info.res = wr_spi_resources;
	spi_info.num_res = ARRAY_SIZE(wr_spi_resources);
	spi_info.data = &wr_spi_pdata;
	spi_info.size_data = sizeof(wr_spi_pdata);
	spi_info.fwnode = software_node_fwnode(&wr_spi_node);
	device = platform_device_register_full(&spi_info);
	if (IS_ERR(device)) {
		pr_err("C33 devices: SPI platform registration failed: %ld\n",
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
	pr_info("C33 devices: registered SPI/MMC, UART, framebuffer, and touchscreen\n");
	return 0;
}
arch_initcall(c33_devices_init);
