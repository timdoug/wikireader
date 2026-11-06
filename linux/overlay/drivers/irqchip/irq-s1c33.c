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
 *
 * The controller's block, 0x300260..0x3002af, also holds the intelligent
 * DMA's request and enable bits and the high-speed DMA channels' trigger
 * selectors, and the flags of causes that other drivers poll rather than
 * take as interrupts.  It is this driver's alone: the IDMA, which Linux
 * does not use, is switched off here; a cause's flag is its pending state,
 * irq_get_irqchip_state() and irq_set_irqchip_state(); and the trigger
 * selectors make the controller a DMA router in front of the HSDMA.
 */
#include <linux/bitops.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_dma.h>
#include <linux/spinlock.h>

/* Offsets in the block, from 0x300260 (the manual's III.2). */
#define S1C33_ITC_ENABLE	0x10	/* the first 16 causes' banks */
#define S1C33_ITC_FLAG		0x20
#define S1C33_ITC_BANK		16
#define S1C33_ITC_IDMA_REQ	0x30	/* four bytes, then four of enables */
#define S1C33_ITC_IDMA_EN	0x34
#define S1C33_ITC_HSDMA_TRIGGER	0x38	/* a nibble a channel, 0 and 1 then 2 and 3 */
#define S1C33_ITC_IDMA_REQ2	0x3b	/* LCDC, serial 2, SPI */
#define S1C33_ITC_IDMA_EN2	0x3c
#define S1C33_ITC_FLAG_MODE	0x3f
#define S1C33_ITC_ENABLE2	0x46	/* ports 8-15 and I2S */
#define S1C33_ITC_FLAG2		0x49
#define S1C33_ITC_IDMA_REQ3	0x4c
#define S1C33_ITC_IDMA_EN3	0x4e
#define S1C33_ITC_VECTORS	108
#define S1C33_HSDMA_CHANNELS	4

/*
 * Register index within the enable and flag banks, stored plus one so that
 * zero means "not an interrupt source", the bit within it, and the group's
 * priority register and nibble.
 */
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
	S1C33_SOURCE(81, 9, 4, 0xe, 4),	/* SPI receive DMA request */
	S1C33_SOURCE(82, 9, 5, 0xe, 4),	/* SPI transmit DMA request */
};

static void __iomem *s1c33_itc_base;
static struct irq_domain *s1c33_itc_domain;
/* The bytes that hold several causes' enables, levels or triggers. */
static DEFINE_RAW_SPINLOCK(s1c33_itc_lock);

static void s1c33_itc_update(unsigned int offset, u8 mask, u8 value)
{
	void __iomem *reg = s1c33_itc_base + offset;
	unsigned long flags;

	raw_spin_lock_irqsave(&s1c33_itc_lock, flags);
	writeb((readb(reg) & ~mask) | value, reg);
	raw_spin_unlock_irqrestore(&s1c33_itc_lock, flags);
}

static void __iomem *s1c33_itc_reg(unsigned int bank, irq_hw_number_t hwirq)
{
	return s1c33_itc_base + bank + s1c33_itc_sources[hwirq].index - 1;
}

static unsigned int s1c33_itc_offset(unsigned int bank, irq_hw_number_t hwirq)
{
	return bank + s1c33_itc_sources[hwirq].index - 1;
}

static void s1c33_itc_mask(struct irq_data *data)
{
	s1c33_itc_update(s1c33_itc_offset(S1C33_ITC_ENABLE, data->hwirq),
			 BIT(s1c33_itc_sources[data->hwirq].bit), 0);
}

static void s1c33_itc_unmask(struct irq_data *data)
{
	u8 bit = BIT(s1c33_itc_sources[data->hwirq].bit);

	s1c33_itc_update(s1c33_itc_offset(S1C33_ITC_ENABLE, data->hwirq),
			 bit, bit);
}

