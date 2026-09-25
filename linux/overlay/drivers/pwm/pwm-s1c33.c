// SPDX-License-Identifier: GPL-2.0-only
/*
 * One Epson S1C33 16-bit timer channel as a PWM output.
 *
 * In PWM mode the channel counts from 0 to comparison B, drives its TMx pin
 * high on the comparison A match and low again on the B match, where it also
 * reloads.  A period is therefore B + 1 ticks and the high time B - A ticks.
 * The WikiReader's panel contrast is channel 1 in this mode: the firmware
 * runs it at MCLK / 4096 and leaves it running, so the driver adopts what it
 * finds rather than restarting it.
 *
 * The channel's ADVMODE and PAUSE registers are shared with the other five
 * channels and belong to the clocksource driver; this driver never touches
 * them, and updates that only move the duty write comparison A alone so the
 * output keeps its phase.
 */
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

#define S1C33_T16_CRA		0x0
#define S1C33_T16_CRB		0x2
#define S1C33_T16_CTL		0x6

#define S1C33_CTL_PRUN		BIT(0)
#define S1C33_CTL_PRESET	BIT(1)
#define S1C33_CTL_PTM		BIT(2)
#define S1C33_CTL_OUTINV	BIT(4)

#define S1C33_CLKCTL_ON		BIT(3)
#define S1C33_CLKCTL_DIV_MASK	0x7

#define S1C33_T16_TICKS_MAX	0x10000

/* P16TS field values 0..7 select these prescaler dividers. */
static const unsigned int s1c33_pwm_dividers[] = {
	1, 2, 4, 16, 64, 256, 1024, 4096,
};

struct s1c33_pwm {
	void __iomem *timer;
	void __iomem *clkctl;
	struct clk *clk;
};

static inline struct s1c33_pwm *to_s1c33_pwm(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static u64 s1c33_pwm_ticks(u64 ns, unsigned long rate, unsigned int divider)
{
	return mul_u64_u64_div_u64(ns, rate, (u64)NSEC_PER_SEC * divider);
}

static u64 s1c33_pwm_ns(u64 ticks, unsigned long rate, unsigned int divider)
{
	return DIV_ROUND_UP_ULL(ticks * NSEC_PER_SEC * divider, rate);
}

static int s1c33_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
			       struct pwm_state *state)
{
	struct s1c33_pwm *priv = to_s1c33_pwm(chip);
	unsigned long rate = clk_get_rate(priv->clk);
	u16 ctl = readw(priv->timer + S1C33_T16_CTL);
	u16 clkctl = readw(priv->clkctl);
	u16 cra = readw(priv->timer + S1C33_T16_CRA);
	u16 crb = readw(priv->timer + S1C33_T16_CRB);
	unsigned int divider = s1c33_pwm_dividers[clkctl & S1C33_CLKCTL_DIV_MASK];
	u64 high;

	state->enabled = (ctl & S1C33_CTL_PRUN) && (ctl & S1C33_CTL_PTM) &&
			 (clkctl & S1C33_CLKCTL_ON);
	state->polarity = (ctl & S1C33_CTL_OUTINV) ? PWM_POLARITY_INVERSED :
						     PWM_POLARITY_NORMAL;
	/* With OUTINV the edges swap: high from the reload until match A. */
	if (ctl & S1C33_CTL_OUTINV)
		high = min_t(u64, cra, crb);
	else
		high = cra < crb ? crb - cra : 0;
	state->period = s1c33_pwm_ns((u64)crb + 1, rate, divider);
	state->duty_cycle = s1c33_pwm_ns(high, rate, divider);
	return 0;
}

static int s1c33_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			   const struct pwm_state *state)
{
	struct s1c33_pwm *priv = to_s1c33_pwm(chip);
	unsigned long rate = clk_get_rate(priv->clk);
	unsigned int select;
	u64 ticks, high;
	u16 clkctl, ctl;

	if (state->polarity != PWM_POLARITY_NORMAL)
		return -EINVAL;

	if (!state->enabled) {
		/* Stop and return the output to its initial, low level. */
		writew(0, priv->timer + S1C33_T16_CTL);
		writew(S1C33_CTL_PRESET, priv->timer + S1C33_T16_CTL);
		return 0;
	}

	for (select = 0; select < ARRAY_SIZE(s1c33_pwm_dividers); select++) {
		ticks = s1c33_pwm_ticks(state->period, rate,
					s1c33_pwm_dividers[select]);
		if (ticks <= S1C33_T16_TICKS_MAX)
			break;
	}
	if (select == ARRAY_SIZE(s1c33_pwm_dividers))
		ticks = S1C33_T16_TICKS_MAX, select--;
	if (ticks < 2)
		return -EINVAL;
	high = s1c33_pwm_ticks(min(state->duty_cycle, state->period), rate,
			       s1c33_pwm_dividers[select]);
	/* Comparison A cannot precede the reload, so 100 % is one tick short. */
	if (high >= ticks)
		high = ticks - 1;

	clkctl = readw(priv->clkctl);
	ctl = readw(priv->timer + S1C33_T16_CTL);
	if ((ctl & (S1C33_CTL_PRUN | S1C33_CTL_PTM | S1C33_CTL_OUTINV)) ==
	    (S1C33_CTL_PRUN | S1C33_CTL_PTM) &&
	    (clkctl & (S1C33_CLKCTL_ON | S1C33_CLKCTL_DIV_MASK)) ==
	    (S1C33_CLKCTL_ON | select) &&
	    readw(priv->timer + S1C33_T16_CRB) == ticks - 1) {
		/* Same period and clock: move the duty without a restart. */
		writew(ticks - 1 - high, priv->timer + S1C33_T16_CRA);
		return 0;
	}

	writew(0, priv->timer + S1C33_T16_CTL);
	writew(S1C33_CLKCTL_ON | select, priv->clkctl);
	writew(ticks - 1, priv->timer + S1C33_T16_CRB);
	writew(ticks - 1 - high, priv->timer + S1C33_T16_CRA);
	writew(S1C33_CTL_PTM | S1C33_CTL_PRESET, priv->timer + S1C33_T16_CTL);
	writew(S1C33_CTL_PTM | S1C33_CTL_PRUN, priv->timer + S1C33_T16_CTL);
	return 0;
}

static const struct pwm_ops s1c33_pwm_ops = {
	.apply = s1c33_pwm_apply,
	.get_state = s1c33_pwm_get_state,
};

static int s1c33_pwm_probe(struct platform_device *pdev)
{
	struct pwm_chip *chip;
	struct s1c33_pwm *priv;

	chip = devm_pwmchip_alloc(&pdev->dev, 1, sizeof(*priv));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	priv = pwmchip_get_drvdata(chip);

	priv->timer = devm_platform_ioremap_resource_byname(pdev, "timer");
	if (IS_ERR(priv->timer))
		return PTR_ERR(priv->timer);
	priv->clkctl = devm_platform_ioremap_resource_byname(pdev, "clock");
	if (IS_ERR(priv->clkctl))
		return PTR_ERR(priv->clkctl);
	/* The channel's CMU gate: holding it is what keeps the panel lit. */
	priv->clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(priv->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->clk),
				     "cannot get the timer clock\n");

	chip->ops = &s1c33_pwm_ops;
	return devm_pwmchip_add(&pdev->dev, chip);
}

static struct platform_driver s1c33_pwm_driver = {
	.driver.name = "s1c33-pwm",
	.probe = s1c33_pwm_probe,
};
module_platform_driver(s1c33_pwm_driver);

MODULE_DESCRIPTION("Epson S1C33 16-bit timer PWM");
MODULE_LICENSE("GPL");
