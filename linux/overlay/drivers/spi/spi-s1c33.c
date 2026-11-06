// SPDX-License-Identifier: GPL-2.0-only
/*
 * The Epson S1C33E07's synchronous serial interface as an SPI controller.
 *
 * Every transfer is synchronous and polled: the CPU waits on the port's
 * status register rather than sleeping for an interrupt, since a transfer
 * here takes less than a sleep and wakeup, and a halted CPU on this part
 * stops the DMA's request pipeline.
 *
 * Transfers go a byte a character by the CPU, except those of whole words,
 * at least SPI_S1C33_DMA_MIN long: those go by HSDMA, 32 bits a character,
 * which is where DMA pays on this part (a byte a trigger costs more than
 * the CPU loop).  A 32-bit character goes out and comes in most
 * significant byte first, so the bytes pass through a bounce buffer of
 * big-endian words.  The
 * character size can only change with the port disabled, which puts an
 * edge on SCLK with the device selected, so the pin is parked at its idle
 * level, the "hold" pin state, across the change.
 */
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/dmaengine.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>

#define S1C33_SPI_RXD			0x00
#define S1C33_SPI_TXD			0x04
#define S1C33_SPI_CTL1		0x08
#define S1C33_SPI_CTL2		0x0c
#define S1C33_SPI_WAIT		0x10
#define S1C33_SPI_STAT		0x14
#define S1C33_SPI_INT			0x18

#define S1C33_SPI_BPT(bits)		(((bits) - 1U) << 10)
#define S1C33_SPI_CPHA		BIT(9)
#define S1C33_SPI_CPOL		BIT(8)
#define S1C33_SPI_DIV_SHIFT		4
#define S1C33_SPI_DIV_MAX		7		/* MCLK / (4 << 7) */
#define S1C33_SPI_TX_DMA		BIT(3)
#define S1C33_SPI_RX_DMA		BIT(2)
#define S1C33_SPI_MASTER		BIT(1)
#define S1C33_SPI_ENABLE		BIT(0)
#define S1C33_SPI_BUSY		BIT(6)
#define S1C33_SPI_RX_FULL		BIT(2)

#define S1C33_SPI_POLLS		1000000
#define SPI_S1C33_DMA_MIN	64
#define SPI_S1C33_BOUNCE	4096

struct s1c33_spi {
	void __iomem *base;
	phys_addr_t base_phys;
	unsigned long clock;
	u32 control;			/* CTL1 as programmed, enable aside */
	struct pinctrl *pinctrl;
	struct pinctrl_state *pins_default, *pins_hold;
	struct dma_chan *rx_chan, *tx_chan;
	unsigned int rx_irq, tx_irq;	/* the DMA requests, polled */
	u32 *bounce;			/* the words on the wire, both ways */
	dma_addr_t bounce_dma;
	unsigned long dma_transfers, pio_bytes;
};

static int s1c33_spi_wait(struct s1c33_spi *spi, u32 flag, bool wanted)
{
	unsigned int count;

	for (count = 0; count < S1C33_SPI_POLLS; count++)
		if (!!(readl(spi->base + S1C33_SPI_STAT) & flag) == wanted)
			return 0;
	return -ETIMEDOUT;
}

/*
 * Program the character size, clock and mode, with both DMA requests
 * enabled for good: they only raise interrupt-controller flags until a
 * transfer arms HSDMA.  A change disables the port, so SCLK is held at
 * its idle level across it.
 */
static void s1c33_spi_configure(struct s1c33_spi *spi, unsigned int bits,
				u32 speed, u32 mode)
{
	unsigned int divider = 0, settle;
	u32 control;

	while (divider < S1C33_SPI_DIV_MAX && spi->clock >> (divider + 2) > speed)
		divider++;
	control = S1C33_SPI_BPT(bits) | divider << S1C33_SPI_DIV_SHIFT | S1C33_SPI_MASTER |
		  S1C33_SPI_RX_DMA | S1C33_SPI_TX_DMA |
		  (mode & SPI_CPOL ? S1C33_SPI_CPOL : 0) |
		  (mode & SPI_CPHA ? S1C33_SPI_CPHA : 0);
	if (control == spi->control)
		return;
	if (spi->pins_hold)
		pinctrl_select_state(spi->pinctrl, spi->pins_hold);
	writel(0, spi->base + S1C33_SPI_CTL1);
	writel(control, spi->base + S1C33_SPI_CTL1);
	writel(control | S1C33_SPI_ENABLE, spi->base + S1C33_SPI_CTL1);
	for (settle = 4U << divider; settle; settle--)
		cpu_relax();
	if (spi->pins_hold)
		pinctrl_select_state(spi->pinctrl, spi->pins_default);
	spi->control = control;
}