/* Flags are cleared by writing 1 (S1C33_ITC_FLAG_MODE): no other bit moves. */
static void s1c33_itc_ack(struct irq_data *data)
{
	writeb(BIT(s1c33_itc_sources[data->hwirq].bit),
	       s1c33_itc_reg(S1C33_ITC_FLAG, data->hwirq));
}

/*
 * A cause's flag, whether or not its interrupt is enabled: how the HSDMA
 * finds a finished transfer and the SD host clears its port's DMA
 * requests.  A flag can be cleared but not set.
 */
static int s1c33_itc_get_state(struct irq_data *data,
			       enum irqchip_irq_state which, bool *state)
{
	if (which != IRQCHIP_STATE_PENDING)
		return -EINVAL;
	*state = readb(s1c33_itc_reg(S1C33_ITC_FLAG, data->hwirq)) &
		 BIT(s1c33_itc_sources[data->hwirq].bit);
	return 0;
}

static int s1c33_itc_set_state(struct irq_data *data,
			       enum irqchip_irq_state which, bool state)
{
	if (which != IRQCHIP_STATE_PENDING || state)
		return -EINVAL;
	s1c33_itc_ack(data);
	return 0;
}

/*
 * A source starts with its flag clear: whatever ran before Linux, or before
 * its driver, may have left the cause pending.
 */
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
	.irq_get_irqchip_state = s1c33_itc_get_state,
	.irq_set_irqchip_state = s1c33_itc_set_state,
	/*
	 * Nothing powers the controller down, so a wake source is simply an
	 * interrupt that suspend leaves enabled.
	 */
	.flags = IRQCHIP_SKIP_SET_WAKE,
};

static bool s1c33_itc_is_source(unsigned int vector)
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
	u32 level;

	if (intsize != 2 || !s1c33_itc_is_source(intspec[0]))
		return -EINVAL;
	level = intspec[1];
	if (level > 7)
		return -EINVAL;
	source = &s1c33_itc_sources[intspec[0]];
	if (level)
		s1c33_itc_update(source->priority, 7 << source->shift,
				 level << source->shift);
	*hwirq = intspec[0];
	*type = IRQ_TYPE_EDGE_RISING;
	return 0;
}

static const struct irq_domain_ops s1c33_itc_domain_ops = {
	.map = s1c33_itc_map,
	.xlate = s1c33_itc_xlate,
};

/*
 * The DMA router.  An HSDMA channel's trigger is a nibble here, and each
 * channel has triggers of its own: a request is <channel trigger>, the
 * value that channel takes for the cause (the manual's table II.1.5.1).
 * The trigger is connected while the channel is allocated, and the HSDMA
 * is handed the channel alone.
 */
static struct s1c33_itc_route {
	bool used;
} s1c33_itc_routes[S1C33_HSDMA_CHANNELS];

static void s1c33_itc_set_trigger(unsigned int channel, unsigned int trigger)
{
	unsigned int shift = (channel % 2) * 4;

	s1c33_itc_update(S1C33_ITC_HSDMA_TRIGGER + channel / 2, 0xf << shift,
			 trigger << shift);
}

static void *s1c33_itc_route_allocate(struct of_phandle_args *spec,
				      struct of_dma *ofdma)
{
	struct s1c33_itc_route *route;
	struct device_node *master;
	unsigned int channel;
	unsigned long flags;
	bool used;

	if (spec->args_count != 2 || spec->args[0] >= S1C33_HSDMA_CHANNELS ||
	    spec->args[1] > 15)
		return ERR_PTR(-EINVAL);
	channel = spec->args[0];
	route = &s1c33_itc_routes[channel];
	master = of_parse_phandle(ofdma->of_node, "dma-masters", 0);
	if (!master)
		return ERR_PTR(-EINVAL);

	raw_spin_lock_irqsave(&s1c33_itc_lock, flags);
	used = route->used;
	route->used = true;
	raw_spin_unlock_irqrestore(&s1c33_itc_lock, flags);
	if (used) {
		of_node_put(master);
		return ERR_PTR(-EBUSY);
	}
	s1c33_itc_set_trigger(channel, spec->args[1]);

