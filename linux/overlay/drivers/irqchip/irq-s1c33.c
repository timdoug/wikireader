// SPDX-License-Identifier: GPL-2.0-only
/*
 * Epson S1C33 interrupt controller (ITC).
 *
 * The core takes trap vectors; the ITC turns peripheral causes into them and
 * keeps one enable bit and one flag bit per cause, spread over sixteen byte
 * registers each.  The trap vector number is the hardware interrupt number
 * here, and the domain hands out Linux numbers for the vectors a driver asks
 * for.  A flag is cleared by writing it, and a cause cannot fire again while
 * its flag is set, so the sources are edge interrupts acked before their
 * handler runs; the arch selects HARDIRQS_SW_RESEND for the one that ends a
 * suspend, which the wake path consumes without handling.
 *
 * A device tree names a source by its vector and priority level, 1 to 7:
 * the core takes an interrupt only above the level in its PSR.  Causes are
 * grouped, each group with a nibble of 0x260..0x26f, so every source in a
 * group has the level the last of them asked for; 0 leaves the nibble as
 * the loader set it.
 */
#include <linux/bitops.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/s1c33-itc.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_address.h>

#define S1C33_ITC_ENABLE	0x10
#define S1C33_ITC_FLAG		0x20
#define S1C33_ITC_FLAG_MODE	0x3f
#define S1C33_ITC_BANK		16
#define S1C33_ITC_VECTORS	64

/* Register index within the enable and flag banks, stored plus one so that
 * zero means "not an interrupt source", the bit within it, and the group's
 * priority register and nibble. */
struct s1c33_itc_source {
	u8 index;
	u8 bit;
	u8 priority;
	u8 shift;
};

#define S1C33_SOURCE(vector, register_index, register_bit, pri, pri_shift) \
	[(vector)] = { .index = (register_index) + 1, .bit = (register_bit), \
		       .priority = (pri), .shift = (pri_shift) }

static const struct s1c33_itc_source s1c33_itc_sources[S1C33_ITC_VECTORS] = {
	S1C33_SOURCE(19, 0, 3, 0x1, 4),	/* port input 3 */
	S1C33_SOURCE(20, 0, 4, 0x2, 0),	/* key input 0 */
	S1C33_SOURCE(22, 1, 0, 0x3, 0),	/* HSDMA0 */
	S1C33_SOURCE(23, 1, 1, 0x3, 4),	/* HSDMA1 */
	S1C33_SOURCE(24, 1, 2, 0x4, 0),	/* HSDMA2 */
	S1C33_SOURCE(25, 1, 3, 0x4, 4),	/* HSDMA3 */
	S1C33_SOURCE(30, 2, 2, 0x6, 0),	/* timer 0 A */
	S1C33_SOURCE(31, 2, 3, 0x6, 0),	/* timer 0 B */
	S1C33_SOURCE(34, 2, 6, 0x6, 4),	/* timer 1 A */
	S1C33_SOURCE(35, 2, 7, 0x6, 4),	/* timer 1 B */
	S1C33_SOURCE(38, 3, 2, 0x7, 0),	/* timer 2 A */
	S1C33_SOURCE(39, 3, 3, 0x7, 0),	/* timer 2 B */
	S1C33_SOURCE(42, 3, 6, 0x7, 4),	/* timer 3 A */
	S1C33_SOURCE(43, 3, 7, 0x7, 4),	/* timer 3 B */
	S1C33_SOURCE(46, 4, 2, 0x8, 0),	/* timer 4 A */
	S1C33_SOURCE(47, 4, 3, 0x8, 0),	/* timer 4 B */
	S1C33_SOURCE(50, 4, 6, 0x8, 4),	/* timer 5 A */
	S1C33_SOURCE(51, 4, 7, 0x8, 4),	/* timer 5 B */
	S1C33_SOURCE(56, 6, 0, 0x9, 4),	/* serial 0 error */
	S1C33_SOURCE(57, 6, 1, 0x9, 4),	/* serial 0 receive */
	S1C33_SOURCE(58, 6, 2, 0x9, 4),	/* serial 0 transmit */
	S1C33_SOURCE(60, 6, 3, 0xa, 0),	/* serial 1 error */
	S1C33_SOURCE(61, 6, 4, 0xa, 0),	/* serial 1 receive */
	S1C33_SOURCE(62, 6, 5, 0xa, 0),	/* serial 1 transmit */
};

static void __iomem *s1c33_itc_base;
static struct irq_domain *s1c33_itc_domain;

static void __iomem *s1c33_itc_reg(unsigned int bank, irq_hw_number_t hwirq)
{
	return s1c33_itc_base + bank + s1c33_itc_sources[hwirq].index - 1;
}

