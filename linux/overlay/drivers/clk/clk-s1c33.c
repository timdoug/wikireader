// SPDX-License-Identifier: GPL-2.0
/*
 * The Epson S1C33E07 clock-management unit: MCLK, from the high-speed
 * oscillator (OSC3) divided, from the low-speed one (OSC1), or from the PLL
 * on OSC3, and the peripheral gates in GATEDCLK1.  The oscillators are the
 * board's, inputs named "osc3" and "osc1".  MCLK is chosen before the
 * kernel starts and is only read here.
 *
 * Registered with CLK_OF_DECLARE, from time_init(): the timer block that
 * is the clocksource takes MCLK's rate and its gates from here before any
 * device probes.
 */
#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <dt-bindings/clock/s1c33-cmu.h>

#define CMU_GATE1	0x04
#define CMU_CLKCNTL	0x08
#define CMU_PLL		0x0c
#define CMU_PROTECT	0x24
#define CMU_UNLOCK	0x96

/* CLKCNTL: the source, OSC3's divider, halving, the PLL's input divider. */
#define CLKCNTL_SOURCE(x)	(((x) >> 2) & 3)
#define CLKCNTL_SOURCE_OSC1	1
#define CLKCNTL_SOURCE_PLL	3
#define CLKCNTL_OSC3DIV(x)	(((x) >> 8) & 7)
#define CLKCNTL_HALF		BIT(12)
#define CLKCNTL_PLLINDIV(x)	(((x) >> 20) & 15)
#define PLL_MULTIPLIER(x)	((((x) >> 4) & 15) + 1)

#define S1C33_CLK_COUNT		(S1C33_CLK_TM5 + 1)

#define MCLK_PARENT_OSC3	0
#define MCLK_PARENT_OSC1	1

static void __iomem *cmu_base;
/* Every CMU register refuses writes unless PROTECT is holding the key. */
static DEFINE_SPINLOCK(cmu_lock);

static u8 s1c33_mclk_get_parent(struct clk_hw *hw)
{
	u32 ctl = readl(cmu_base + CMU_CLKCNTL);

	return CLKCNTL_SOURCE(ctl) == CLKCNTL_SOURCE_OSC1 ?
		MCLK_PARENT_OSC1 : MCLK_PARENT_OSC3;
}

static unsigned long s1c33_mclk_recalc_rate(struct clk_hw *hw,
					    unsigned long parent_rate)
{
	u32 ctl = readl(cmu_base + CMU_CLKCNTL);
	unsigned long rate = parent_rate;
	unsigned int shift;

	switch (CLKCNTL_SOURCE(ctl)) {
	case CLKCNTL_SOURCE_OSC1:
		break;
	case CLKCNTL_SOURCE_PLL: {
		unsigned int input_div = CLKCNTL_PLLINDIV(ctl) + 1;

		/* Values 10 to 15 are reserved; the documented reset is /8. */
		if (input_div > 10)
			input_div = 8;
		rate = parent_rate / input_div *
			PLL_MULTIPLIER(readl(cmu_base + CMU_PLL));
		break;
	}
	default:
		/* OSC3DIV encodes /1, /2, /4, /8, /16, /32. */
		shift = CLKCNTL_OSC3DIV(ctl);
		rate = parent_rate >> (shift > 5 ? 0 : shift);
		break;
	}
	if (ctl & CLKCNTL_HALF)
		rate >>= 1;
	return rate;
}

static const struct clk_ops s1c33_mclk_ops = {
	.get_parent = s1c33_mclk_get_parent,
	.recalc_rate = s1c33_mclk_recalc_rate,
};

struct s1c33_gate {
	struct clk_hw hw;
	u32 mask;
};

static inline struct s1c33_gate *to_s1c33_gate(struct clk_hw *hw)
{
	return container_of(hw, struct s1c33_gate, hw);
}

static void s1c33_gate_set(struct clk_hw *hw, bool enable)
{
	u32 mask = to_s1c33_gate(hw)->mask;
	unsigned long flags;
	u32 gate;

	spin_lock_irqsave(&cmu_lock, flags);
	writel(CMU_UNLOCK, cmu_base + CMU_PROTECT);
	gate = readl(cmu_base + CMU_GATE1);
	writel(enable ? gate | mask : gate & ~mask, cmu_base + CMU_GATE1);
	writel(0, cmu_base + CMU_PROTECT);
	spin_unlock_irqrestore(&cmu_lock, flags);
}

static int s1c33_gate_enable(struct clk_hw *hw)
{
	s1c33_gate_set(hw, true);
	return 0;
}