	/* of_dma_router_xlate() puts the reference of_parse_phandle() took. */
	spec->np = master;
	spec->args_count = 1;
	spec->args[0] = channel;
	return route;
}

static void s1c33_itc_route_free(struct device *dev, void *route_data)
{
	struct s1c33_itc_route *route = route_data;

	s1c33_itc_set_trigger(route - s1c33_itc_routes, 0);
	WRITE_ONCE(route->used, false);
}

static struct dma_router s1c33_itc_router = {
	.route_free = s1c33_itc_route_free,
};

/*
 * Called by the arch's entry path for every trap from vector 16 up, the
 * maskable interrupts.  The core vectors each cause to its own trap, and
 * the entry stub stores the vector in the frame.
 */
static void s1c33_itc_handle_irq(struct pt_regs *regs)
{
	if (unlikely(generic_handle_domain_irq(s1c33_itc_domain, regs->vector)))
		pr_warn_ratelimited("s1c33-itc: spurious trap vector %lu\n",
				    regs->vector);
}

static int __init s1c33_itc_init(struct device_node *node,
				 struct device_node *parent)
{
	void __iomem *base;
	unsigned int sources = 0;
	unsigned int i;

	base = of_io_request_and_map(node, 0, "s1c33-itc");
	if (IS_ERR(base))
		return PTR_ERR(base);
	s1c33_itc_base = base;

	/*
	 * Every cause off, then every pending flag cleared by writing it;
	 * every IDMA request and enable and every HSDMA trigger off.  What
	 * ran before Linux may have left any of them set.
	 */
	for (i = 0; i < S1C33_ITC_BANK; i++)
		writeb(0, base + S1C33_ITC_ENABLE + i);
	for (i = 0; i < 2; i++)
		writeb(0, base + S1C33_ITC_ENABLE2 + i);
	writeb(1, base + S1C33_ITC_FLAG_MODE);
	for (i = 0; i < S1C33_ITC_BANK; i++)
		writeb(0xff, base + S1C33_ITC_FLAG + i);
	for (i = 0; i < 2; i++)
		writeb(0xff, base + S1C33_ITC_FLAG2 + i);
	for (i = 0; i < 4; i++) {
		writeb(0, base + S1C33_ITC_IDMA_EN + i);
		writeb(0, base + S1C33_ITC_IDMA_REQ + i);
	}
	for (i = 0; i < 2; i++) {
		writeb(0, base + S1C33_ITC_IDMA_EN3 + i);
		writeb(0, base + S1C33_ITC_IDMA_REQ3 + i);
	}
	writeb(0, base + S1C33_ITC_IDMA_EN2);
	writeb(0, base + S1C33_ITC_IDMA_REQ2);
	writeb(0, base + S1C33_ITC_HSDMA_TRIGGER);
	writeb(0, base + S1C33_ITC_HSDMA_TRIGGER + 1);

	s1c33_itc_domain = irq_domain_create_linear(of_fwnode_handle(node),
						    S1C33_ITC_VECTORS,
						    &s1c33_itc_domain_ops, NULL);
	if (!s1c33_itc_domain)
		return -ENOMEM;
	for (i = 0; i < S1C33_ITC_VECTORS; i++)
		if (s1c33_itc_sources[i].index)
			sources++;
	set_handle_irq(s1c33_itc_handle_irq);
	if (of_property_present(node, "dma-masters")) {
		int ret = of_dma_router_register(node, s1c33_itc_route_allocate,
						 &s1c33_itc_router);

		if (ret)
			pr_err("s1c33-itc: no DMA router: %d\n", ret);
	}
	pr_info("s1c33-itc: %u interrupt sources, trap vector as hardware interrupt number\n",
		sources);
	return 0;
}
IRQCHIP_DECLARE(s1c33_itc, "epson,s1c33e07-itc", s1c33_itc_init);
