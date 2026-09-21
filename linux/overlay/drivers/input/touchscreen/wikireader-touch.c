// SPDX-License-Identifier: GPL-2.0-only
/* UART-attached touchscreen used by the Openmoko WikiReader. */
#include <linux/input.h>
#include <linux/input/touchscreen.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/serdev.h>

#define WR_TOUCH_PACKET_START 0xaa
#define WR_TOUCH_WIDTH        240
#define WR_TOUCH_HEIGHT       208

struct wr_touch {
	struct input_dev *input;
	struct touchscreen_properties properties;
	u8 state;
	u16 x;
	u16 y;
};

static void wr_touch_report(struct wr_touch *touch, bool pressed)
{
	unsigned int x = touch->x >> 1;
	unsigned int y = touch->y >> 1;

	touchscreen_report_pos(touch->input, &touch->properties, x, y, false);
	input_report_key(touch->input, BTN_TOUCH, pressed);
	input_sync(touch->input);
}

static void wr_touch_byte(struct wr_touch *touch, u8 byte)
{
	if (byte == WR_TOUCH_PACKET_START) {
		touch->state = 1;
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
		if (byte <= 1)
			wr_touch_report(touch, byte);
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
	u32 baud;
	int ret;

	touch = devm_kzalloc(&serdev->dev, sizeof(*touch), GFP_KERNEL);
	if (!touch)
		return -ENOMEM;
	input = devm_input_allocate_device(&serdev->dev);
	if (!input)
		return -ENOMEM;
	input->name = "WikiReader touchscreen";
	input->phys = "wikireader/input0";
	input->id.bustype = BUS_RS232;
	input_set_capability(input, EV_KEY, BTN_TOUCH);
	input_set_abs_params(input, ABS_X, 0, WR_TOUCH_WIDTH - 1, 0, 0);
	input_set_abs_params(input, ABS_Y, 0, WR_TOUCH_HEIGHT - 1, 0, 0);
	touchscreen_parse_properties(input, false, &touch->properties);
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
	ret = device_property_read_u32(&serdev->dev, "current-speed", &baud);
	if (ret)
		return dev_err_probe(&serdev->dev, ret,
				     "current-speed is not specified\n");
	if (serdev_device_set_baudrate(serdev, baud) != baud)
		return dev_err_probe(&serdev->dev, -EINVAL,
				     "cannot set %u baud\n", baud);
	serdev_device_set_flow_control(serdev, false);

	dev_info(&serdev->dev,
		 "registered 240x208 touchscreen through serdev\n");
	return 0;
}

static const struct of_device_id wr_touch_of_match[] = {
	{ .compatible = "openmoko,wikireader-touchscreen" },
	{ }
};
MODULE_DEVICE_TABLE(of, wr_touch_of_match);

static struct serdev_device_driver wr_touch_driver = {
	.probe = wr_touch_serdev_probe,
	.driver = {
		.name = "wikireader-touch",
		.of_match_table = wr_touch_of_match,
	},
};
module_serdev_device_driver(wr_touch_driver);

MODULE_DESCRIPTION("Openmoko WikiReader touchscreen");
MODULE_LICENSE("GPL");
