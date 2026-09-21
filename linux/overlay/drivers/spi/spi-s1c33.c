// SPDX-License-Identifier: GPL-2.0-only
/* Epson S1C33 synchronous serial interface controller. */
#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/irqflags.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/swab.h>
#include <linux/unaligned.h>

#include <linux/platform_data/spi-s1c33.h>

#define S1C33_SPI_RXD       0x00
#define S1C33_SPI_TXD       0x04
#define S1C33_SPI_CTL1      0x08
#define S1C33_SPI_CTL2      0x0c
#define S1C33_SPI_WAIT      0x10
#define S1C33_SPI_STAT      0x14
#define S1C33_SPI_INT       0x18

#define S1C33_SPI_BPT(bits) (((bits) - 1U) << 10)
#define S1C33_SPI_CPHA      BIT(9)
#define S1C33_SPI_CPOL      BIT(8)
#define S1C33_SPI_DIV_SHIFT 4
#define S1C33_SPI_MASTER    BIT(1)
#define S1C33_SPI_ENABLE    BIT(0)
#define S1C33_SPI_BUSY      BIT(6)
#define S1C33_SPI_RX_FULL   BIT(2)
#define S1C33_SPI_RX_DMA    BIT(2)
#define S1C33_SPI_TX_DMA    BIT(3)

#define S1C33_SPI_POLLS     1000000
#define S1C33_DMA_TIMEOUT   1000

#define S1C33_DMA_HS2       0x40
#define S1C33_DMA_HS3       0x50
#define S1C33_DMA_COUNT     0x00
#define S1C33_DMA_CONTROL   0x02
#define S1C33_DMA_SOURCE_LO 0x04
#define S1C33_DMA_SOURCE_HI 0x06
#define S1C33_DMA_DEST_LO   0x08
#define S1C33_DMA_DEST_HI   0x0a
#define S1C33_DMA_ENABLE    0x0c
#define S1C33_DMA_TRIGGER   0x0e
#define S1C33_DMA_ADV_MODE  0x9c
#define S1C33_DMA_ADV_CTL2  0x82
#define S1C33_DMA_ADV_SRC2  0x84
#define S1C33_DMA_ADV_DST2  0x88
#define S1C33_DMA_ADV_CTL3  0x92
#define S1C33_DMA_ADV_SRC3  0x94
#define S1C33_DMA_ADV_DST3  0x98

#define S1C33_ITC_DMA_PRIORITY 0x01
#define S1C33_ITC_DMA_FLAG     0x1e
#define S1C33_ITC_SPI_FLAG     0x26
#define S1C33_ITC_HS_TRIGGER   0x36
#define S1C33_HSDMA2_FLAG    BIT(2)
#define S1C33_HSDMA3_FLAG    BIT(3)
#define S1C33_SPI_DMA_FLAGS  (BIT(4) | BIT(5))

struct s1c33_spi {
	void __iomem *base;
	void __iomem *dma;
	void __iomem *itc;
	const struct s1c33_spi_platform_data *pdata;
	u32 control;
	u32 dummy;
	struct completion dma_done;
	int dma_irq;
	unsigned long dma_transfers;
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
					 unsigned int mode,
					 unsigned int bits,
					 bool dma)
{
	unsigned long effective;
	unsigned int divider = s1c33_spi_divisor(hw, speed_hz, &effective);
	unsigned int settle;
	unsigned long flags;
	u32 interrupts;
	u32 control = S1C33_SPI_BPT(bits) |
		(divider << S1C33_SPI_DIV_SHIFT) | S1C33_SPI_MASTER;

	if (mode & SPI_CPHA)
		control |= S1C33_SPI_CPHA;
	if (mode & SPI_CPOL)
		control |= S1C33_SPI_CPOL;
	if (dma)
		control |= S1C33_SPI_RX_DMA | S1C33_SPI_TX_DMA;

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
	s1c33_spi_configure(hw, transfer->speed_hz, message->spi->mode, 8,
			     false);
	return 0;
}

static void s1c33_spi_set_cs(struct spi_device *spi, bool high)
{
	struct s1c33_spi *hw = spi_controller_get_devdata(spi->controller);

	hw->pdata->set_cs(spi_get_chipselect(spi, 0), high);
}

