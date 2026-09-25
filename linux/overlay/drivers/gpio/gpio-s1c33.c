// SPDX-License-Identifier: GPL-2.0-only
/*
 * GPIO controller for the Epson S1C33 port block.
 *
 * Two of the block's interrupt sources are wired to lines here.  Key input 0
 * compares P60..P62 with a stored pattern and interrupts on a mismatch, so
 * the handler reports every line that changed and stores the new state.
 * Port input 3 watches P03 for one edge, chosen by its polarity bit; both
 * edges are had by pointing it at the opposite of the level after each one.
 * Each source has its own ITC vector, chained here.
 */
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#define S1C33_GPIO_PORTS	7
#define S1C33_GPIO_PER_PORT	8
#define S1C33_GPIO_DATA(port)	((port) * 2)
#define S1C33_GPIO_DIRECTION(port) (S1C33_GPIO_DATA(port) + 1)

#define S1C33_GPIO_P03		(0 * S1C33_GPIO_PER_PORT + 3)
#define S1C33_GPIO_P60		(6 * S1C33_GPIO_PER_PORT + 0)
#define S1C33_GPIO_KEY_LINES	3	/* P60..P62 */
#define S1C33_GPIO_KEY_MASK	GENMASK(S1C33_GPIO_KEY_LINES - 1, 0)

/* The interrupt registers, the second memory resource. */
#define S1C33_SPT03		0x00	/* port inputs 0-3: pin select */
#define S1C33_SPT3_MASK		0xc0	/* port input 3: 0 selects P03 */
#define S1C33_SPP07		0x02	/* polarity: set is high/rising */
#define S1C33_SEPT07		0x03	/* set is edge, clear is level */
#define S1C33_PORT3		BIT(3)
#define S1C33_SPPK01		0x10	/* key inputs: port group select */
#define S1C33_SPPK0_MASK	0x07
#define S1C33_SPPK0_P60		0x04
#define S1C33_SCPK0		0x12	/* key input 0: comparison pattern */
#define S1C33_SMPK0		0x14	/* key input 0: bits compared */

/*
 * The ITC's priority nibbles for the two causes: high half of 0x261 for
 * port input 3, low half of 0x262 for key input 0.  Below the timer.
 */
#define S1C33_ITC_PRIORITY_PORT23	0x00300261UL
#define S1C33_ITC_PRIORITY_KEY01	0x00300262UL
#define S1C33_GPIO_IRQ_PRIORITY		3

