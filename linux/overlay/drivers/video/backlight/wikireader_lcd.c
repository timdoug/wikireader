// SPDX-License-Identifier: GPL-2.0-only
/*
 * WikiReader panel contrast.
 *
 * The panel's bias comes from a PWM: a longer high time makes the display
 * lighter.  The firmware expresses contrast as a 12-bit number that is the
 * PWM's comparison A value, so 0 is lightest and 4095 darkest, and it leaves
 * the channel running when it hands over.  This exposes the same number as
 * /sys/class/lcd/wikireader/contrast and adopts whatever is programmed when
 * it probes, so a boot changes nothing on the panel.
 */
#include <linux/lcd.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

#define WR_LCD_CONTRAST_MAX	4095
#define WR_LCD_CONTRAST_DEFAULT	2048

struct wr_lcd {
	struct pwm_device *pwm;
	int contrast;
};

static int wr_lcd_apply(struct wr_lcd *lcd, int contrast)
{
	struct pwm_state state;
	int ret;

	pwm_get_state(lcd->pwm, &state);
	/*
	 * Round up so the driver's own rounding down lands on the exact
	 * tick: the period it reports is itself rounded up from whole ticks.
	 */
	state.duty_cycle = DIV_ROUND_UP_ULL(state.period *
					    (WR_LCD_CONTRAST_MAX - contrast),
					    WR_LCD_CONTRAST_MAX + 1);
	state.enabled = true;
	ret = pwm_apply_might_sleep(lcd->pwm, &state);
	if (ret)
		return ret;
	lcd->contrast = contrast;
	return 0;
}

static int wr_lcd_get_contrast(struct lcd_device *device)
{
	struct wr_lcd *lcd = lcd_get_data(device);

	return lcd->contrast;
}

static int wr_lcd_set_contrast(struct lcd_device *device, int contrast)
{
	struct wr_lcd *lcd = lcd_get_data(device);

	return wr_lcd_apply(lcd, clamp(contrast, 0, WR_LCD_CONTRAST_MAX));
}

static const struct lcd_ops wr_lcd_ops = {
	.get_contrast = wr_lcd_get_contrast,
	.set_contrast = wr_lcd_set_contrast,
};

static int wr_lcd_probe(struct platform_device *pdev)
{
	struct lcd_device *device;
	struct pwm_state state;
	struct wr_lcd *lcd;
	int contrast;
	int ret;

	lcd = devm_kzalloc(&pdev->dev, sizeof(*lcd), GFP_KERNEL);
	if (!lcd)
		return -ENOMEM;
	lcd->pwm = devm_pwm_get(&pdev->dev, NULL);
	if (IS_ERR(lcd->pwm))
		return dev_err_probe(&pdev->dev, PTR_ERR(lcd->pwm),
				     "cannot get the contrast PWM\n");

	pwm_get_state(lcd->pwm, &state);
	if (state.enabled && state.period) {
		u64 high = state.duty_cycle * (WR_LCD_CONTRAST_MAX + 1);

		contrast = WR_LCD_CONTRAST_MAX -
			   DIV_ROUND_CLOSEST_ULL(high, state.period);
		contrast = clamp(contrast, 0, WR_LCD_CONTRAST_MAX);
	} else {
		/* Nothing set it up: start from the firmware's default. */
		pwm_init_state(lcd->pwm, &state);
		ret = pwm_apply_might_sleep(lcd->pwm, &state);
		if (ret)
			return ret;
		contrast = WR_LCD_CONTRAST_DEFAULT;
	}
	ret = wr_lcd_apply(lcd, contrast);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot program the contrast\n");

	device = devm_lcd_device_register(&pdev->dev, "wikireader", &pdev->dev,
					  lcd, &wr_lcd_ops);
	if (IS_ERR(device))
		return PTR_ERR(device);
	device->props.max_contrast = WR_LCD_CONTRAST_MAX;
	platform_set_drvdata(pdev, lcd);
	dev_info(&pdev->dev, "contrast %d of %d adopted from the PWM\n",
		 contrast, WR_LCD_CONTRAST_MAX);
	return 0;
}

static struct platform_driver wr_lcd_driver = {
	.driver.name = "wikireader-lcd",
	.probe = wr_lcd_probe,
};
module_platform_driver(wr_lcd_driver);

MODULE_DESCRIPTION("WikiReader panel contrast");
MODULE_LICENSE("GPL");
