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
 * suspend, which the wake path consumes without handling.  Priorities live in
 * the lower nibbles of 0x260..0x26f and stay with the code that knows what
 * each cause is for.
 */
#include <linux/bitops.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip/s1c33-itc.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>

#define S1C33_ITC_BASE		0x00300260UL
#define S1C33_ITC_ENABLE	0x10
#define S1C33_ITC_FLAG		0x20
#define S1C33_ITC_FLAG_MODE	0x3f
#define S1C33_ITC_BANK		16
#define S1C33_ITC_VECTORS	64

/* Register index within the enable and flag banks, stored plus one so that
 * zero means "not an interrupt source", and the bit within it. */
struct s1c33_itc_source {
	u8 index;
	u8 bit;
};

#define S1C33_SOURCE(vector, register_index, register_bit)		\
	[(vector)] = { .index = (register_index) + 1, .bit = (register_bit) }

static const struct s1c33_itc_source s1c33_itc_sources[S1C33_ITC_VECTORS] = {
	S1C33_SOURCE(19, 0, 3),		/* port input 3 */
	S1C33_SOURCE(20, 0, 4),		/* key input 0 */
	S1C33_SOURCE(22, 1, 0),		/* HSDMA0 */
	S1C33_SOURCE(23, 1, 1),		/* HSDMA1 */
	S1C33_SOURCE(24, 1, 2),		/* HSDMA2 */
	S1C33_SOURCE(25, 1, 3),		/* HSDMA3 */
	S1C33_SOURCE(30, 2, 2),		/* timer 0 A */
	S1C33_SOURCE(31, 2, 3),		/* timer 0 B */
	S1C33_SOURCE(34, 2, 6),		/* timer 1 A */
	S1C33_SOURCE(35, 2, 7),		/* timer 1 B */
	S1C33_SOURCE(38, 3, 2),		/* timer 2 A */
	S1C33_SOURCE(39, 3, 3),		/* timer 2 B */
	S1C33_SOURCE(42, 3, 6),		/* timer 3 A */
	S1C33_SOURCE(43, 3, 7),		/* timer 3 B */
	S1C33_SOURCE(46, 4, 2),		/* timer 4 A */
	S1C33_SOURCE(47, 4, 3),		/* timer 4 B */
	S1C33_SOURCE(50, 4, 6),		/* timer 5 A */
	S1C33_SOURCE(51, 4, 7),		/* timer 5 B */
	S1C33_SOURCE(56, 6, 0),		/* serial 0 error */
	S1C33_SOURCE(57, 6, 1),		/* serial 0 receive */
	S1C33_SOURCE(58, 6, 2),		/* serial 0 transmit */
	S1C33_SOURCE(60, 6, 3),		/* serial 1 error */
	S1C33_SOURCE(61, 6, 4),		/* serial 1 receive */
	S1C33_SOURCE(62, 6, 5),		/* serial 1 transmit */
};

static struct irq_domain *s1c33_itc_domain;

static void __iomem *s1c33_itc_reg(unsigned int bank, irq_hw_number_t hwirq)
{
	return (void __iomem *)(S1C33_ITC_BASE + bank +
				s1c33_itc_sources[hwirq].index - 1);
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

static struct irq_chip s1c33_itc_chip = {
	.name = "S1C33-ITC",
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

static const struct irq_domain_ops s1c33_itc_domain_ops = {
	.map = s1c33_itc_map,
};

int s1c33_itc_irq(unsigned int vector)
{
	unsigned int virq;

	if (!s1c33_itc_domain || !s1c33_itc_is_source(vector))
		return -EINVAL;
	virq = irq_create_mapping(s1c33_itc_domain, vector);
	return virq ? (int)virq : -ENOMEM;
}

int s1c33_itc_handle(unsigned int vector)
{
	return generic_handle_domain_irq(s1c33_itc_domain, vector);
}

int __init s1c33_itc_init(void)
{
	void __iomem *base = (void __iomem *)S1C33_ITC_BASE;
	struct fwnode_handle *fwnode;
	unsigned int sources = 0;
	unsigned int i;

	/* Every cause off, then every pending flag cleared by writing it. */
	for (i = 0; i < S1C33_ITC_BANK; i++)
		writeb(0, base + S1C33_ITC_ENABLE + i);
	writeb(1, base + S1C33_ITC_FLAG_MODE);
	for (i = 0; i < S1C33_ITC_BANK; i++)
		writeb(0xff, base + S1C33_ITC_FLAG + i);

	fwnode = irq_domain_alloc_named_fwnode("s1c33-itc");
	if (!fwnode)
		return -ENOMEM;
	s1c33_itc_domain = irq_domain_create_linear(fwnode, S1C33_ITC_VECTORS,
						    &s1c33_itc_domain_ops, NULL);
	if (!s1c33_itc_domain) {
		irq_domain_free_fwnode(fwnode);
		return -ENOMEM;
	}
	for (i = 0; i < S1C33_ITC_VECTORS; i++)
		if (s1c33_itc_sources[i].index)
			sources++;
	pr_info("s1c33-itc: %u interrupt sources, trap vector as hardware interrupt number\n",
		sources);
	return sources;
}