struct s1c33_gpio {
	void __iomem *base;
	void __iomem *irq_base;
	spinlock_t lock; /* Protects data and direction register RMW. */
	struct gpio_chip chip;
	int key_irq;
	int port3_irq;
	/* Under lock: key input 0's last state and unmasked lines, P03's. */
	u8 key_state;
	u8 key_enabled;
	bool port3_enabled;
	u8 port3_level;
	/* Edges wanted, per line: bit 0 rising, bit 1 falling. */
	u8 key_edges[S1C33_GPIO_KEY_LINES];
	u8 port3_edges;
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

#define S1C33_EDGE_RISING	BIT(0)
#define S1C33_EDGE_FALLING	BIT(1)

static u8 s1c33_gpio_key_bits(struct s1c33_gpio *gpio)
{
	return readb(gpio->base + S1C33_GPIO_DATA(6)) & S1C33_GPIO_KEY_MASK;
}

static u8 s1c33_gpio_port3_bit(struct s1c33_gpio *gpio)
{
	return !!(readb(gpio->base + S1C33_GPIO_DATA(0)) & S1C33_PORT3);
}

static void s1c33_gpio_update8(void __iomem *reg, u8 clear, u8 set)
{
	writeb((readb(reg) & ~clear) | set, reg);
}

/* Watch P03 for the next edge wanted from the level it has now. */
static void s1c33_gpio_arm_port3(struct s1c33_gpio *gpio)
{
	bool rising;

	gpio->port3_level = s1c33_gpio_port3_bit(gpio);
	if (gpio->port3_edges == S1C33_EDGE_RISING)
		rising = true;
	else if (gpio->port3_edges == S1C33_EDGE_FALLING)
		rising = false;
	else
		rising = !gpio->port3_level;
	s1c33_gpio_update8(gpio->irq_base + S1C33_SPP07, S1C33_PORT3,
			   rising ? S1C33_PORT3 : 0);
}

static void s1c33_gpio_key_interrupt(struct s1c33_gpio *gpio)
{
	unsigned long rose = 0, fell = 0;
	unsigned int i;
	u8 bits;

	spin_lock(&gpio->lock);
	/* Store what was read as the new pattern, and go round again if a
	 * line moved in between: the mismatch would never interrupt. */
	do {
		bits = s1c33_gpio_key_bits(gpio);
		rose |= bits & ~gpio->key_state;
		fell |= ~bits & gpio->key_state & S1C33_GPIO_KEY_MASK;
		gpio->key_state = bits;
		writeb(bits, gpio->irq_base + S1C33_SCPK0);
	} while (s1c33_gpio_key_bits(gpio) != bits);
	rose &= gpio->key_enabled;
	fell &= gpio->key_enabled;
	spin_unlock(&gpio->lock);

	for (i = 0; i < S1C33_GPIO_KEY_LINES; i++)
		if (((rose & BIT(i)) &&
		     (gpio->key_edges[i] & S1C33_EDGE_RISING)) ||
		    ((fell & BIT(i)) &&
		     (gpio->key_edges[i] & S1C33_EDGE_FALLING)))
			generic_handle_domain_irq(gpio->chip.irq.domain,
						  S1C33_GPIO_P60 + i);
}

static void s1c33_gpio_port3_interrupt(struct s1c33_gpio *gpio)
{
	bool report;
	u8 before;

	spin_lock(&gpio->lock);
	before = gpio->port3_level;
	s1c33_gpio_arm_port3(gpio);
	/* Re-armed for the next edge; one that came first is already lost
	 * to the hardware, so check the level once more. */
	if (s1c33_gpio_port3_bit(gpio) != gpio->port3_level)
		s1c33_gpio_arm_port3(gpio);
	report = gpio->port3_enabled &&
		(gpio->port3_level != before ||
		 gpio->port3_edges != (S1C33_EDGE_RISING | S1C33_EDGE_FALLING));
	spin_unlock(&gpio->lock);

	if (report)
		generic_handle_domain_irq(gpio->chip.irq.domain,
					  S1C33_GPIO_P03);
}

static void s1c33_gpio_irq_handler(struct irq_desc *desc)
{
	struct gpio_chip *chip = irq_desc_get_handler_data(desc);
	struct s1c33_gpio *gpio = gpiochip_get_data(chip);
	struct irq_chip *parent = irq_desc_get_chip(desc);

	chained_irq_enter(parent, desc);
	if (irq_desc_get_irq(desc) == gpio->key_irq)
		s1c33_gpio_key_interrupt(gpio);
	else
		s1c33_gpio_port3_interrupt(gpio);
	chained_irq_exit(parent, desc);
}

static void s1c33_gpio_irq_mask(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct s1c33_gpio *gpio = gpiochip_get_data(chip);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);
	unsigned long flags;

	spin_lock_irqsave(&gpio->lock, flags);
	if (hwirq == S1C33_GPIO_P03) {
		gpio->port3_enabled = false;
	} else {
		gpio->key_enabled &= ~BIT(hwirq - S1C33_GPIO_P60);
		writeb(gpio->key_enabled, gpio->irq_base + S1C33_SMPK0);
	}
	spin_unlock_irqrestore(&gpio->lock, flags);
	gpiochip_disable_irq(chip, hwirq);
}

static void s1c33_gpio_irq_unmask(struct irq_data *data)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct s1c33_gpio *gpio = gpiochip_get_data(chip);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);
	unsigned long flags;

	gpiochip_enable_irq(chip, hwirq);
	spin_lock_irqsave(&gpio->lock, flags);
	if (hwirq == S1C33_GPIO_P03) {
		gpio->port3_enabled = true;
		s1c33_gpio_arm_port3(gpio);
	} else {
		/* Compare from the level the line has now. */
		gpio->key_state = s1c33_gpio_key_bits(gpio);
		gpio->key_enabled |= BIT(hwirq - S1C33_GPIO_P60);
		writeb(gpio->key_state, gpio->irq_base + S1C33_SCPK0);
		writeb(gpio->key_enabled, gpio->irq_base + S1C33_SMPK0);
	}
	spin_unlock_irqrestore(&gpio->lock, flags);
}

static int s1c33_gpio_irq_set_type(struct irq_data *data, unsigned int type)
{
	struct gpio_chip *chip = irq_data_get_irq_chip_data(data);
	struct s1c33_gpio *gpio = gpiochip_get_data(chip);
	irq_hw_number_t hwirq = irqd_to_hwirq(data);
	unsigned long flags;
	u8 edges = 0;

	if (type & ~IRQ_TYPE_EDGE_BOTH || !type)
		return -EINVAL;
	if (type & IRQ_TYPE_EDGE_RISING)
		edges |= S1C33_EDGE_RISING;
	if (type & IRQ_TYPE_EDGE_FALLING)
		edges |= S1C33_EDGE_FALLING;

	spin_lock_irqsave(&gpio->lock, flags);
	if (hwirq == S1C33_GPIO_P03) {
		gpio->port3_edges = edges;
		s1c33_gpio_arm_port3(gpio);
	} else {
		gpio->key_edges[hwirq - S1C33_GPIO_P60] = edges;
	}
	spin_unlock_irqrestore(&gpio->lock, flags);
	return 0;
}