static bool s1c33_spi_all_ones(const u8 *buffer, unsigned int length)
{
	unsigned int i;

	if (!buffer)
		return true;
	for (i = 0; i < length; i++)
		if (buffer[i] != 0xff)
			return false;
	return true;
}

static void s1c33_hsdma_channel(struct s1c33_spi *hw, unsigned int channel,
				 unsigned int count, u32 source, u32 destination,
				 bool increment_source,
				 bool increment_destination)
{
	unsigned int base = channel == 2 ? S1C33_DMA_HS2 : S1C33_DMA_HS3;
	unsigned int adv_control = channel == 2 ? S1C33_DMA_ADV_CTL2 :
		S1C33_DMA_ADV_CTL3;
	unsigned int adv_source = channel == 2 ? S1C33_DMA_ADV_SRC2 :
		S1C33_DMA_ADV_SRC3;
	unsigned int adv_destination = channel == 2 ? S1C33_DMA_ADV_DST2 :
		S1C33_DMA_ADV_DST3;

	writew(0, hw->dma + base + S1C33_DMA_ENABLE);
	writew(1, hw->dma + adv_control);
	writew(count, hw->dma + base + S1C33_DMA_COUNT);
	writew(0x8000, hw->dma + base + S1C33_DMA_CONTROL);
	writew(0, hw->dma + base + S1C33_DMA_SOURCE_LO);
	writew(increment_source ? 0x2000 : 0,
	       hw->dma + base + S1C33_DMA_SOURCE_HI);
	writew(0, hw->dma + base + S1C33_DMA_DEST_LO);
	writew(increment_destination ? 0x2000 : 0,
	       hw->dma + base + S1C33_DMA_DEST_HI);
	writel(source, hw->dma + adv_source);
	writel(destination, hw->dma + adv_destination);
	writew(1, hw->dma + base + S1C33_DMA_TRIGGER);
}

static irqreturn_t s1c33_spi_dma_interrupt(int irq, void *data)
{
	struct s1c33_spi *hw = data;

	complete(&hw->dma_done);
	return IRQ_HANDLED;
}

static int s1c33_spi_dma_read(struct s1c33_spi *hw, struct spi_device *spi,
			      struct spi_transfer *transfer)
{
	u8 *rx = transfer->rx_buf;
	u32 destination = (u32)(unsigned long)rx;
	unsigned int words = transfer->len / 4;
	unsigned int i;
	u8 old_trigger;
	bool completed;
	bool irq_disabled = false;

	if (!rx || transfer->len < 64 || transfer->len & 3 ||
	    destination & 3 || words > 0xffff ||
	    destination < hw->pdata->dma_memory_start ||
	    destination + transfer->len > hw->pdata->dma_memory_end ||
	    !s1c33_spi_all_ones(transfer->tx_buf, transfer->len))
		return -EOPNOTSUPP;

	s1c33_spi_configure(hw, transfer->speed_hz, spi->mode, 32, true);
	hw->dummy = ~0U;
	reinit_completion(&hw->dma_done);
	writew(1, hw->dma + S1C33_DMA_ADV_MODE);
	s1c33_hsdma_channel(hw, 3, words,
			      (u32)(unsigned long)hw->base + S1C33_SPI_RXD,
			      destination, false, true);
	s1c33_hsdma_channel(hw, 2, words - 1,
			      (u32)(unsigned long)&hw->dummy,
			      (u32)(unsigned long)hw->base + S1C33_SPI_TXD,
			      false, false);
	old_trigger = readb(hw->itc + S1C33_ITC_HS_TRIGGER);
	writeb(0x99, hw->itc + S1C33_ITC_HS_TRIGGER);
	writeb(S1C33_SPI_DMA_FLAGS, hw->itc + S1C33_ITC_SPI_FLAG);
	writeb(S1C33_HSDMA2_FLAG | S1C33_HSDMA3_FLAG,
	       hw->itc + S1C33_ITC_DMA_FLAG);
	writew(1, hw->dma + S1C33_DMA_HS3 + S1C33_DMA_ENABLE);
	writew(1, hw->dma + S1C33_DMA_HS2 + S1C33_DMA_ENABLE);
	writel(~0U, hw->base + S1C33_SPI_TXD);

