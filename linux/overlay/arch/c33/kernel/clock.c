// SPDX-License-Identifier: GPL-2.0
/* Clock-rate discovery and common-clock registration for the S1C33E07. */
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <asm/clock.h>

#define C33_CMU_GATE1   0x00301b04UL
#define C33_CMU_CLKCNTL 0x00301b08UL
#define C33_CMU_PLL     0x00301b0cUL
#define C33_CMU_PROTECT 0x00301b24UL
#define C33_CMU_UNLOCK  0x96

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

/* Every CMU register refuses writes unless PROTECT is holding the key. */
static DEFINE_SPINLOCK(c33_cmu_lock);

void c33_cmu_gate(u32 mask, bool enable)
{
	unsigned long flags;
	u32 gate;

	spin_lock_irqsave(&c33_cmu_lock, flags);
	writel(C33_CMU_UNLOCK, (void __iomem *)C33_CMU_PROTECT);
	gate = readl((void __iomem *)C33_CMU_GATE1);
	if (enable)
		gate |= mask;
	else
		gate &= ~mask;
	writel(gate, (void __iomem *)C33_CMU_GATE1);
	writel(0, (void __iomem *)C33_CMU_PROTECT);
	spin_unlock_irqrestore(&c33_cmu_lock, flags);
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

struct c33_gate {
	struct clk_hw hw;
	u32 mask;
};

static inline struct c33_gate *to_c33_gate(struct clk_hw *hw)
{
	return container_of(hw, struct c33_gate, hw);
}

static int c33_gate_enable(struct clk_hw *hw)
{
	c33_cmu_gate(to_c33_gate(hw)->mask, true);
	return 0;
}

static void c33_gate_disable(struct clk_hw *hw)
{
	c33_cmu_gate(to_c33_gate(hw)->mask, false);
}

static int c33_gate_is_enabled(struct clk_hw *hw)
{
	u32 mask = to_c33_gate(hw)->mask;

	return (readl((void __iomem *)C33_CMU_GATE1) & mask) == mask;
}

/* A gate passes its parent's rate through: no recalc_rate, no set_rate. */
static const struct clk_ops c33_gate_ops = {
	.enable = c33_gate_enable,
	.disable = c33_gate_disable,
	.is_enabled = c33_gate_is_enabled,
};

#define C33_GATE(symbol, clock_name, bits)				\
	static struct c33_gate symbol = {				\
		.hw.init = CLK_HW_INIT_HW(clock_name, &c33_mclk_hw,	\
					  &c33_gate_ops, 0),		\
		.mask = bits,						\
	}

C33_GATE(c33_spi_gate, "spi", C33_CMU_SPI);
C33_GATE(c33_dma_gate, "hsdma", C33_CMU_DMA);
C33_GATE(c33_efsio_gate, "efsio", C33_CMU_EFSIO);
C33_GATE(c33_tm1_gate, "tm1", C33_CMU_TM1);

/*
 * The clocksource's timer gates are deliberately absent: the timer block
 * starts from time_init(), nothing would ever hold a reference to them, and
 * the core turns off every gate that no driver has claimed.  Timer 1 is the
 * panel's contrast PWM and is a gate precisely because a driver holds it.
 */
static struct c33_gate * const c33_gates[] = {
	&c33_spi_gate,
	&c33_dma_gate,
	&c33_efsio_gate,
	&c33_tm1_gate,
};

static const struct {
	struct c33_gate *gate;
	const char *con_id;
	const char *dev_id;
} c33_clock_consumers[] = {
	{ &c33_efsio_gate, NULL,  "s1c33-uart.0" },
	{ &c33_efsio_gate, NULL,  "s1c33-uart.1" },
	{ &c33_spi_gate,   NULL,  "s1c33-spi" },
	{ &c33_dma_gate,   "dma", "s1c33-spi" },
	{ &c33_tm1_gate,   NULL,  "s1c33-pwm" },
};

static int __init c33_clock_init(void)
{
	struct clk_lookup *lookups[ARRAY_SIZE(c33_clock_consumers)];
	unsigned int registered;
	unsigned int i;
	int ret;

	ret = clk_hw_register(NULL, &c33_mclk_hw);
	if (ret)
		return ret;
	for (registered = 0; registered < ARRAY_SIZE(c33_gates); registered++) {
		ret = clk_hw_register(NULL, &c33_gates[registered]->hw);
		if (ret)
			goto err_drop_gates;
	}
	for (i = 0; i < ARRAY_SIZE(c33_clock_consumers); i++) {
		lookups[i] = clkdev_hw_create(&c33_clock_consumers[i].gate->hw,
					      c33_clock_consumers[i].con_id,
					      "%s",
					      c33_clock_consumers[i].dev_id);
		if (!lookups[i]) {
			ret = -ENOMEM;
			goto err_drop_lookups;
		}
	}
	pr_info("C33 clock: registered %lu Hz MCLK and %zu peripheral gates\n",
		c33_mclk_hz(), ARRAY_SIZE(c33_gates));
	return 0;

err_drop_lookups:
	while (i)
		clkdev_drop(lookups[--i]);
err_drop_gates:
	while (registered)
		clk_hw_unregister(&c33_gates[--registered]->hw);
	clk_hw_unregister(&c33_mclk_hw);
	return ret;
}
postcore_initcall(c33_clock_init);
