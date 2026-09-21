// SPDX-License-Identifier: GPL-2.0-only
/* UART-attached touchscreen used by the Openmoko WikiReader. */
#include <linux/bitops.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <linux/platform_data/wikireader-touch.h>

#include <asm/wikireader.h>

#define WR_TOUCH_TXD          0x00
#define WR_TOUCH_RXD          0x01
#define WR_TOUCH_STATUS       0x02
#define WR_TOUCH_CTL          0x03
#define WR_TOUCH_IRDA         0x04
#define WR_TOUCH_BRTRUN       0x05
#define WR_TOUCH_BRTRDL       0x06
#define WR_TOUCH_BRTRDM       0x07

#define WR_TOUCH_RX_READY     BIT(0)
#define WR_TOUCH_ERRORS       0x1c
#define WR_TOUCH_BAUD         9600UL
#define WR_TOUCH_PACKET_START 0xaa
#define WR_TOUCH_WIDTH        240
#define WR_TOUCH_HEIGHT       208

/* Temporary compatibility keyboard; userspace will replace this policy. */
#define WR_KEYBOARD_Y         120
#define WR_KEY_WIDTH          24

struct wr_touch {
	void __iomem *base;
	struct input_dev *input;
	u8 state;
	u16 x;
	u16 y;
	int active_key;
	bool pressed;
};

static u8 wr_touch_read(struct wr_touch *touch, unsigned int reg)
{
	return readb(touch->base + reg);
}

static void wr_touch_write(struct wr_touch *touch, unsigned int reg, u8 value)
{
	writeb(value, touch->base + reg);
}

static int wr_touch_hit_key(unsigned int x, unsigned int y)
{
	unsigned int row;

	if (x >= WR_TOUCH_WIDTH || y < WR_KEYBOARD_Y || y >= WR_TOUCH_HEIGHT)
		return -1;
	row = (y - WR_KEYBOARD_Y) * 3 / (WR_TOUCH_HEIGHT - WR_KEYBOARD_Y);
	return row * 10 + x / WR_KEY_WIDTH;
}

static u8 wr_touch_key_character(int key)
{
	static const u8 keys[3][10] = {
		"qwertyuiop",
		"asdfghjkl\b",
		"zxcv  bnm\n",
	};

	return keys[key / 10][key % 10];
}

static void wr_touch_report(struct wr_touch *touch, bool pressed, bool valid)
{
	unsigned int x = touch->x >> 1;
	unsigned int y = touch->y >> 1;
	int key = valid ? wr_touch_hit_key(x, y) : -1;
	int previous = touch->active_key;

	if (valid) {
		input_report_abs(touch->input, ABS_X, x);
		input_report_abs(touch->input, ABS_Y, y);
	}
	if (valid || (!pressed && touch->pressed)) {
		input_report_key(touch->input, BTN_TOUCH, pressed);
		input_sync(touch->input);
		dev_info_once(&touch->input->dev,
			      "reported absolute touch events through evdev\n");
	}

	if (pressed && !touch->pressed) {
		touch->active_key = key;
		if (key >= 0)
			c33_lcd_keyboard_press(key, true);
	} else if (pressed && touch->pressed && key != touch->active_key) {
		touch->active_key = -1;
		if (previous >= 0)
			c33_lcd_keyboard_press(previous, false);
	} else if (!pressed && touch->pressed) {
		touch->active_key = -1;
		if (previous >= 0)
			c33_lcd_keyboard_press(previous, false);
		if (valid && key == previous && previous >= 0 &&
		    c33_tty_inject_char(wr_touch_key_character(previous))) {
			c33_lcd_checkpoint(10);
			pr_info_once("C33 touch: compatibility keyboard injected console input\n");
		}
	}
	touch->pressed = pressed;
}

static void wr_touch_byte(struct wr_touch *touch, u8 byte)
{
	if (byte == WR_TOUCH_PACKET_START) {
		touch->state = 1;
		c33_lcd_checkpoint(8);
		return;
	}
	if ((byte & 0x80) && byte != 0xff) {
		touch->state = 0;
		return;
	}

	switch (touch->state++) {
	case 1:
		if (byte != 0xff)
			touch->x = (touch->x & 0x7f) | ((u16)byte << 7);
		break;
	case 2:
		if (byte != 0xff)
			touch->x = (touch->x & 0x3f80) | byte;
		break;
	case 3:
		if (byte != 0xff)
			touch->y = (touch->y & 0x7f) | ((u16)byte << 7);
		break;
	case 4:
		if (byte != 0xff)
			touch->y = (touch->y & 0x3f80) | byte;
		break;
	case 5:
		if (byte <= 1) {
			c33_lcd_checkpoint(9);
			wr_touch_report(touch, byte, true);
		}
		touch->state = 0;
		break;
	default:
		touch->state = 0;
		break;
	}
}

