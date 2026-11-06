// SPDX-License-Identifier: GPL-2.0-only
/*
 * The Openmoko WikiReader's panel: 240x208, monochrome, on the S1C33E07's
 * LCD controller.
 *
 * Its display-enable line goes up once the controller drives it and down
 * before the controller stops, since a panel without it fades instead of
 * clearing: the panel bridge's enable and disable, after the controller's
 * CRTC is on and before it goes off.  The loader leaves the panel lit, so
 * the line is taken as it is.  The line is a GPIO that does not sleep, so
 * enable() is safe where nothing may sleep, and the LCD controller's panic
 * path calls it directly to light the panel for the panic screen.
 *
 * The panel's bias comes from a PWM: a longer high time makes the display
 * lighter.  The firmware expresses contrast as a 12-bit number that is the
 * PWM's comparison A value, so 0 is lightest and 4095 darkest, and it leaves
 * the channel running when it hands over.  This exposes the same number as
 * /sys/class/lcd/wikireader/contrast and adopts whatever is programmed when
 * it probes, so a boot changes nothing on the panel.
 */
#include <linux/gpio/consumer.h>
#include <linux/lcd.h>
#include <linux/math64.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

#include <drm/drm_connector.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

#define WR_LCD_WIDTH		240
#define WR_LCD_HEIGHT		208
#define WR_LCD_CONTRAST_MAX	4095
#define WR_LCD_CONTRAST_DEFAULT	2048

struct wr_panel {
	struct drm_panel panel;
	struct gpio_desc *enable;
	struct pwm_device *pwm;
	int contrast;
};

static struct wr_panel *to_wr_panel(struct drm_panel *panel)
{
	return container_of(panel, struct wr_panel, panel);
}

static const struct drm_display_mode wr_panel_mode = {
	DRM_SIMPLE_MODE(WR_LCD_WIDTH, WR_LCD_HEIGHT, 0, 0),
};

static int wr_panel_enable(struct drm_panel *panel)
{
	gpiod_set_value(to_wr_panel(panel)->enable, 1);
	return 0;
}

static int wr_panel_disable(struct drm_panel *panel)
{
	gpiod_set_value(to_wr_panel(panel)->enable, 0);
	return 0;
}

static int wr_panel_get_modes(struct drm_panel *panel,
			      struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &wr_panel_mode);
	if (!mode)
		return -ENOMEM;
	drm_mode_set_name(mode);
	mode->type |= DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);
	return 1;
}

static const struct drm_panel_funcs wr_panel_funcs = {
	.enable = wr_panel_enable,
	.disable = wr_panel_disable,
	.get_modes = wr_panel_get_modes,
};

static int wr_panel_apply(struct wr_panel *wr, int contrast)
{
	struct pwm_state state;
	int ret;

	pwm_get_state(wr->pwm, &state);
	/*
	 * Round up so the driver's own rounding down lands on the exact
	 * tick: the period it reports is itself rounded up from whole ticks.
	 */
	state.duty_cycle = DIV_ROUND_UP_ULL(state.period *
					    (WR_LCD_CONTRAST_MAX - contrast),
					    WR_LCD_CONTRAST_MAX + 1);
	state.enabled = true;
	ret = pwm_apply_might_sleep(wr->pwm, &state);
	if (ret)
		return ret;
	wr->contrast = contrast;
	return 0;
}

static int wr_panel_get_contrast(struct lcd_device *device)
{
	struct wr_panel *wr = lcd_get_data(device);

	return wr->contrast;
}

static int wr_panel_set_contrast(struct lcd_device *device, int contrast)
{
	struct wr_panel *wr = lcd_get_data(device);

	return wr_panel_apply(wr, clamp(contrast, 0, WR_LCD_CONTRAST_MAX));
}

static const struct lcd_ops wr_panel_lcd_ops = {
	.get_contrast = wr_panel_get_contrast,
	.set_contrast = wr_panel_set_contrast,
};

static int wr_panel_contrast_init(struct device *dev, struct wr_panel *wr)
{
	struct lcd_device *device;
	struct pwm_state state;
	int contrast;
	int ret;

	wr->pwm = devm_pwm_get(dev, NULL);
	if (IS_ERR(wr->pwm))
		return dev_err_probe(dev, PTR_ERR(wr->pwm),
				     "cannot get the contrast PWM\n");
	pwm_get_state(wr->pwm, &state);
	if (state.enabled && state.period) {
		u64 high = state.duty_cycle * (WR_LCD_CONTRAST_MAX + 1);

		contrast = WR_LCD_CONTRAST_MAX -
			   DIV_ROUND_CLOSEST_ULL(high, state.period);
		contrast = clamp(contrast, 0, WR_LCD_CONTRAST_MAX);
	} else {
		/* Nothing set it up: start from the firmware's default. */
		pwm_init_state(wr->pwm, &state);
		ret = pwm_apply_might_sleep(wr->pwm, &state);
		if (ret)
			return ret;
		contrast = WR_LCD_CONTRAST_DEFAULT;
	}
	ret = wr_panel_apply(wr, contrast);
	if (ret)
		return dev_err_probe(dev, ret, "cannot program the contrast\n");

	device = devm_lcd_device_register(dev, "wikireader", dev, wr,
					  &wr_panel_lcd_ops);
	if (IS_ERR(device))
		return PTR_ERR(device);
	device->props.max_contrast = WR_LCD_CONTRAST_MAX;
	dev_info(dev, "contrast %d of %d adopted from the PWM\n",
		 contrast, WR_LCD_CONTRAST_MAX);
	return 0;
}

static int wr_panel_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct wr_panel *wr;
	int ret;

	wr = devm_drm_panel_alloc(dev, struct wr_panel, panel, &wr_panel_funcs,
				  DRM_MODE_CONNECTOR_DPI);
	if (IS_ERR(wr))
		return PTR_ERR(wr);

	/* Lit by the loader: adopt the line's state rather than glitch it. */
	wr->enable = devm_gpiod_get(dev, "enable", GPIOD_ASIS);
	if (IS_ERR(wr->enable))
		return dev_err_probe(dev, PTR_ERR(wr->enable),
				     "cannot claim the display enable\n");
	if (gpiod_cansleep(wr->enable))
		return dev_err_probe(dev, -EINVAL,
				     "the display enable must not sleep\n");
	ret = gpiod_direction_output(wr->enable, gpiod_get_value(wr->enable));
	if (ret)
		return ret;

	ret = wr_panel_contrast_init(dev, wr);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, wr);
	return devm_drm_panel_add(dev, &wr->panel);
}

static const struct of_device_id wr_panel_of_match[] = {
	{ .compatible = "openmoko,wikireader-lcd" },
	{ }
};
MODULE_DEVICE_TABLE(of, wr_panel_of_match);

static struct platform_driver wr_panel_driver = {
	.driver.name = "wikireader-lcd",
	.driver.of_match_table = wr_panel_of_match,
	.probe = wr_panel_probe,
};
module_platform_driver(wr_panel_driver);

MODULE_DESCRIPTION("Openmoko WikiReader panel");
MODULE_LICENSE("GPL");
