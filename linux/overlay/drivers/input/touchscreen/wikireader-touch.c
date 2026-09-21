// SPDX-License-Identifier: GPL-2.0-only
/* UART-attached touchscreen used by the Openmoko WikiReader. */
#include <linux/bitops.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/serdev.h>
#include <linux/serial_core.h>

#include <asm/wikireader.h>

#define WR_TOUCH_BAUD         9600
#define WR_TOUCH_PACKET_START 0xaa
#define WR_TOUCH_WIDTH        240
#define WR_TOUCH_HEIGHT       208

/* Temporary compatibility keyboard; userspace will replace this policy. */
#define WR_KEYBOARD_Y         120
#define WR_KEY_WIDTH          24

struct wr_touch {
	struct input_dev *input;
	u8 state;
	u16 x;
	u16 y;
	int active_key;
	bool pressed;
};

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

static size_t wr_touch_receive_buf(struct serdev_device *serdev,
				   const u8 *data, size_t count)
{
	struct wr_touch *touch = serdev_device_get_drvdata(serdev);
	size_t i;

	for (i = 0; i < count; i++)
		wr_touch_byte(touch, data[i]);
	return count;
}

static const struct serdev_device_ops wr_touch_serdev_ops = {
	.receive_buf = wr_touch_receive_buf,
};

static int wr_touch_serdev_probe(struct serdev_device *serdev)
{
	struct wr_touch *touch;
	struct input_dev *input;
	int ret;

	touch = devm_kzalloc(&serdev->dev, sizeof(*touch), GFP_KERNEL);
	if (!touch)
		return -ENOMEM;
	touch->active_key = -1;

	input = devm_input_allocate_device(&serdev->dev);
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
	serdev_device_set_drvdata(serdev, touch);
	serdev_device_set_client_ops(serdev, &wr_touch_serdev_ops);

	ret = input_register_device(input);
	if (ret)
		return dev_err_probe(&serdev->dev, ret,
				     "cannot register input device\n");
	ret = devm_serdev_device_open(&serdev->dev, serdev);
	if (ret)
		return dev_err_probe(&serdev->dev, ret,
				     "cannot open UART transport\n");
	if (serdev_device_set_baudrate(serdev, WR_TOUCH_BAUD) !=
	    WR_TOUCH_BAUD)
		return dev_err_probe(&serdev->dev, -EINVAL,
				     "cannot set 9600 baud\n");
	serdev_device_set_flow_control(serdev, false);

	c33_lcd_keyboard_init();
	dev_info(&serdev->dev,
		 "registered 240x208 touchscreen through serdev\n");
	return 0;
}

static struct serdev_device_driver wr_touch_serdev_driver = {
	.probe = wr_touch_serdev_probe,
	.driver.name = "wikireader-touch-serdev",
};

/*
 * Legacy board files have no DT or ACPI child to enumerate. Instantiate the
 * serdev child explicitly, as the in-tree x86 legacy-board quirks do.
 */
static int wr_touch_platform_probe(struct platform_device *pdev)
{
	struct uart_port *port = dev_get_drvdata(pdev->dev.parent);
	struct serdev_controller *controller;
	struct serdev_device *serdev;
	int ret;

	if (!port || !port->state || !port->state->port.client_data)
		return -EPROBE_DEFER;
	controller = port->state->port.client_data;
	serdev = serdev_device_alloc(controller);
	if (!serdev)
		return -ENOMEM;
	ret = serdev_device_add(serdev);
	if (ret) {
		serdev_device_put(serdev);
		return dev_err_probe(&pdev->dev, ret,
				     "cannot add serdev child\n");
	}
	ret = device_driver_attach(&wr_touch_serdev_driver.driver,
				   &serdev->dev);
	if (ret) {
		serdev_device_remove(serdev);
		return dev_err_probe(&pdev->dev,
				     ret == -EAGAIN ? -EPROBE_DEFER : ret,
				     "cannot attach serdev driver\n");
	}
	platform_set_drvdata(pdev, serdev);
	return 0;
}

static void wr_touch_platform_remove(struct platform_device *pdev)
{
	serdev_device_remove(platform_get_drvdata(pdev));
}

static struct platform_driver wr_touch_driver = {
	.probe = wr_touch_platform_probe,
	.remove = wr_touch_platform_remove,
	.driver.name = "wikireader-touch",
};

static int __init wr_touch_init(void)
{
	int ret;

	ret = serdev_device_driver_register(&wr_touch_serdev_driver);
	if (ret)
		return ret;
	ret = platform_driver_register(&wr_touch_driver);
	if (ret)
		serdev_device_driver_unregister(&wr_touch_serdev_driver);
	return ret;
}
module_init(wr_touch_init);

static void __exit wr_touch_exit(void)
{
	platform_driver_unregister(&wr_touch_driver);
	serdev_device_driver_unregister(&wr_touch_serdev_driver);
}
module_exit(wr_touch_exit);

MODULE_DESCRIPTION("Openmoko WikiReader touchscreen");
MODULE_LICENSE("GPL");