static irqreturn_t wr_touch_interrupt(int irq, void *data)
{
	struct wr_touch *touch = data;
	u8 status = wr_touch_read(touch, WR_TOUCH_STATUS);
	int limit = 16;

	c33_lcd_checkpoint(7);
	if (status & WR_TOUCH_ERRORS) {
		c33_lcd_checkpoint(11);
		touch->state = 0;
		while ((wr_touch_read(touch, WR_TOUCH_STATUS) & WR_TOUCH_RX_READY) &&
		       limit--)
			wr_touch_read(touch, WR_TOUCH_RXD);
		if (touch->pressed)
			wr_touch_report(touch, false, false);
	} else {
		while ((wr_touch_read(touch, WR_TOUCH_STATUS) & WR_TOUCH_RX_READY) &&
		       limit--)
			wr_touch_byte(touch,
				      wr_touch_read(touch, WR_TOUCH_RXD));
	}
	wr_touch_write(touch, WR_TOUCH_STATUS, 0);
	return IRQ_HANDLED;
}

static int wr_touch_probe(struct platform_device *pdev)
{
	const struct wikireader_touch_platform_data *pdata =
		dev_get_platdata(&pdev->dev);
	struct wr_touch *touch;
	struct input_dev *input;
	unsigned long divisor;
	int error_irq;
	int limit = 8;
	int ret;
	int rx_irq;

	if (!pdata || !pdata->clock_rate)
		return -EINVAL;
	touch = devm_kzalloc(&pdev->dev, sizeof(*touch), GFP_KERNEL);
	if (!touch)
		return -ENOMEM;
	touch->base = devm_platform_ioremap_resource_byname(pdev, "uart");
	if (IS_ERR(touch->base))
		return PTR_ERR(touch->base);
	touch->active_key = -1;

	input = devm_input_allocate_device(&pdev->dev);
	if (!input)
		return -ENOMEM;
	input->name = "WikiReader touchscreen";
	input->phys = "wikireader/input0";
	input->id.bustype = BUS_RS232;
	input_set_capability(input, EV_KEY, BTN_TOUCH);
	input_set_abs_params(input, ABS_X, 0, WR_TOUCH_WIDTH - 1, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, WR_TOUCH_HEIGHT - 1, 0, 0);
	__set_bit(INPUT_PROP_DIRECT, input->propbit);
	touch->input = input;
	platform_set_drvdata(pdev, touch);

	divisor = (pdata->clock_rate + WR_TOUCH_BAUD * 8) /
		(WR_TOUCH_BAUD * 16) - 1;
	wr_touch_write(touch, WR_TOUCH_CTL, 0x4b);
	wr_touch_write(touch, WR_TOUCH_IRDA, 0x10);
	wr_touch_write(touch, WR_TOUCH_BRTRUN, 0);
	wr_touch_write(touch, WR_TOUCH_BRTRDM, divisor >> 8);
	wr_touch_write(touch, WR_TOUCH_BRTRDL, divisor);
	wr_touch_write(touch, WR_TOUCH_BRTRUN, 1);
	while ((wr_touch_read(touch, WR_TOUCH_STATUS) & WR_TOUCH_RX_READY) &&
	       limit--)
		wr_touch_read(touch, WR_TOUCH_RXD);
	wr_touch_write(touch, WR_TOUCH_STATUS, 0);

	ret = input_register_device(input);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot register input device\n");
	error_irq = platform_get_irq_byname(pdev, "error");
	if (error_irq < 0)
		return error_irq;
	rx_irq = platform_get_irq_byname(pdev, "rx");
	if (rx_irq < 0)
		return rx_irq;
	ret = devm_request_irq(&pdev->dev, error_irq, wr_touch_interrupt, 0,
			       "wikireader-touch-error", touch);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot request error IRQ\n");
	ret = devm_request_irq(&pdev->dev, rx_irq, wr_touch_interrupt, 0,
			       "wikireader-touch-rx", touch);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot request receive IRQ\n");

	c33_lcd_keyboard_init();
	dev_info(&pdev->dev,
		 "registered 240x208 absolute touchscreen input device\n");
	return 0;
}

static struct platform_driver wr_touch_driver = {
	.probe = wr_touch_probe,
	.driver.name = "wikireader-touch",
};
module_platform_driver(wr_touch_driver);

MODULE_DESCRIPTION("Openmoko WikiReader touchscreen");
MODULE_LICENSE("GPL");