static int s1c33_spi_pio(struct s1c33_spi *spi, struct spi_transfer *xfer)
{
	const u8 *tx = xfer->tx_buf;
	u8 *rx = xfer->rx_buf;
	unsigned int i;

	for (i = 0; i < xfer->len; i++) {
		u32 in;

		if (s1c33_spi_wait(spi, S1C33_SPI_BUSY, false))
			return -ETIMEDOUT;
		writel(tx ? tx[i] : 0, spi->base + S1C33_SPI_TXD);
		if (s1c33_spi_wait(spi, S1C33_SPI_RX_FULL, true))
			return -ETIMEDOUT;
		in = readl(spi->base + S1C33_SPI_RXD);
		if (rx)
			rx[i] = in;
	}
	spi->pio_bytes += xfer->len;
	return 0;
}

static int s1c33_spi_dma_wait(struct dma_chan *chan, dma_cookie_t cookie)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(100);
	enum dma_status status;

	while ((status = dmaengine_tx_status(chan, cookie, NULL)) ==
	       DMA_IN_PROGRESS)
		if (time_after(jiffies, deadline))
			return -ETIMEDOUT;
	return status == DMA_COMPLETE ? 0 : -EIO;
}

/*
 * A transfer of whole words: HSDMA 3 moves the received words to the
 * bounce buffer while HSDMA 2 feeds the transmitter from it, whose first
 * word the CPU writes to start the exchange.  Each word goes out and comes
 * in most significant byte first, the byte order of a big-endian word.
 */
static int s1c33_spi_dma(struct s1c33_spi *spi, struct spi_transfer *xfer)
{
	struct dma_async_tx_descriptor *rxd, *txd;
	unsigned int words = xfer->len / 4, i;
	const u8 *tx = xfer->tx_buf;
	u8 *rx = xfer->rx_buf;
	dma_cookie_t rx_cookie;
	int ret;

	for (i = 0; i < words; i++)
		spi->bounce[i] = tx ? get_unaligned_be32(tx + 4 * i) : 0;

	rxd = dmaengine_prep_slave_single(spi->rx_chan, spi->bounce_dma,
					  xfer->len, DMA_DEV_TO_MEM, 0);
	txd = dmaengine_prep_slave_single(spi->tx_chan, spi->bounce_dma + 4,
					  xfer->len - 4, DMA_MEM_TO_DEV, 0);
	if (!rxd || !txd)
		return -ENOMEM;
	rx_cookie = dmaengine_submit(rxd);
	if (dma_submit_error(rx_cookie) ||
	    dma_submit_error(dmaengine_submit(txd)))
		return -EBUSY;

	irq_set_irqchip_state(spi->rx_irq, IRQCHIP_STATE_PENDING, false);
	irq_set_irqchip_state(spi->tx_irq, IRQCHIP_STATE_PENDING, false);
	dma_async_issue_pending(spi->rx_chan);
	dma_async_issue_pending(spi->tx_chan);
	writel(spi->bounce[0], spi->base + S1C33_SPI_TXD);

	/* The port stays busy until the last word is in. */
	s1c33_spi_wait(spi, S1C33_SPI_BUSY, false);
	ret = s1c33_spi_dma_wait(spi->rx_chan, rx_cookie);
	if (!ret && s1c33_spi_wait(spi, S1C33_SPI_BUSY, false))
		ret = -ETIMEDOUT;
	if (ret) {
		dmaengine_terminate_sync(spi->rx_chan);
		dmaengine_terminate_sync(spi->tx_chan);
		return ret;
	}
	if (rx)
		for (i = 0; i < words; i++)
			put_unaligned_be32(spi->bounce[i], rx + 4 * i);
	spi->dma_transfers++;
	return 0;
}

/* Whole words by HSDMA; off, every transfer goes a byte at a time. */
static bool use_dma = true;
module_param_named(dma, use_dma, bool, 0644);

static bool s1c33_spi_use_dma(struct s1c33_spi *spi,
			      struct spi_transfer *xfer)
{
	return use_dma && spi->rx_chan && xfer->len >= SPI_S1C33_DMA_MIN &&
	       !(xfer->len & 3) && xfer->len <= SPI_S1C33_BOUNCE;
}

static int s1c33_spi_transfer_one(struct spi_controller *host,
				  struct spi_device *dev,
				  struct spi_transfer *xfer)
{
	struct s1c33_spi *spi = spi_controller_get_devdata(host);
	int ret;

	if (s1c33_spi_use_dma(spi, xfer)) {
		s1c33_spi_configure(spi, 32, xfer->speed_hz, dev->mode);
		ret = s1c33_spi_dma(spi, xfer);
	} else {
		s1c33_spi_configure(spi, 8, xfer->speed_hz, dev->mode);
		ret = s1c33_spi_pio(spi, xfer);
	}
	/* Synchronous: the core does not wait for a completion. */
	return ret;
}

static int s1c33_spi_dma_init(struct platform_device *pdev,
			      struct s1c33_spi *spi)
{
	struct dma_slave_config config = {
		.src_addr = spi->base_phys + S1C33_SPI_RXD,
		.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
		.dst_addr = spi->base_phys + S1C33_SPI_TXD,
		.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
	};
	struct device *dev = &pdev->dev;
	int ret;

