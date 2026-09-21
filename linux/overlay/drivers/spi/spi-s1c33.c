// SPDX-License-Identifier: GPL-2.0-only
/* Epson S1C33 synchronous serial interface controller. */
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/irqflags.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>

#include <linux/platform_data/spi-s1c33.h>

#define S1C33_SPI_RXD       0x00
#define S1C33_SPI_TXD       0x04
#define S1C33_SPI_CTL1      0x08
#define S1C33_SPI_CTL2      0x0c
#define S1C33_SPI_WAIT      0x10
#define S1C33_SPI_STAT      0x14
#define S1C33_SPI_INT       0x18

#define S1C33_SPI_BPT_8     (7U << 10)
#define S1C33_SPI_CPHA      BIT(9)
#define S1C33_SPI_CPOL      BIT(8)
#define S1C33_SPI_DIV_SHIFT 4
#define S1C33_SPI_MASTER    BIT(1)
#define S1C33_SPI_ENABLE    BIT(0)
#define S1C33_SPI_BUSY      BIT(6)
#define S1C33_SPI_RX_FULL   BIT(2)

#define S1C33_SPI_POLLS     1000000

struct s1c33_spi {
	void __iomem *base;
	const struct s1c33_spi_platform_data *pdata;
	u32 control;
};

static int s1c33_spi_wait(struct s1c33_spi *hw, u32 flag, bool wanted)
{
	unsigned int count;

	for (count = 0; count < S1C33_SPI_POLLS; count++) {
		if (!!(readl(hw->base + S1C33_SPI_STAT) & flag) == wanted)
			return 0;
		cpu_relax();
	}
	return -ETIMEDOUT;
}

static unsigned int s1c33_spi_divisor(struct s1c33_spi *hw,
				       unsigned int requested,
				       unsigned long *effective)
{
	unsigned long clock = hw->pdata->get_clock_rate();
	unsigned int setting = 0;
	unsigned int divisor = 4;

	while (setting < 7 && clock / divisor > requested) {
		setting++;
		divisor <<= 1;
	}
	*effective = clock / divisor;
	return setting;
}

static unsigned long s1c33_spi_configure(struct s1c33_spi *hw,
					 unsigned int speed_hz,
					 unsigned int mode)
{
	unsigned long effective;
	unsigned int divider = s1c33_spi_divisor(hw, speed_hz, &effective);
	unsigned int settle;
	unsigned long flags;
	u32 interrupts;
	u32 control = S1C33_SPI_BPT_8 |
		(divider << S1C33_SPI_DIV_SHIFT) | S1C33_SPI_MASTER;

	if (mode & SPI_CPHA)
		control |= S1C33_SPI_CPHA;
	if (mode & SPI_CPOL)
		control |= S1C33_SPI_CPOL;

	if (control == hw->control)
		return effective;
	local_irq_save(flags);
	if (hw->pdata->hold_clock)
		hw->pdata->hold_clock(true, mode & SPI_CPOL);
	interrupts = readl(hw->base + S1C33_SPI_INT);
	writel(0, hw->base + S1C33_SPI_INT);
	writel(0, hw->base + S1C33_SPI_CTL1);
	writel(control, hw->base + S1C33_SPI_CTL1);
	writel(control | S1C33_SPI_ENABLE, hw->base + S1C33_SPI_CTL1);
	for (settle = 4U << divider; settle; settle--)
		cpu_relax();
	if (hw->pdata->hold_clock)
		hw->pdata->hold_clock(false, false);
	writel(interrupts, hw->base + S1C33_SPI_INT);
	local_irq_restore(flags);
	hw->control = control;
	return effective;
}

static int s1c33_spi_setup(struct spi_device *spi)
{
	if (spi->mode & SPI_LSB_FIRST)
		return -EINVAL;
	if (spi->bits_per_word && spi->bits_per_word != 8)
		return -EINVAL;
	return 0;
}

static int s1c33_spi_prepare_message(struct spi_controller *controller,
				      struct spi_message *message)
{
	struct s1c33_spi *hw = spi_controller_get_devdata(controller);
	struct spi_transfer *transfer;

