// SPDX-License-Identifier: GPL-2.0-only
/*
 * The Epson S1C33E07 watchdog: a 30-bit up-counter on MCLK that resets the
 * chip when it reaches the comparison register.  At 60 MHz that is at most
 * 17.9 s; the watchdog core pings on the hardware's behalf for anything
 * longer.
 *
 * The enable and comparison registers are write-protected by a key in their
 * own register.  Only the reset output is used: the NMI output, pin
 * #WDT_NMI, is P63, which the WikiReader wires to its power switch logic.
 *
 * The counter keeps running while the CPU halts, and suspend-to-idle is a
 * halt, so a running watchdog is stopped across a suspend and restarted on
 * resume.
 */
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/watchdog.h>

#define S1C33_WD_WP		0x0
#define S1C33_WD_EN		0x2
#define S1C33_WD_COMP_LOW	0x4
#define S1C33_WD_COMP_HIGH	0x6
#define S1C33_WD_CNTL		0xc

#define S1C33_WP_UNLOCK		0x96
#define S1C33_WP_LOCK		0x00

#define S1C33_EN_RUNSTP		BIT(4)
#define S1C33_EN_RESEN		BIT(0)

#define S1C33_CNTL_WDRESEN	BIT(0)

#define S1C33_COMP_MAX		GENMASK(29, 0)
/* The manual forbids comparison values of 0x1f and below. */
#define S1C33_COMP_MIN		0x20

#define S1C33_WDT_DEFAULT_TIMEOUT 30

struct s1c33_wdt {
	struct watchdog_device wdd;
	void __iomem *base;
	struct clk *clk;
	unsigned long rate;
	/* The gate is held exactly while the counter runs. */
	bool counting;
};

static inline struct s1c33_wdt *to_s1c33_wdt(struct watchdog_device *wdd)
{
	return watchdog_get_drvdata(wdd);
}

/* Program the counter; the caller has lifted the write protection. */
static void s1c33_wdt_arm(struct s1c33_wdt *wdt, u32 compare)
{
	writew(0, wdt->base + S1C33_WD_EN);
	writew(compare & 0xffff, wdt->base + S1C33_WD_COMP_LOW);
	writew(compare >> 16, wdt->base + S1C33_WD_COMP_HIGH);
	/* A cleared counter first: arming over a stale count can fire. */
	writew(S1C33_CNTL_WDRESEN, wdt->base + S1C33_WD_CNTL);
	writew(S1C33_EN_RUNSTP | S1C33_EN_RESEN, wdt->base + S1C33_WD_EN);
}

static u32 s1c33_wdt_compare(struct s1c33_wdt *wdt, unsigned int seconds)
{
	u64 ticks = (u64)wdt->rate * seconds;

	return clamp_t(u64, ticks, S1C33_COMP_MIN + 1, S1C33_COMP_MAX + 1) - 1;
}

static int s1c33_wdt_start(struct watchdog_device *wdd)
{
	struct s1c33_wdt *wdt = to_s1c33_wdt(wdd);
	unsigned int hw_timeout = min(wdd->timeout,
				      wdd->max_hw_heartbeat_ms / 1000);
	int ret;

	if (!wdt->counting) {
		ret = clk_enable(wdt->clk);
		if (ret)
			return ret;
		wdt->counting = true;
	}
	writew(S1C33_WP_UNLOCK, wdt->base + S1C33_WD_WP);
	s1c33_wdt_arm(wdt, s1c33_wdt_compare(wdt, hw_timeout));
	writew(S1C33_WP_LOCK, wdt->base + S1C33_WD_WP);
	return 0;
}

static int s1c33_wdt_stop(struct watchdog_device *wdd)
{
	struct s1c33_wdt *wdt = to_s1c33_wdt(wdd);

	writew(S1C33_WP_UNLOCK, wdt->base + S1C33_WD_WP);
	writew(0, wdt->base + S1C33_WD_EN);
	writew(S1C33_WP_LOCK, wdt->base + S1C33_WD_WP);
	if (wdt->counting) {
		clk_disable(wdt->clk);
		wdt->counting = false;
	}
	return 0;
}

static int s1c33_wdt_ping(struct watchdog_device *wdd)
{
	struct s1c33_wdt *wdt = to_s1c33_wdt(wdd);

	writew(S1C33_CNTL_WDRESEN, wdt->base + S1C33_WD_CNTL);
	return 0;
}