static void s1c33_gate_disable(struct clk_hw *hw)
{
	s1c33_gate_set(hw, false);
}

static int s1c33_gate_is_enabled(struct clk_hw *hw)
{
	u32 mask = to_s1c33_gate(hw)->mask;

	return (readl(cmu_base + CMU_GATE1) & mask) == mask;
}

/* A gate passes MCLK's rate through: no recalc_rate, no set_rate. */
static const struct clk_ops s1c33_gate_ops = {
	.enable = s1c33_gate_enable,
	.disable = s1c33_gate_disable,
	.is_enabled = s1c33_gate_is_enabled,
};

/* GATEDCLK1's bits, by the device tree's clock numbers. */
static const struct {
	const char *name;
	u32 mask;
} s1c33_gates[] = {
	[S1C33_CLK_SPI] = { "spi", BIT(6) },
	[S1C33_CLK_HSDMA] = { "hsdma", BIT(1) },
	[S1C33_CLK_EFSIO] = { "efsio", BIT(5) | BIT(25) },
	[S1C33_CLK_TM1] = { "tm1", BIT(14) },
	[S1C33_CLK_ADC] = { "adc", BIT(3) },
	[S1C33_CLK_WDT] = { "wdt", BIT(9) },
	[S1C33_CLK_TM0] = { "tm0", BIT(13) },
	[S1C33_CLK_TM2] = { "tm2", BIT(15) },
	[S1C33_CLK_TM3] = { "tm3", BIT(16) },
	[S1C33_CLK_TM5] = { "tm5", BIT(18) },
};

static struct clk_hw *s1c33_register_mclk(struct device_node *node)
{
	static const struct clk_parent_data parents[] = {
		[MCLK_PARENT_OSC3] = { .fw_name = "osc3" },
		[MCLK_PARENT_OSC1] = { .fw_name = "osc1" },
	};
	struct clk_init_data init = {
		.name = "mclk",
		.ops = &s1c33_mclk_ops,
		.parent_data = parents,
		.num_parents = ARRAY_SIZE(parents),
	};
	struct clk_hw *hw;
	int ret;

	hw = kzalloc_obj(*hw);
	if (!hw)
		return ERR_PTR(-ENOMEM);
	hw->init = &init;
	ret = of_clk_hw_register(node, hw);
	if (ret) {
		kfree(hw);
		return ERR_PTR(ret);
	}
	return hw;
}

static void __init s1c33_cmu_init(struct device_node *node)
{
	struct clk_hw_onecell_data *clocks;
	struct clk_hw *mclk;
	unsigned int i;
	int ret;

	clocks = kzalloc(struct_size(clocks, hws, S1C33_CLK_COUNT), GFP_KERNEL);
	if (!clocks)
		return;
	clocks->num = S1C33_CLK_COUNT;
	cmu_base = of_iomap(node, 0);
	if (!cmu_base) {
		ret = -ENOMEM;
		goto err;
	}
	mclk = s1c33_register_mclk(node);
	if (IS_ERR(mclk)) {
		ret = PTR_ERR(mclk);
		goto err;
	}
	clocks->hws[S1C33_CLK_MCLK] = mclk;
	for (i = 0; i < ARRAY_SIZE(s1c33_gates); i++) {
		struct clk_init_data init = {
			.name = s1c33_gates[i].name,
			.ops = &s1c33_gate_ops,
			.parent_hws = (const struct clk_hw *[]){ mclk },
			.num_parents = 1,
		};
		struct s1c33_gate *gate;

		if (!s1c33_gates[i].name)
			continue;
		gate = kzalloc_obj(*gate);
		if (!gate) {
			ret = -ENOMEM;
			goto err;
		}
		gate->mask = s1c33_gates[i].mask;
		gate->hw.init = &init;
		ret = of_clk_hw_register(node, &gate->hw);
		if (ret) {
			kfree(gate);
			goto err;
		}
		clocks->hws[i] = &gate->hw;
	}
	ret = of_clk_add_hw_provider(node, of_clk_hw_onecell_get, clocks);
	if (ret)
		goto err;
	pr_info("s1c33-cmu: %lu Hz MCLK from %s, %zu gates\n",
		clk_hw_get_rate(mclk), clk_hw_get_name(clk_hw_get_parent(mclk)),
		ARRAY_SIZE(s1c33_gates) - 1);
	return;

err:
	/* No clocks, no clocksource: nothing to unwind into. */
	pr_err("s1c33-cmu: registration failed: %d\n", ret);
}
CLK_OF_DECLARE(s1c33_cmu, "epson,s1c33-cmu", s1c33_cmu_init);