	/*
	 * Program the serial clock before the SPI core asserts chip select.
	 * Disabling this controller while an S1C33 pin is actively driving
	 * SCLK creates an edge on the wire, so doing this from ->setup() or
	 * for the first byte of a message can consume a card response bit.
	 */
	transfer = list_first_entry(&message->transfers, struct spi_transfer,
				    transfer_list);
	s1c33_spi_configure(hw, transfer->speed_hz, message->spi->mode);
	return 0;
}

static void s1c33_spi_set_cs(struct spi_device *spi, bool high)
{
	struct s1c33_spi *hw = spi_controller_get_devdata(spi->controller);

	hw->pdata->set_cs(spi_get_chipselect(spi, 0), high);
}

static int s1c33_spi_transfer_one(struct spi_controller *controller,
				  struct spi_device *spi,
				  struct spi_transfer *transfer)
{
	struct s1c33_spi *hw = spi_controller_get_devdata(controller);
	const u8 *tx = transfer->tx_buf;
	u8 *rx = transfer->rx_buf;
	unsigned long effective;
	unsigned int i;

	if (transfer->bits_per_word != 8)
		return -EINVAL;
	effective = s1c33_spi_configure(hw, transfer->speed_hz, spi->mode);
	transfer->effective_speed_hz = effective;

	for (i = 0; i < transfer->len; i++) {
		if (s1c33_spi_wait(hw, S1C33_SPI_BUSY, false))
			return -ETIMEDOUT;
		writel(tx ? tx[i] : 0xff, hw->base + S1C33_SPI_TXD);
		if (s1c33_spi_wait(hw, S1C33_SPI_RX_FULL, true))
			return -ETIMEDOUT;
		if (rx)
			rx[i] = readl(hw->base + S1C33_SPI_RXD);
		else
			readl(hw->base + S1C33_SPI_RXD);
	}
	return 0;
}

static int s1c33_spi_probe(struct platform_device *pdev)
{
	const struct s1c33_spi_platform_data *pdata =
		dev_get_platdata(&pdev->dev);
	struct spi_controller *controller;
	struct s1c33_spi *hw;
	unsigned int i;
	unsigned long clock;
	int ret;

	if (!pdata || !pdata->get_clock_rate || !pdata->set_cs)
		return -EINVAL;
	controller = devm_spi_alloc_host(&pdev->dev, sizeof(*hw));
	if (!controller)
		return -ENOMEM;
	hw = spi_controller_get_devdata(controller);
	hw->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(hw->base))
		return dev_err_probe(&pdev->dev, PTR_ERR(hw->base),
				     "cannot map controller registers\n");
	hw->pdata = pdata;
	hw->control = ~0U;

	clock = pdata->get_clock_rate();
	controller->bus_num = 0;
	controller->num_chipselect = 1;
	controller->mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH;
	controller->bits_per_word_mask = SPI_BPW_MASK(8);
	controller->min_speed_hz = clock / 512;
	controller->max_speed_hz = clock / 4;
	controller->setup = s1c33_spi_setup;
	controller->prepare_message = s1c33_spi_prepare_message;
	controller->set_cs = s1c33_spi_set_cs;
	controller->transfer_one = s1c33_spi_transfer_one;

	writel(0, hw->base + S1C33_SPI_CTL2);
	writel(0, hw->base + S1C33_SPI_WAIT);
	writel(0, hw->base + S1C33_SPI_INT);
	readl(hw->base + S1C33_SPI_RXD);
	pdata->set_cs(0, true);

	platform_set_drvdata(pdev, controller);
	ret = devm_spi_register_controller(&pdev->dev, controller);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot register SPI controller\n");
	for (i = 0; i < pdata->num_devices; i++) {
		if (!spi_new_device(controller, &pdata->devices[i]))
			return dev_err_probe(&pdev->dev, -ENODEV,
					     "cannot register SPI device %u\n", i);
	}
	dev_info(&pdev->dev, "SPI controller at %u..%u Hz\n",
		 controller->min_speed_hz, controller->max_speed_hz);
	return 0;
}

static struct platform_driver s1c33_spi_driver = {
	.driver.name = "s1c33-spi",
	.probe = s1c33_spi_probe,
};
module_platform_driver(s1c33_spi_driver);

MODULE_DESCRIPTION("Epson S1C33 SPI controller");
MODULE_LICENSE("GPL");