static const struct irq_chip s1c33_gpio_irq_chip = {
	.name = "s1c33-gpio",
	.irq_mask = s1c33_gpio_irq_mask,
	.irq_unmask = s1c33_gpio_irq_unmask,
	.irq_set_type = s1c33_gpio_irq_set_type,
	.flags = IRQCHIP_IMMUTABLE | IRQCHIP_SKIP_SET_WAKE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static void s1c33_gpio_irq_valid_mask(struct gpio_chip *chip,
				      unsigned long *valid_mask,
				      unsigned int ngpios)
{
	unsigned int i;

	bitmap_zero(valid_mask, ngpios);
	set_bit(S1C33_GPIO_P03, valid_mask);
	for (i = 0; i < S1C33_GPIO_KEY_LINES; i++)
		set_bit(S1C33_GPIO_P60 + i, valid_mask);
}

/*
 * Key input 0 on P60..P62 comparing nothing yet, port input 3 on P03 by
 * edge, and both causes at a priority the core will take.  The board
 * supplies the registers and the two vectors; without them the chip is
 * plain GPIO.
 */
static int s1c33_gpio_init_irq(struct platform_device *pdev,
			       struct s1c33_gpio *gpio)
{
	struct gpio_irq_chip *girq = &gpio->chip.irq;

	gpio->key_irq = platform_get_irq_byname_optional(pdev, "key0");
	gpio->port3_irq = platform_get_irq_byname_optional(pdev, "port3");
	if (gpio->key_irq <= 0 || gpio->port3_irq <= 0)
		return 0;
	gpio->irq_base = devm_platform_ioremap_resource_byname(pdev,
							       "interrupt");
	if (IS_ERR(gpio->irq_base))
		return PTR_ERR(gpio->irq_base);

	writeb(0, gpio->irq_base + S1C33_SMPK0);
	s1c33_gpio_update8(gpio->irq_base + S1C33_SPPK01, S1C33_SPPK0_MASK,
			   S1C33_SPPK0_P60);
	s1c33_gpio_update8(gpio->irq_base + S1C33_SPT03, S1C33_SPT3_MASK, 0);
	s1c33_gpio_update8(gpio->irq_base + S1C33_SEPT07, 0, S1C33_PORT3);
	gpio->port3_edges = S1C33_EDGE_RISING | S1C33_EDGE_FALLING;
	s1c33_gpio_arm_port3(gpio);
	s1c33_gpio_update8((void __iomem *)S1C33_ITC_PRIORITY_PORT23, 0x70,
			   S1C33_GPIO_IRQ_PRIORITY << 4);
	s1c33_gpio_update8((void __iomem *)S1C33_ITC_PRIORITY_KEY01, 0x07,
			   S1C33_GPIO_IRQ_PRIORITY);

	gpio_irq_chip_set_chip(girq, &s1c33_gpio_irq_chip);
	girq->parent_handler = s1c33_gpio_irq_handler;
	girq->num_parents = 2;
	girq->parents = devm_kcalloc(&pdev->dev, 2, sizeof(*girq->parents),
				     GFP_KERNEL);
	if (!girq->parents)
		return -ENOMEM;
	girq->parents[0] = gpio->key_irq;
	girq->parents[1] = gpio->port3_irq;
	girq->default_type = IRQ_TYPE_NONE;
	girq->handler = handle_simple_irq;
	girq->init_valid_mask = s1c33_gpio_irq_valid_mask;
	return 1;
}

static int s1c33_gpio_probe(struct platform_device *pdev)
{
	struct s1c33_gpio *gpio;
	int interrupts;
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
	interrupts = s1c33_gpio_init_irq(pdev, gpio);
	if (interrupts < 0)
		return dev_err_probe(&pdev->dev, interrupts,
				     "cannot map interrupt registers\n");

	ret = devm_gpiochip_add_data(&pdev->dev, &gpio->chip, gpio);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot register GPIO controller\n");
	dev_info(&pdev->dev, "registered 56 GPIOs through gpiolib%s\n",
		 interrupts ? ", P03 and P60..P62 interrupting" : "");
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