	spi->rx_chan = devm_dma_request_chan(dev, "rx");
	if (IS_ERR(spi->rx_chan)) {
		ret = PTR_ERR(spi->rx_chan);
		spi->rx_chan = NULL;
		return ret == -ENODEV ? 0 : ret;
	}
	spi->tx_chan = devm_dma_request_chan(dev, "tx");
	if (IS_ERR(spi->tx_chan))
		return PTR_ERR(spi->tx_chan);
	config.direction = DMA_DEV_TO_MEM;
	ret = dmaengine_slave_config(spi->rx_chan, &config);
	config.direction = DMA_MEM_TO_DEV;
	if (!ret)
		ret = dmaengine_slave_config(spi->tx_chan, &config);
	if (ret)
		return ret;
	spi->bounce = dmam_alloc_coherent(dmaengine_get_dma_device(spi->rx_chan),
					  SPI_S1C33_BOUNCE, &spi->bounce_dma,
					  GFP_KERNEL);
	if (!spi->bounce)
		return -ENOMEM;

	ret = platform_get_irq_byname(pdev, "rx");
	if (ret < 0)
		return ret;
	spi->rx_irq = ret;
	ret = platform_get_irq_byname(pdev, "tx");
	if (ret < 0)
		return ret;
	spi->tx_irq = ret;
	return 0;
}

static ssize_t transfers_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct spi_controller *host = dev_get_drvdata(dev);
	struct s1c33_spi *spi = spi_controller_get_devdata(host);

	return sysfs_emit(buf, "%lu dma %lu pio-bytes\n", spi->dma_transfers,
			  spi->pio_bytes);
}
static DEVICE_ATTR_RO(transfers);

static struct attribute *s1c33_spi_attrs[] = {
	&dev_attr_transfers.attr,
	NULL
};
ATTRIBUTE_GROUPS(s1c33_spi);

static int s1c33_spi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct spi_controller *host;
	struct s1c33_spi *spi;
	struct resource *res;
	struct clk *clk;
	int ret;

	host = devm_spi_alloc_host(dev, sizeof(*spi));
	if (!host)
		return -ENOMEM;
	spi = spi_controller_get_devdata(host);

	spi->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(spi->base))
		return PTR_ERR(spi->base);
	spi->base_phys = res->start;
	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "no clock\n");
	spi->clock = clk_get_rate(clk);
	if (!spi->clock)
		return dev_err_probe(dev, -EINVAL, "clock has no rate\n");

	spi->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(spi->pinctrl))
		return dev_err_probe(dev, PTR_ERR(spi->pinctrl),
				     "cannot get pin control\n");
	spi->pins_default = pinctrl_lookup_state(spi->pinctrl,
						 PINCTRL_STATE_DEFAULT);
	spi->pins_hold = pinctrl_lookup_state(spi->pinctrl, "hold");
	if (IS_ERR(spi->pins_default) || IS_ERR(spi->pins_hold))
		spi->pins_hold = NULL;

	ret = s1c33_spi_dma_init(pdev, spi);
	if (ret)
		return dev_err_probe(dev, ret, "no DMA\n");

	writel(0, spi->base + S1C33_SPI_CTL2);
	writel(0, spi->base + S1C33_SPI_WAIT);
	writel(0, spi->base + S1C33_SPI_INT);
	readl(spi->base + S1C33_SPI_RXD);

	host->dev.of_node = dev->of_node;
	host->bus_num = -1;
	host->mode_bits = SPI_CPOL | SPI_CPHA;
	host->bits_per_word_mask = SPI_BPW_MASK(8);
	host->min_speed_hz = spi->clock >> (S1C33_SPI_DIV_MAX + 2);
	host->max_speed_hz = spi->clock >> 2;
	host->use_gpio_descriptors = true;
	host->transfer_one = s1c33_spi_transfer_one;
	platform_set_drvdata(pdev, host);

	ret = devm_spi_register_controller(dev, host);
	if (ret)
		return ret;
	dev_info(dev, "SPI controller at up to %u Hz, %s\n",
		 host->max_speed_hz, spi->rx_chan ?
		 "whole words by HSDMA" : "no DMA");
	return 0;
}

static const struct of_device_id s1c33_spi_of_match[] = {
	{ .compatible = "epson,s1c33e07-spi" },
	{ }
};
MODULE_DEVICE_TABLE(of, s1c33_spi_of_match);

static struct platform_driver s1c33_spi_driver = {
	.driver = {
		.name = "s1c33-spi",
		.of_match_table = s1c33_spi_of_match,
		.dev_groups = s1c33_spi_groups,
	},
	.probe = s1c33_spi_probe,
};
module_platform_driver(s1c33_spi_driver);

MODULE_DESCRIPTION("Epson S1C33E07 SPI controller");
MODULE_LICENSE("GPL");
