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
#include <linux/spi/mmc_spi.h>
#include <linux/spi/spi.h>

#include <linux/platform_data/spi-s1c33.h>

#include <asm/irq.h>
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
#define WR_CMU_GATE1      (WR_REG_BASE + 0x1b04)
#define WR_CMU_PROTECT    (WR_REG_BASE + 0x1b24)
#define WR_HS2_ENABLE     (WR_REG_BASE + 0x114c)
#define WR_HS3_ENABLE     (WR_REG_BASE + 0x115c)
#define WR_IDMA_ENABLE    (WR_REG_BASE + 0x1105)
#define WR_IDMAREQ_SPI    (WR_REG_BASE + 0x29b)
#define WR_IDMAEN_SPI     (WR_REG_BASE + 0x29c)

#define WR_CMU_GATE1_DMA  BIT(1)
#define WR_CMU_GATE1_SPI  BIT(6)
#define WR_IDMA_SPI_BIT   BIT(4)
#define WR_SD_CS          BIT(0)
#define WR_EEPROM_CS      BIT(2)
#define WR_CS_OUTPUTS     (WR_SD_CS | BIT(1) | WR_EEPROM_CS)
#define WR_SD_VCCEN       BIT(2)
#define WR_SD_BUFEN       BIT(3)
#define WR_SD_POWER_BITS  (WR_SD_VCCEN | WR_SD_BUFEN)
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

static void wr_mmc_setpower(struct device *dev, unsigned int vdd)
{
	if (!vdd) {
		wr_modify8(WR_P3_DATA, WR_SD_POWER_BITS, WR_SD_VCCEN);
		return;
	}

	wr_modify8(WR_P3_DATA, WR_SD_POWER_BITS, WR_SD_VCCEN);
	fsleep(10);
	wr_modify8(WR_P3_DATA, WR_SD_POWER_BITS, 0);
	mdelay(1);
	wr_modify8(WR_P3_DATA, WR_SD_POWER_BITS, WR_SD_BUFEN);
}

static struct mmc_spi_platform_data wr_mmc_pdata = {
	.caps = MMC_CAP_NEEDS_POLL,
	.ocr_mask = MMC_VDD_32_33 | MMC_VDD_33_34,
	.powerup_msecs = 10,
	.setpower = wr_mmc_setpower,
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
	.devices = wr_spi_devices,
	.num_devices = ARRAY_SIZE(wr_spi_devices),
	.dma_memory_start = 0x10000000,
	.dma_memory_end = 0x12000000,
};

static const struct resource wr_gpio_resource =
	DEFINE_RES_MEM(WR_REG_BASE + 0x380, 14);

static const struct resource wr_spi_resources[] = {
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x1700, 0x20, "spi"),
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x1100, 0xa0, "dma"),
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x263, 0x37, "itc"),
	DEFINE_RES_IRQ_NAMED(C33_IRQ_HSDMA3, "rx-dma"),
};

static const struct resource wr_lcd_resource =
	DEFINE_RES_MEM(0x00080000, 32 * 208);

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
	{ }
};

static const struct property_entry wr_touch_properties[] = {
	PROPERTY_ENTRY_STRING("compatible", "openmoko,wikireader-touchscreen"),
	PROPERTY_ENTRY_U32("current-speed", 9600),
	PROPERTY_ENTRY_U32("touchscreen-size-x", 240),
	PROPERTY_ENTRY_U32("touchscreen-size-y", 208),
	{ }
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
	struct platform_device_info spi_info = { };
	struct platform_device_info uart_info = { };
	struct platform_device *device;
	int ret;
	u32 gate;

	ret = software_node_register_node_group(wr_nodes);
	if (ret) {
		pr_err("C33 devices: firmware nodes failed: %d\n", ret);
		return ret;
	}

	/* WikiReader SPI pins, inactive chip selects, and SD power controls. */
	wr_modify8(WR_P6_FUNC47, 0xfc, 0x54);
	wr_modify8(WR_P5_FUNC03, 0x3f, 0x01);
	wr_modify8(WR_P5_DATA, 0, WR_CS_OUTPUTS);
	wr_modify8(WR_P5_DIR, 0, WR_CS_OUTPUTS);
	wr_modify8(WR_P3_DIR, 0, WR_SD_POWER_BITS);
	wr_mmc_setpower(NULL, 0);

	writel(0x96, (void __iomem *)WR_CMU_PROTECT);
	gate = readl((void __iomem *)WR_CMU_GATE1);
	writel(gate | WR_CMU_GATE1_SPI | WR_CMU_GATE1_DMA,
	       (void __iomem *)WR_CMU_GATE1);
	writel(0, (void __iomem *)WR_CMU_PROTECT);

	/* The card loader may have left its SPI DMA channels armed. */
	writew(0, (void __iomem *)WR_HS2_ENABLE);
	writew(0, (void __iomem *)WR_HS3_ENABLE);
	wr_modify8(WR_IDMAEN_SPI, WR_IDMA_SPI_BIT, 0);
	wr_modify8(WR_IDMAREQ_SPI, WR_IDMA_SPI_BIT, 0);
	writeb(0, (void __iomem *)WR_IDMA_ENABLE);

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
	device = platform_device_register_simple("s1c33-fb", -1,
						 &wr_lcd_resource, 1);
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