static void s1c33_itc_mask(struct irq_data *data)
{
	void __iomem *reg = s1c33_itc_reg(S1C33_ITC_ENABLE, data->hwirq);

	writeb(readb(reg) & ~BIT(s1c33_itc_sources[data->hwirq].bit), reg);
}

static void s1c33_itc_unmask(struct irq_data *data)
{
	void __iomem *reg = s1c33_itc_reg(S1C33_ITC_ENABLE, data->hwirq);

	writeb(readb(reg) | BIT(s1c33_itc_sources[data->hwirq].bit), reg);
}

static void s1c33_itc_ack(struct irq_data *data)
{
	writeb(BIT(s1c33_itc_sources[data->hwirq].bit),
	       s1c33_itc_reg(S1C33_ITC_FLAG, data->hwirq));
}

/* A source starts with its flag clear: whatever ran before Linux, or before
 * its driver, may have left the cause pending. */
static unsigned int s1c33_itc_startup(struct irq_data *data)
{
	s1c33_itc_ack(data);
	s1c33_itc_unmask(data);
	return 0;
}

static struct irq_chip s1c33_itc_chip = {
	.name = "S1C33-ITC",
	.irq_startup = s1c33_itc_startup,
	.irq_mask = s1c33_itc_mask,
	.irq_unmask = s1c33_itc_unmask,
	.irq_ack = s1c33_itc_ack,
	/* Nothing powers the controller down, so a wake source is simply an
	 * interrupt that suspend leaves enabled. */
	.flags = IRQCHIP_SKIP_SET_WAKE,
};

bool s1c33_itc_is_source(unsigned int vector)
{
	return vector < S1C33_ITC_VECTORS && s1c33_itc_sources[vector].index;
}

static int s1c33_itc_map(struct irq_domain *domain, unsigned int virq,
			 irq_hw_number_t hwirq)
{
	if (!s1c33_itc_is_source(hwirq))
		return -EINVAL;
	irq_set_chip_and_handler(virq, &s1c33_itc_chip, handle_edge_irq);
	irq_set_noprobe(virq);
	return 0;
}

/* <vector priority>: the vector is the source, the level goes to its group. */
static int s1c33_itc_xlate(struct irq_domain *domain, struct device_node *node,
			   const u32 *intspec, unsigned int intsize,
			   irq_hw_number_t *hwirq, unsigned int *type)
{
	const struct s1c33_itc_source *source;
	void __iomem *reg;
	u32 level;

	if (intsize != 2 || !s1c33_itc_is_source(intspec[0]))
		return -EINVAL;
	level = intspec[1];
	if (level > 7)
		return -EINVAL;
	source = &s1c33_itc_sources[intspec[0]];
	if (level) {
		reg = s1c33_itc_base + source->priority;
		writeb((readb(reg) & ~(7 << source->shift)) |
		       level << source->shift, reg);
	}
	*hwirq = intspec[0];
	*type = IRQ_TYPE_EDGE_RISING;
	return 0;
}

static const struct irq_domain_ops s1c33_itc_domain_ops = {
	.map = s1c33_itc_map,
	.xlate = s1c33_itc_xlate,
};

int s1c33_itc_handle(unsigned int vector)
{
	return generic_handle_domain_irq(s1c33_itc_domain, vector);
}

static int __init s1c33_itc_init(struct device_node *node,
				 struct device_node *parent)
{
	void __iomem *base;
	unsigned int sources = 0;
	unsigned int i;

	base = of_iomap(node, 0);
	if (!base)
		return -ENOMEM;
	s1c33_itc_base = base;

	/* Every cause off, then every pending flag cleared by writing it. */
	for (i = 0; i < S1C33_ITC_BANK; i++)
		writeb(0, base + S1C33_ITC_ENABLE + i);
	writeb(1, base + S1C33_ITC_FLAG_MODE);
	for (i = 0; i < S1C33_ITC_BANK; i++)
		writeb(0xff, base + S1C33_ITC_FLAG + i);

	s1c33_itc_domain = irq_domain_create_linear(of_fwnode_handle(node),
						    S1C33_ITC_VECTORS,
						    &s1c33_itc_domain_ops, NULL);
	if (!s1c33_itc_domain)
		return -ENOMEM;
	for (i = 0; i < S1C33_ITC_VECTORS; i++)
		if (s1c33_itc_sources[i].index)
			sources++;
	pr_info("s1c33-itc: %u interrupt sources, trap vector as hardware interrupt number\n",
		sources);
	return 0;
}
IRQCHIP_DECLARE(s1c33_itc, "epson,s1c33-itc", s1c33_itc_init);