	completed = wait_for_completion_timeout(&hw->dma_done,
						msecs_to_jiffies(S1C33_DMA_TIMEOUT));
	if (!completed) {
		/* Close the late-IRQ race before checking the latched cause. */
		disable_irq(hw->dma_irq);
		irq_disabled = true;
		completed = completion_done(&hw->dma_done) ||
			(readb(hw->itc + S1C33_ITC_DMA_FLAG) &
			 S1C33_HSDMA3_FLAG);
	}
	writew(0, hw->dma + S1C33_DMA_HS2 + S1C33_DMA_ENABLE);
	writew(0, hw->dma + S1C33_DMA_HS3 + S1C33_DMA_ENABLE);
	writeb(old_trigger, hw->itc + S1C33_ITC_HS_TRIGGER);
	writeb(S1C33_HSDMA2_FLAG | S1C33_HSDMA3_FLAG,
	       hw->itc + S1C33_ITC_DMA_FLAG);
	if (irq_disabled)
		enable_irq(hw->dma_irq);
	if (!completed || s1c33_spi_wait(hw, S1C33_SPI_BUSY, false))
		return -ETIMEDOUT;

	for (i = 0; i < transfer->len; i += 4) {
		u32 value = get_unaligned((u32 *)(rx + i));

		put_unaligned(swab32(value), (u32 *)(rx + i));
	}
	hw->dma_transfers++;
	dev_info_once(&spi->dev, "32-bit HSDMA bulk reads use IRQ completion\n");
	return 0;
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
	u32 value;
	int ret;

	if (transfer->bits_per_word != 8)
		return -EINVAL;
	ret = s1c33_spi_dma_read(hw, spi, transfer);
	if (!ret) {
		transfer->effective_speed_hz =
			s1c33_spi_configure(hw, transfer->speed_hz,
					    spi->mode, 32, true);
		return 0;
	}
	if (ret != -EOPNOTSUPP)
		return ret;
	effective = s1c33_spi_configure(hw, transfer->speed_hz, spi->mode,
					 transfer->len >= 4 ? 32 : 8, false);
	transfer->effective_speed_hz = effective;

	for (i = 0; i + 4 <= transfer->len; i += 4) {
		if (s1c33_spi_wait(hw, S1C33_SPI_BUSY, false))
			return -ETIMEDOUT;
		value = tx ? get_unaligned_be32(tx + i) : ~0U;
		writel(value, hw->base + S1C33_SPI_TXD);
		if (s1c33_spi_wait(hw, S1C33_SPI_RX_FULL, true))
			return -ETIMEDOUT;
		value = readl(hw->base + S1C33_SPI_RXD);
		if (rx)
			put_unaligned_be32(value, rx + i);
	}
	if (i != transfer->len)
		s1c33_spi_configure(hw, transfer->speed_hz, spi->mode, 8, false);
	for (; i < transfer->len; i++) {
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
	int irq;
	int ret;

	if (!pdata || !pdata->get_clock_rate || !pdata->set_cs)
		return -EINVAL;
	controller = devm_spi_alloc_host(&pdev->dev, sizeof(*hw));
	if (!controller)
		return -ENOMEM;
	hw = spi_controller_get_devdata(controller);
	hw->base = devm_platform_ioremap_resource_byname(pdev, "spi");
	if (IS_ERR(hw->base))
		return dev_err_probe(&pdev->dev, PTR_ERR(hw->base),
				     "cannot map controller registers\n");
	hw->dma = devm_platform_ioremap_resource_byname(pdev, "dma");
	if (IS_ERR(hw->dma))
		return dev_err_probe(&pdev->dev, PTR_ERR(hw->dma),
				     "cannot map DMA registers\n");
	hw->itc = devm_platform_ioremap_resource_byname(pdev, "itc");
	if (IS_ERR(hw->itc))
		return dev_err_probe(&pdev->dev, PTR_ERR(hw->itc),
				     "cannot map interrupt registers\n");
	hw->pdata = pdata;
	hw->control = ~0U;
	init_completion(&hw->dma_done);
	writeb((readb(hw->itc + S1C33_ITC_DMA_PRIORITY) & 0x8f) | 0x40,
	       hw->itc + S1C33_ITC_DMA_PRIORITY);
	irq = platform_get_irq_byname(pdev, "rx-dma");
	if (irq < 0)
		return irq;
	hw->dma_irq = irq;
	ret = devm_request_irq(&pdev->dev, irq,
			       s1c33_spi_dma_interrupt, 0, "s1c33-spi-rx", hw);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot request receive DMA interrupt\n");

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
