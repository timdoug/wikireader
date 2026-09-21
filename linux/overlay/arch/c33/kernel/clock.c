// SPDX-License-Identifier: GPL-2.0
/* Clock-rate discovery and common-clock registration for the S1C33E07. */
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/types.h>

#include <asm/wikireader.h>

#define C33_CMU_CLKCNTL 0x00301b08UL
#define C33_CMU_PLL     0x00301b0cUL

#define C33_OSC3_HZ     48000000UL
#define C33_OSC1_HZ     32768UL

unsigned long c33_mclk_hz(void)
{
	unsigned long ctl = readl((void __iomem *)C33_CMU_CLKCNTL);
	unsigned long hz;
	unsigned int source = (ctl >> 2) & 3;

	if (source == 1) {
		hz = C33_OSC1_HZ;
	} else if (source == 3) {
		unsigned long pll = readl((void __iomem *)C33_CMU_PLL);
		unsigned int input_div = ((ctl >> 20) & 15) + 1;

		/* Values 10..15 are reserved; the documented reset is /8. */
		if (input_div > 10)
			input_div = 8;
		switch (input_div) {
		case 1:
			hz = C33_OSC3_HZ;
			break;
		case 2:
			hz = C33_OSC3_HZ / 2;
			break;
		case 3:
			hz = C33_OSC3_HZ / 3;
			break;
		case 4:
			hz = C33_OSC3_HZ / 4;
			break;
		case 5:
			hz = C33_OSC3_HZ / 5;
			break;
		case 6:
			hz = C33_OSC3_HZ / 6;
			break;
		case 7:
			hz = C33_OSC3_HZ / 7;
			break;
		case 8:
			hz = C33_OSC3_HZ / 8;
			break;
		case 9:
			hz = C33_OSC3_HZ / 9;
			break;
		default:
			hz = C33_OSC3_HZ / 10;
			break;
		}
		hz *= ((pll >> 4) & 15) + 1;
	} else {
		unsigned int shift = (ctl >> 8) & 7;

		/* OSC3DIV encodes /1,/2,/4,/8,/16,/32. */
		if (shift > 5)
			shift = 0;
		hz = C33_OSC3_HZ >> shift;
	}

	if (ctl & (1 << 12))
		hz >>= 1;
	return hz;
}

static unsigned long c33_mclk_recalc_rate(struct clk_hw *hw,
					  unsigned long parent_rate)
{
	return c33_mclk_hz();
}

static const struct clk_ops c33_mclk_ops = {
	.recalc_rate = c33_mclk_recalc_rate,
};

static const struct clk_init_data c33_mclk_init = {
	.name = "mclk",
	.ops = &c33_mclk_ops,
};

static struct clk_hw c33_mclk_hw = {
	.init = &c33_mclk_init,
};

static int __init c33_clock_init(void)
{
	static const char * const consumers[] = {
		"s1c33-uart.0",
		"s1c33-uart.1",
		"s1c33-spi",
	};
	struct clk_lookup *lookups[ARRAY_SIZE(consumers)];
	unsigned int i;
	int ret;

	ret = clk_hw_register(NULL, &c33_mclk_hw);
	if (ret)
		return ret;
	for (i = 0; i < ARRAY_SIZE(consumers); i++) {
		lookups[i] = clkdev_hw_create(&c33_mclk_hw, NULL, "%s",
					      consumers[i]);
		if (!lookups[i])
			goto err_drop_lookups;
	}
	pr_info("C33 clock: registered %lu Hz MCLK\n",
		c33_mclk_hz());
	return 0;

err_drop_lookups:
	while (i)
		clkdev_drop(lookups[--i]);
	clk_hw_unregister(&c33_mclk_hw);
	return -ENOMEM;
}
postcore_initcall(c33_clock_init);
