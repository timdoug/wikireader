// SPDX-License-Identifier: GPL-2.0-only
/*
 * The Epson S1C33E07's 10-bit successive-approximation A/D converter.
 *
 * Five inputs, AIN0..AIN4, measure against AVDD, which the board supplies as
 * the "vref" regulator.  Each read converts one channel on a software
 * trigger in the converter's standard mode and polls for the end: at
 * MCLK/256 a conversion takes about 80 us, which is less than a sleep would
 * cost.  Between reads the converter is disabled and its clock gated, since
 * an enabled converter draws current from AVDD whether it converts or not.
 *
 * The prescaler is the original firmware's MCLK/256, not the fastest the
 * converter allows: the WikiReader's inputs are high-impedance dividers,
 * and the nine-clock sampling window has to be long enough to charge the
 * sample capacitor through them.
 */
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>

#include <linux/iio/iio.h>

#define S1C33_AD_CLKCTL		0x00
#define S1C33_AD_ADD		0x20
#define S1C33_AD_TRIG_CHNL	0x22
#define S1C33_AD_EN_SMPL_STAT	0x24
#define S1C33_AD_ADVMODE	0x3e

#define S1C33_CLKCTL_PSONAD	BIT(3)
#define S1C33_CLKCTL_MCLK_256	0x7

#define S1C33_TRIG_CS		GENMASK(10, 8)
#define S1C33_TRIG_CE		GENMASK(13, 11)

/* Nine-clock sampling, the manual's only recommended setting. */
#define S1C33_CTRL_ST_9		(3 << 8)
#define S1C33_CTRL_ADE		BIT(2)
#define S1C33_CTRL_ADST		BIT(1)

#define S1C33_ADC_BITS		10
#define S1C33_ADC_CHANNELS	5

struct s1c33_adc {
	void __iomem *base;
	struct clk *clk;
	int vref_uv;
	/* One conversion at a time: the channel select is shared. */
	struct mutex lock;
};

static int s1c33_adc_convert(struct s1c33_adc *adc, unsigned int channel)
{
	u16 control;
	int ret;

	ret = clk_prepare_enable(adc->clk);
	if (ret)
		return ret;

	writew(S1C33_CLKCTL_PSONAD | S1C33_CLKCTL_MCLK_256,
	       adc->base + S1C33_AD_CLKCTL);
	/* Standard mode; everything else is set while the converter is off. */
	writew(0, adc->base + S1C33_AD_ADVMODE);
	writew(S1C33_CTRL_ST_9, adc->base + S1C33_AD_EN_SMPL_STAT);
	writew(FIELD_PREP(S1C33_TRIG_CS, channel) |
	       FIELD_PREP(S1C33_TRIG_CE, channel),
	       adc->base + S1C33_AD_TRIG_CHNL);
	writew(S1C33_CTRL_ST_9 | S1C33_CTRL_ADE,
	       adc->base + S1C33_AD_EN_SMPL_STAT);
	writew(S1C33_CTRL_ST_9 | S1C33_CTRL_ADE | S1C33_CTRL_ADST,
	       adc->base + S1C33_AD_EN_SMPL_STAT);

	/* In normal mode ADST falls once the selected channel is converted. */
	ret = readw_poll_timeout_atomic(adc->base + S1C33_AD_EN_SMPL_STAT,
					control, !(control & S1C33_CTRL_ADST),
					1, 1000);
	if (!ret)
		ret = readw(adc->base + S1C33_AD_ADD) &
		      GENMASK(S1C33_ADC_BITS - 1, 0);

	writew(S1C33_CTRL_ST_9, adc->base + S1C33_AD_EN_SMPL_STAT);
	writew(S1C33_CLKCTL_MCLK_256, adc->base + S1C33_AD_CLKCTL);
	clk_disable_unprepare(adc->clk);
	return ret;
}

static int s1c33_adc_read_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan,
			      int *val, int *val2, long mask)
{
	struct s1c33_adc *adc = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&adc->lock);
		ret = s1c33_adc_convert(adc, chan->channel);
		mutex_unlock(&adc->lock);
		if (ret < 0)
			return ret;
		*val = ret;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = adc->vref_uv / 1000;
		*val2 = S1C33_ADC_BITS;
		return IIO_VAL_FRACTIONAL_LOG2;
	default:
		return -EINVAL;
	}
}

static const struct iio_info s1c33_adc_info = {
	.read_raw = s1c33_adc_read_raw,
};

#define S1C33_ADC_CHANNEL(n) {					\
	.type = IIO_VOLTAGE,					\
	.indexed = 1,						\
	.channel = (n),						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
	.datasheet_name = "AIN" #n,				\
}

static const struct iio_chan_spec s1c33_adc_channels[S1C33_ADC_CHANNELS] = {
	S1C33_ADC_CHANNEL(0),
	S1C33_ADC_CHANNEL(1),
	S1C33_ADC_CHANNEL(2),
	S1C33_ADC_CHANNEL(3),
	S1C33_ADC_CHANNEL(4),
};

static int s1c33_adc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct iio_dev *indio_dev;
	struct s1c33_adc *adc;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*adc));
	if (!indio_dev)
		return -ENOMEM;
	adc = iio_priv(indio_dev);

	adc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(adc->base))
		return PTR_ERR(adc->base);
	adc->clk = devm_clk_get(dev, NULL);
	if (IS_ERR(adc->clk))
		return dev_err_probe(dev, PTR_ERR(adc->clk),
				     "cannot get the converter clock\n");
	adc->vref_uv = devm_regulator_get_enable_read_voltage(dev, "vref");
	if (adc->vref_uv < 0)
		return dev_err_probe(dev, adc->vref_uv,
				     "cannot read the reference voltage\n");
	ret = devm_mutex_init(dev, &adc->lock);
	if (ret)
		return ret;

	/* Leave the converter as the manual wants it when unused: disabled. */
	ret = clk_prepare_enable(adc->clk);
	if (ret)
		return ret;
	writew(S1C33_CTRL_ST_9, adc->base + S1C33_AD_EN_SMPL_STAT);
	writew(S1C33_CLKCTL_MCLK_256, adc->base + S1C33_AD_CLKCTL);
	clk_disable_unprepare(adc->clk);

	indio_dev->name = "s1c33-adc";
	indio_dev->info = &s1c33_adc_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = s1c33_adc_channels;
	indio_dev->num_channels = ARRAY_SIZE(s1c33_adc_channels);
	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id s1c33_adc_of_match[] = {
	{ .compatible = "epson,s1c33-adc" },
	{ }
};
MODULE_DEVICE_TABLE(of, s1c33_adc_of_match);

static struct platform_driver s1c33_adc_driver = {
	.driver.name = "s1c33-adc",
	.driver.of_match_table = s1c33_adc_of_match,
	.probe = s1c33_adc_probe,
};
module_platform_driver(s1c33_adc_driver);

MODULE_DESCRIPTION("Epson S1C33E07 A/D converter");
MODULE_LICENSE("GPL");