static int s1c33_wdt_set_timeout(struct watchdog_device *wdd,
				 unsigned int timeout)
{
	wdd->timeout = timeout;
	if (to_s1c33_wdt(wdd)->counting)
		return s1c33_wdt_start(wdd);
	return 0;
}

/* Reset the chip now: the shortest comparison the manual allows. */
static int s1c33_wdt_restart(struct watchdog_device *wdd,
			     unsigned long action, void *data)
{
	struct s1c33_wdt *wdt = to_s1c33_wdt(wdd);

	clk_enable(wdt->clk);
	writew(S1C33_WP_UNLOCK, wdt->base + S1C33_WD_WP);
	s1c33_wdt_arm(wdt, S1C33_COMP_MIN);
	writew(S1C33_WP_LOCK, wdt->base + S1C33_WD_WP);
	mdelay(1);
	return 0;
}

static const struct watchdog_info s1c33_wdt_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE,
	.identity = "S1C33E07 watchdog",
};

static const struct watchdog_ops s1c33_wdt_ops = {
	.owner = THIS_MODULE,
	.start = s1c33_wdt_start,
	.stop = s1c33_wdt_stop,
	.ping = s1c33_wdt_ping,
	.set_timeout = s1c33_wdt_set_timeout,
	.restart = s1c33_wdt_restart,
};

static int s1c33_wdt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s1c33_wdt *wdt;
	int ret;

	wdt = devm_kzalloc(dev, sizeof(*wdt), GFP_KERNEL);
	if (!wdt)
		return -ENOMEM;
	wdt->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(wdt->base))
		return PTR_ERR(wdt->base);
	wdt->clk = devm_clk_get_prepared(dev, NULL);
	if (IS_ERR(wdt->clk))
		return dev_err_probe(dev, PTR_ERR(wdt->clk),
				     "cannot get the watchdog clock\n");
	wdt->rate = clk_get_rate(wdt->clk);
	if (!wdt->rate)
		return dev_err_probe(dev, -EINVAL, "watchdog clock has no rate\n");

	wdt->wdd.parent = dev;
	wdt->wdd.info = &s1c33_wdt_info;
	wdt->wdd.ops = &s1c33_wdt_ops;
	wdt->wdd.min_timeout = 1;
	wdt->wdd.max_hw_heartbeat_ms =
		div_u64((u64)(S1C33_COMP_MAX + 1) * MSEC_PER_SEC, wdt->rate);
	wdt->wdd.timeout = S1C33_WDT_DEFAULT_TIMEOUT;
	watchdog_init_timeout(&wdt->wdd, 0, dev);
	watchdog_set_drvdata(&wdt->wdd, wdt);
	watchdog_set_restart_priority(&wdt->wdd, 128);
	watchdog_stop_on_reboot(&wdt->wdd);
	watchdog_stop_on_unregister(&wdt->wdd);
	platform_set_drvdata(pdev, wdt);

	/* c33_start() stopped the launcher's watchdog: off until opened. */
	ret = devm_watchdog_register_device(dev, &wdt->wdd);
	if (ret)
		return ret;
	dev_info(dev, "%lu Hz, up to %u ms a period\n", wdt->rate,
		 wdt->wdd.max_hw_heartbeat_ms);
	return 0;
}

static int s1c33_wdt_suspend(struct device *dev)
{
	struct s1c33_wdt *wdt = dev_get_drvdata(dev);

	if (watchdog_active(&wdt->wdd))
		return s1c33_wdt_stop(&wdt->wdd);
	return 0;
}

static int s1c33_wdt_resume(struct device *dev)
{
	struct s1c33_wdt *wdt = dev_get_drvdata(dev);

	if (watchdog_active(&wdt->wdd))
		return s1c33_wdt_start(&wdt->wdd);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(s1c33_wdt_pm, s1c33_wdt_suspend,
				s1c33_wdt_resume);

static struct platform_driver s1c33_wdt_driver = {
	.driver = {
		.name = "s1c33-wdt",
		.pm = pm_sleep_ptr(&s1c33_wdt_pm),
	},
	.probe = s1c33_wdt_probe,
};
module_platform_driver(s1c33_wdt_driver);

MODULE_DESCRIPTION("Epson S1C33E07 watchdog timer");
MODULE_LICENSE("GPL");
