// SPDX-License-Identifier: GPL-2.0-only
/* GPIO controller for the Epson S1C33 port block. */
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define S1C33_GPIO_PORTS	7
#define S1C33_GPIO_PER_PORT	8
#define S1C33_GPIO_DATA(port)	((port) * 2)
#define S1C33_GPIO_DIRECTION(port) (S1C33_GPIO_DATA(port) + 1)

struct s1c33_gpio {
	void __iomem *base;
	spinlock_t lock; /* Protects data and direction register RMW. */
	struct gpio_chip chip;
};

static void __iomem *s1c33_gpio_register(struct s1c33_gpio *gpio,
					 unsigned int offset, bool direction)
{
	unsigned int port = offset / S1C33_GPIO_PER_PORT;

	return gpio->base + (direction ? S1C33_GPIO_DIRECTION(port) :
					  S1C33_GPIO_DATA(port));
}

static u8 s1c33_gpio_mask(unsigned int offset)
{
	return BIT(offset % S1C33_GPIO_PER_PORT);
}

static int s1c33_gpio_get_direction(struct gpio_chip *chip,
				    unsigned int offset)
{
	struct s1c33_gpio *gpio = gpiochip_get_data(chip);

	return readb(s1c33_gpio_register(gpio, offset, true)) &
		s1c33_gpio_mask(offset) ? GPIO_LINE_DIRECTION_OUT :
		GPIO_LINE_DIRECTION_IN;
}

static int s1c33_gpio_get(struct gpio_chip *chip, unsigned int offset)
{
	struct s1c33_gpio *gpio = gpiochip_get_data(chip);

	return !!(readb(s1c33_gpio_register(gpio, offset, false)) &
		  s1c33_gpio_mask(offset));
}

static void s1c33_gpio_set_locked(struct s1c33_gpio *gpio,
				  unsigned int offset, int value)
{
	void __iomem *reg = s1c33_gpio_register(gpio, offset, false);
	u8 mask = s1c33_gpio_mask(offset);
	u8 data = readb(reg);

	writeb(value ? data | mask : data & ~mask, reg);
}

static int s1c33_gpio_set(struct gpio_chip *chip, unsigned int offset,
			  int value)
{
	struct s1c33_gpio *gpio = gpiochip_get_data(chip);
	unsigned long flags;

	spin_lock_irqsave(&gpio->lock, flags);
	s1c33_gpio_set_locked(gpio, offset, value);
	spin_unlock_irqrestore(&gpio->lock, flags);
	return 0;
}

static int s1c33_gpio_direction_input(struct gpio_chip *chip,
				      unsigned int offset)
{
	struct s1c33_gpio *gpio = gpiochip_get_data(chip);
	void __iomem *reg = s1c33_gpio_register(gpio, offset, true);
	unsigned long flags;

	spin_lock_irqsave(&gpio->lock, flags);
	writeb(readb(reg) & ~s1c33_gpio_mask(offset), reg);
	spin_unlock_irqrestore(&gpio->lock, flags);
	return 0;
}

static int s1c33_gpio_direction_output(struct gpio_chip *chip,
				       unsigned int offset, int value)
{
	struct s1c33_gpio *gpio = gpiochip_get_data(chip);
	void __iomem *reg = s1c33_gpio_register(gpio, offset, true);
	unsigned long flags;

	spin_lock_irqsave(&gpio->lock, flags);
	s1c33_gpio_set_locked(gpio, offset, value);
	writeb(readb(reg) | s1c33_gpio_mask(offset), reg);
	spin_unlock_irqrestore(&gpio->lock, flags);
	return 0;
}

static int s1c33_gpio_probe(struct platform_device *pdev)
{
	struct s1c33_gpio *gpio;
	int ret;

	gpio = devm_kzalloc(&pdev->dev, sizeof(*gpio), GFP_KERNEL);
	if (!gpio)
		return -ENOMEM;
	gpio->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(gpio->base))
		return dev_err_probe(&pdev->dev, PTR_ERR(gpio->base),
				     "cannot map port registers\n");
	spin_lock_init(&gpio->lock);
	gpio->chip.label = "s1c33-gpio";
	gpio->chip.parent = &pdev->dev;
	gpio->chip.owner = THIS_MODULE;
	gpio->chip.get_direction = s1c33_gpio_get_direction;
	gpio->chip.direction_input = s1c33_gpio_direction_input;
	gpio->chip.direction_output = s1c33_gpio_direction_output;
	gpio->chip.get = s1c33_gpio_get;
	gpio->chip.set = s1c33_gpio_set;
	gpio->chip.base = -1;
	gpio->chip.ngpio = S1C33_GPIO_PORTS * S1C33_GPIO_PER_PORT;

	ret = devm_gpiochip_add_data(&pdev->dev, &gpio->chip, gpio);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot register GPIO controller\n");
	dev_info(&pdev->dev, "registered 56 GPIOs through gpiolib\n");
	return 0;
}

static struct platform_driver s1c33_gpio_driver = {
	.probe = s1c33_gpio_probe,
	.driver.name = "s1c33-gpio",
};
/*
 * Register early: the board's fixed-voltage regulators take their enable
 * lines from this chip, and the regulator core binds them at subsys level.
 */
static int __init s1c33_gpio_init(void)
{
	return platform_driver_register(&s1c33_gpio_driver);
}
postcore_initcall(s1c33_gpio_init);

MODULE_DESCRIPTION("Epson S1C33 GPIO controller");
MODULE_LICENSE("GPL");
