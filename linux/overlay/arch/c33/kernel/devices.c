// SPDX-License-Identifier: GPL-2.0-only
/* Legacy board description for devices not yet described by a device tree. */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/mmc/host.h>
#include <linux/platform_device.h>
#include <linux/spi/mmc_spi.h>
#include <linux/spi/spi.h>

#include <linux/platform_data/spi-s1c33.h>

#include <asm/irq.h>
#include <asm/wikireader.h>

#define WR_REG_BASE       0x00300000UL
#define WR_P3_DATA        (WR_REG_BASE + 0x386)
#define WR_P3_DIR         (WR_REG_BASE + 0x387)
#define WR_P5_DATA        (WR_REG_BASE + 0x38a)
#define WR_P5_DIR         (WR_REG_BASE + 0x38b)
#define WR_P6_DATA        (WR_REG_BASE + 0x38c)
#define WR_P6_DIR         (WR_REG_BASE + 0x38d)
#define WR_P5_FUNC03      (WR_REG_BASE + 0x3aa)
#define WR_P6_FUNC47      (WR_REG_BASE + 0x3ad)
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

static void wr_modify8(unsigned long address, u8 clear, u8 set)
{
	u8 value = readb((void __iomem *)address);

	writeb((value & ~clear) | set, (void __iomem *)address);
}

static void wr_spi_set_cs(unsigned int chip_select, bool high)
{
	if (chip_select)
		return;
	wr_modify8(WR_P5_DATA, high ? 0 : WR_SD_CS,
		   high ? WR_SD_CS : 0);
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
	.get_clock_rate = c33_mclk_hz,
	.set_cs = wr_spi_set_cs,
	.hold_clock = wr_spi_hold_clock,
	.devices = wr_spi_devices,
	.num_devices = ARRAY_SIZE(wr_spi_devices),
	.dma_memory_start = 0x10000000,
	.dma_memory_end = 0x12000000,
};

static const struct resource wr_spi_resources[] = {
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x1700, 0x20, "spi"),
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x1100, 0xa0, "dma"),
	DEFINE_RES_MEM_NAMED(WR_REG_BASE + 0x263, 0x37, "itc"),
	DEFINE_RES_IRQ_NAMED(C33_IRQ_HSDMA3, "rx-dma"),
};

static const struct resource wr_lcd_resource =
	DEFINE_RES_MEM(0x00080000, 32 * 208);

static int __init c33_devices_init(void)
{
	struct platform_device *device;
	u32 gate;

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

	device = platform_device_register_resndata(NULL, "s1c33-spi", -1,
		wr_spi_resources, ARRAY_SIZE(wr_spi_resources),
		&wr_spi_pdata, sizeof(wr_spi_pdata));
	if (IS_ERR(device)) {
		pr_err("C33 devices: SPI platform registration failed: %ld\n",
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
	pr_info("C33 devices: registered SPI controller and MMC slot\n");
	return 0;
}
arch_initcall(c33_devices_init);
