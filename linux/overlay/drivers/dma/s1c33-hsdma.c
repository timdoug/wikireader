// SPDX-License-Identifier: GPL-2.0-only
/*
 * The Epson S1C33E07's high-speed DMA controller as a DMAengine provider.
 *
 * Four channels move one unit, a byte, half-word or word, per trigger from
 * a source to a destination address, each incremented or fixed.  A
 * channel's trigger is chosen by a nibble in the interrupt controller, and
 * each channel has triggers of its own: the SPI receiver can only trigger
 * channel 3 and its transmitter channel 2.  So a request line is a channel
 * and a trigger together, and the board's slave map names them.
 *
 * Completion is found by polling: the transfer-count flag each channel
 * raises in the interrupt controller is read by tx_status, which retires
 * the descriptor, runs its callback and starts the next, so a client waits
 * with dmaengine_tx_status() or dma_sync_wait().  tx_status also gives the
 * residue of a transfer under way, from the channel's count, so a client
 * can use what has come in so far.  That is the point on this
 * part, not a shortcut: the CPU would otherwise sleep in HALT, and on
 * physical E07 parts HALT stops the requests that SPI-triggered transfers
 * wait for.
 *
 * It is also why the driver keeps its own lists rather than virt-dma's: a
 * card read costs a descriptor a block, and on this core, which fetches
 * every instruction outside a short loop from SDRAM, virt-dma's completion
 * tasklet and the register writes it did not need cost a third of the
 * block's time.  Descriptors are recycled, and registers that hold the
 * same value from one transfer to the next are not written again.
 */
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/dmaengine.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/platform_data/dma-s1c33-hsdma.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "dmaengine.h"

#define HSDMA_CHANNELS		4
#define HSDMA_SPARE_DESCS	4

/* The "dma" registers: the IDMA run bit, then each channel's block. */
#define HSDMA_IDMA_RUN		0x05
#define HSDMA_COUNT(x)		(0x20 + 0x10 * (x))
#define HSDMA_CONTROL(x)	(0x22 + 0x10 * (x))
#define HSDMA_SRC_HI(x)		(0x26 + 0x10 * (x))
#define HSDMA_DST_HI(x)		(0x2a + 0x10 * (x))
#define HSDMA_ENABLE(x)		(0x2c + 0x10 * (x))
#define HSDMA_TRIGGER_FLAG(x)	(0x2e + 0x10 * (x))
#define HSDMA_ADV_CTL(x)	(0x62 + 0x10 * (x))
#define HSDMA_ADV_SRC(x)	(0x64 + 0x10 * (x))
#define HSDMA_ADV_DST(x)	(0x68 + 0x10 * (x))
#define HSDMA_ADV_MODE		0x9c
#define HSDMA_ADV_TIME		0x9e

#define HSDMA_DUAL		BIT(15)		/* CONTROL: dual address */
#define HSDMA_HALFWORD		BIT(14)		/* SRC_HI: 16-bit units */
#define HSDMA_INCREMENT		(2 << 12)	/* SRC_HI, DST_HI */
#define HSDMA_WORD		BIT(0)		/* ADV_CTL: 32-bit units */
#define HSDMA_COUNT_MAX		0xffffff

struct hsdma_desc {
	struct dma_async_tx_descriptor tx;
	struct list_head node;
	u32 source;
	u32 destination;
	u32 count;
	u16 control;
	u16 source_hi;
	u16 destination_hi;
	u16 word;
	u8 shift;		/* log2 of the unit */
};

struct hsdma_chan {
	struct dma_chan chan;
	struct hsdma *hsdma;
	unsigned int id;
	unsigned int trigger;
	struct dma_slave_config config;
	spinlock_t lock;		/* the lists and the channel's registers */
	struct list_head prepared;	/* not submitted, or kept for reuse */
	struct list_head submitted;
	struct list_head issued;
	struct hsdma_desc *active;
	struct hsdma_desc *spare[HSDMA_SPARE_DESCS];
	unsigned int spares;
	/* What the registers written only on a change hold now. */
	u16 control, source_hi, destination_hi, word;
};

struct hsdma {
	struct dma_device dd;
	void __iomem *base;
	void __iomem *priority;
	void __iomem *flags;
	void __iomem *triggers;
	spinlock_t trigger_lock;	/* the trigger nibbles share bytes */
	struct hsdma_chan chans[HSDMA_CHANNELS];
};

static inline struct hsdma_chan *to_hsdma_chan(struct dma_chan *chan)
{
	return container_of(chan, struct hsdma_chan, chan);
}

static inline struct hsdma_desc *to_hsdma_desc(struct dma_async_tx_descriptor *tx)
{
	return container_of(tx, struct hsdma_desc, tx);
}

static void hsdma_set_trigger(struct hsdma *hsdma, unsigned int id,
			      unsigned int trigger)
{
	void __iomem *reg = hsdma->triggers + id / 2;
	unsigned int shift = (id % 2) * 4;
	unsigned long flags;

	spin_lock_irqsave(&hsdma->trigger_lock, flags);
	writeb((readb(reg) & ~(0xf << shift)) | trigger << shift, reg);
	spin_unlock_irqrestore(&hsdma->trigger_lock, flags);
}

/* Stop the channel and forget its completion.  Under the channel lock. */
static void hsdma_stop(struct hsdma_chan *c)
{
	writew(0, c->hsdma->base + HSDMA_ENABLE(c->id));
	writeb(BIT(c->id), c->hsdma->flags);
}

static void hsdma_write_changed(struct hsdma_chan *c, u16 *cached, u16 value,
				unsigned int offset)
{
	if (*cached != value) {
		writew(value, c->hsdma->base + offset);
		*cached = value;
	}
}

/* Program and enable the next issued descriptor.  Under the channel lock. */
static void hsdma_start(struct hsdma_chan *c)
{
	void __iomem *base = c->hsdma->base;
	unsigned int x = c->id;
	struct hsdma_desc *d;

	d = list_first_entry_or_null(&c->issued, struct hsdma_desc, node);
	c->active = d;
	if (!d)
		return;
	list_del_init(&d->node);

	hsdma_write_changed(c, &c->word, d->word, HSDMA_ADV_CTL(x));
	hsdma_write_changed(c, &c->control, d->control, HSDMA_CONTROL(x));
	hsdma_write_changed(c, &c->source_hi, d->source_hi, HSDMA_SRC_HI(x));
	hsdma_write_changed(c, &c->destination_hi, d->destination_hi,
			    HSDMA_DST_HI(x));
	writew(d->count & 0xffff, base + HSDMA_COUNT(x));
	writel(d->source, base + HSDMA_ADV_SRC(x));
	writel(d->destination, base + HSDMA_ADV_DST(x));
	/* A request seen while the channel was idle must not start it. */
	writew(1, base + HSDMA_TRIGGER_FLAG(x));
	writew(1, base + HSDMA_ENABLE(x));
}

static void hsdma_recycle(struct hsdma_chan *c, struct hsdma_desc *d)
{
	if (c->spares < HSDMA_SPARE_DESCS)
		c->spare[c->spares++] = d;
	else
		kfree(d);
}

/*
 * Retire the active descriptor if its count has run out, and start the
 * next.  Under the channel lock; the callback, if any, is left in @cb for
 * the caller to run once the lock is dropped.
 */
static void hsdma_poll(struct hsdma_chan *c, struct dmaengine_desc_callback *cb)
{
	struct hsdma_desc *d = c->active;

	if (!d || !(readb(c->hsdma->flags) & BIT(c->id)))
		return;
	hsdma_stop(c);
	dma_cookie_complete(&d->tx);
	dmaengine_desc_get_callback(&d->tx, cb);
	if (dmaengine_desc_test_reuse(&d->tx))
		list_add_tail(&d->node, &c->prepared);
	else
		hsdma_recycle(c, d);
	hsdma_start(c);
}

/*
 * The bytes a transfer not yet complete has still to move.  The count
 * falls as each unit is read from the source, so the last unit or two
 * counted may not have reached the destination yet.  Under the channel
 * lock.
 */
static u32 hsdma_residue(struct hsdma_chan *c, dma_cookie_t cookie)
{
	void __iomem *base = c->hsdma->base;
	struct hsdma_desc *d = c->active;
	unsigned int x = c->id;
	u32 high, count;

	if (d && d->tx.cookie == cookie) {
		/* The count's top byte shares a register; read it either side. */
		do {
			high = readw(base + HSDMA_CONTROL(x)) & 0xff;
			count = readw(base + HSDMA_COUNT(x));
		} while ((readw(base + HSDMA_CONTROL(x)) & 0xff) != high);
		return (high << 16 | count) << d->shift;
	}
	list_for_each_entry(d, &c->issued, node)
		if (d->tx.cookie == cookie)
			return d->count << d->shift;
	list_for_each_entry(d, &c->submitted, node)
		if (d->tx.cookie == cookie)
			return d->count << d->shift;
	return 0;
}

static enum dma_status hsdma_tx_status(struct dma_chan *chan,
				       dma_cookie_t cookie,
				       struct dma_tx_state *state)
{
	struct hsdma_chan *c = to_hsdma_chan(chan);
	struct dmaengine_desc_callback cb = { };
	enum dma_status status;
	unsigned long flags;

	status = dma_cookie_status(chan, cookie, state);
	if (status == DMA_COMPLETE)
		return status;
	spin_lock_irqsave(&c->lock, flags);
	hsdma_poll(c, &cb);
	status = dma_cookie_status(chan, cookie, state);
	if (status != DMA_COMPLETE && state)
		dma_set_residue(state, hsdma_residue(c, cookie));
	spin_unlock_irqrestore(&c->lock, flags);
	dmaengine_desc_callback_invoke(&cb, NULL);
	return status;
}

/*
 * A transfer that has run out is retired first: a client that submitted
 * the next descriptor while the last still ran, and never asked after the
 * last, must not find it holding the channel.
 */
static void hsdma_issue_pending(struct dma_chan *chan)
{
	struct hsdma_chan *c = to_hsdma_chan(chan);
	struct dmaengine_desc_callback cb = { };
	unsigned long flags;

	spin_lock_irqsave(&c->lock, flags);
	hsdma_poll(c, &cb);
	list_splice_tail_init(&c->submitted, &c->issued);
	if (!c->active)
		hsdma_start(c);
	spin_unlock_irqrestore(&c->lock, flags);
	dmaengine_desc_callback_invoke(&cb, NULL);
}

/*
 * A transfer that has run out is retired here too, not only when its
 * status is asked for: a client that knows from its device that the
 * transfer is over, as the SD host does for its transmit side, need not
 * ask, and can resubmit a reused descriptor straight away.
 */
static dma_cookie_t hsdma_tx_submit(struct dma_async_tx_descriptor *tx)
{
	struct hsdma_chan *c = to_hsdma_chan(tx->chan);
	struct hsdma_desc *d = to_hsdma_desc(tx);
	struct dmaengine_desc_callback cb = { };
	unsigned long flags;
	dma_cookie_t cookie;

	spin_lock_irqsave(&c->lock, flags);
	hsdma_poll(c, &cb);
	if (d == c->active) {
		cookie = -EBUSY;
	} else {
		cookie = dma_cookie_assign(tx);
		list_move_tail(&d->node, &c->submitted);
	}
	spin_unlock_irqrestore(&c->lock, flags);
	dmaengine_desc_callback_invoke(&cb, NULL);
	return cookie;
}

/* dmaengine_desc_free(), for a descriptor kept for reuse. */
static int hsdma_desc_free(struct dma_async_tx_descriptor *tx)
{
	struct hsdma_chan *c = to_hsdma_chan(tx->chan);
	struct hsdma_desc *d = to_hsdma_desc(tx);
	unsigned long flags;

	spin_lock_irqsave(&c->lock, flags);
	list_del(&d->node);
	hsdma_recycle(c, d);
	spin_unlock_irqrestore(&c->lock, flags);
	return 0;
}

static int hsdma_config(struct dma_chan *chan, struct dma_slave_config *config)
{
	to_hsdma_chan(chan)->config = *config;
	return 0;
}

/*
 * A transfer of @length bytes in units of @width, each address incremented
 * or fixed.  The unit is a power of two, and a division is a libgcc call
 * here, so it is kept as a shift.
 */
static struct dma_async_tx_descriptor *
hsdma_prep(struct hsdma_chan *c, dma_addr_t source, bool source_inc,
	   dma_addr_t destination, bool destination_inc, size_t length,
	   enum dma_slave_buswidth width, unsigned long flags)
{
	unsigned long irqflags;
	struct hsdma_desc *d;
	unsigned int shift;

	if (width != DMA_SLAVE_BUSWIDTH_1_BYTE &&
	    width != DMA_SLAVE_BUSWIDTH_2_BYTES &&
	    width != DMA_SLAVE_BUSWIDTH_4_BYTES)
		return NULL;
	shift = __ffs(width);
	if (!length || (length | source | destination) & (width - 1) ||
	    length >> shift > HSDMA_COUNT_MAX)
		return NULL;

	spin_lock_irqsave(&c->lock, irqflags);
	d = c->spares ? c->spare[--c->spares] : NULL;
	if (!d) {
		spin_unlock_irqrestore(&c->lock, irqflags);
		d = kmalloc(sizeof(*d), GFP_NOWAIT);
		if (!d)
			return NULL;
		spin_lock_irqsave(&c->lock, irqflags);
	}
	/* Callbacks and unmap data must not survive from the last use. */
	memset(d, 0, sizeof(*d));
	dma_async_tx_descriptor_init(&d->tx, &c->chan);
	d->tx.flags = flags;
	d->tx.tx_submit = hsdma_tx_submit;
	d->tx.desc_free = hsdma_desc_free;
	d->shift = shift;
	d->count = length >> shift;
	d->control = HSDMA_DUAL | d->count >> 16;
	d->word = width == DMA_SLAVE_BUSWIDTH_4_BYTES ? HSDMA_WORD : 0;
	d->source = source;
	d->source_hi = width == DMA_SLAVE_BUSWIDTH_2_BYTES ? HSDMA_HALFWORD : 0;
	if (source_inc)
		d->source_hi |= HSDMA_INCREMENT;
	d->destination = destination;
	d->destination_hi = destination_inc ? HSDMA_INCREMENT : 0;
	list_add_tail(&d->node, &c->prepared);
	spin_unlock_irqrestore(&c->lock, irqflags);
	return &d->tx;
}

static struct dma_async_tx_descriptor *
hsdma_prep_slave_sg(struct dma_chan *chan, struct scatterlist *sgl,
		    unsigned int sg_len, enum dma_transfer_direction direction,
		    unsigned long flags, void *context)
{
	struct hsdma_chan *c = to_hsdma_chan(chan);
	dma_addr_t memory = sg_dma_address(sgl);
	size_t length = sg_dma_len(sgl);

	/* One trigger moves one unit: a list would need a trigger per entry. */
	if (sg_len != 1)
		return NULL;
	if (direction == DMA_DEV_TO_MEM)
		return hsdma_prep(c, c->config.src_addr, false, memory, true,
				  length, c->config.src_addr_width, flags);
	if (direction == DMA_MEM_TO_DEV)
		return hsdma_prep(c, memory, true, c->config.dst_addr, false,
				  length, c->config.dst_addr_width, flags);
	return NULL;
}

/*
 * One chunk of one frame, which is how a client asks for a memory address
 * held fixed: src_inc or dst_inc false.  The SD host sends a stream of
 * all-ones from one word this way.  The unit is the slave configuration's
 * for the device side.
 */
static struct dma_async_tx_descriptor *
hsdma_prep_interleaved(struct dma_chan *chan,
		       struct dma_interleaved_template *xt, unsigned long flags)
{
	struct hsdma_chan *c = to_hsdma_chan(chan);
	enum dma_slave_buswidth width;

	if (xt->numf != 1 || xt->frame_size != 1)
		return NULL;
	if (xt->dir == DMA_DEV_TO_MEM)
		width = c->config.src_addr_width;
	else if (xt->dir == DMA_MEM_TO_DEV)
		width = c->config.dst_addr_width;
	else
		return NULL;
	return hsdma_prep(c, xt->src_start, xt->src_inc, xt->dst_start,
			  xt->dst_inc, xt->sgl[0].size, width, flags);
}

/*
 * Stop the channel and drop everything queued.  Descriptors kept for reuse
 * go back to the prepared list for their owner to free.
 */
static int hsdma_terminate_all(struct dma_chan *chan)
{
	struct hsdma_chan *c = to_hsdma_chan(chan);
	struct hsdma_desc *d, *next;
	unsigned long flags;
	LIST_HEAD(head);

	spin_lock_irqsave(&c->lock, flags);
	hsdma_stop(c);
	if (c->active)
		list_add_tail(&c->active->node, &head);
	c->active = NULL;
	list_splice_tail_init(&c->submitted, &head);
	list_splice_tail_init(&c->issued, &head);
	list_for_each_entry_safe(d, next, &head, node) {
		if (dmaengine_desc_test_reuse(&d->tx)) {
			list_move_tail(&d->node, &c->prepared);
		} else {
			list_del(&d->node);
			hsdma_recycle(c, d);
		}
	}
	spin_unlock_irqrestore(&c->lock, flags);
	return 0;
}

static int hsdma_alloc_chan_resources(struct dma_chan *chan)
{
	struct hsdma_chan *c = to_hsdma_chan(chan);

	dma_cookie_init(chan);
	hsdma_set_trigger(c->hsdma, c->id, c->trigger);
	return 0;
}

static void hsdma_free_chan_resources(struct dma_chan *chan)
{
	struct hsdma_chan *c = to_hsdma_chan(chan);
	struct hsdma_desc *d, *next;

	hsdma_terminate_all(chan);
	hsdma_set_trigger(c->hsdma, c->id, 0);
	list_for_each_entry_safe(d, next, &c->prepared, node) {
		list_del(&d->node);
		kfree(d);
	}
	while (c->spares)
		kfree(c->spare[--c->spares]);
}

/* A slave map's parameter is HSDMA_REQUEST(channel, trigger). */
static bool hsdma_filter(struct dma_chan *chan, void *param)
{
	struct hsdma_chan *c = to_hsdma_chan(chan);
	unsigned long request = (unsigned long)param;

	if (HSDMA_REQUEST_CHANNEL(request) != c->id)
		return false;
	c->trigger = HSDMA_REQUEST_TRIGGER(request);
	return true;
}

/*
 * The documented reset-equivalent state: every channel stopped with its
 * trigger disconnected and latches clear, IDMA off, advanced mode for
 * 32-bit units and addresses, and no interrupt priority, since nothing
 * here takes the interrupts.  The loader that ran first may have left
 * any of it otherwise.
 */
static void hsdma_reset(struct hsdma *hsdma)
{
	void __iomem *base = hsdma->base;
	unsigned int x;

	writeb(0, base + HSDMA_IDMA_RUN);
	for (x = 0; x < HSDMA_CHANNELS; x++) {
		struct hsdma_chan *c = &hsdma->chans[x];

		hsdma_set_trigger(hsdma, x, 0);
		hsdma_stop(c);
		writew(1, base + HSDMA_TRIGGER_FLAG(x));
		writew(c->word, base + HSDMA_ADV_CTL(x));
		writew(c->control, base + HSDMA_CONTROL(x));
		writew(c->source_hi, base + HSDMA_SRC_HI(x));
		writew(c->destination_hi, base + HSDMA_DST_HI(x));
	}
	writew(1, base + HSDMA_ADV_MODE);
	writew(0, base + HSDMA_ADV_TIME);
	writeb(0, hsdma->priority);
}

static int hsdma_probe(struct platform_device *pdev)
{
	const struct s1c33_hsdma_platform_data *pdata =
		dev_get_platdata(&pdev->dev);
	struct device *dev = &pdev->dev;
	struct hsdma *hsdma;
	struct clk *clk;
	unsigned int x;
	int ret;

	hsdma = devm_kzalloc(dev, sizeof(*hsdma), GFP_KERNEL);
	if (!hsdma)
		return -ENOMEM;
	hsdma->base = devm_platform_ioremap_resource_byname(pdev, "dma");
	if (IS_ERR(hsdma->base))
		return PTR_ERR(hsdma->base);
	hsdma->priority = devm_platform_ioremap_resource_byname(pdev,
								"priority");
	if (IS_ERR(hsdma->priority))
		return PTR_ERR(hsdma->priority);
	hsdma->flags = devm_platform_ioremap_resource_byname(pdev, "flags");
	if (IS_ERR(hsdma->flags))
		return PTR_ERR(hsdma->flags);
	hsdma->triggers = devm_platform_ioremap_resource_byname(pdev,
								"triggers");
	if (IS_ERR(hsdma->triggers))
		return PTR_ERR(hsdma->triggers);
	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "cannot enable the DMA clock\n");
	spin_lock_init(&hsdma->trigger_lock);

	dma_cap_set(DMA_SLAVE, hsdma->dd.cap_mask);
	dma_cap_set(DMA_PRIVATE, hsdma->dd.cap_mask);
	dma_cap_set(DMA_INTERLEAVE, hsdma->dd.cap_mask);
	hsdma->dd.dev = dev;
	hsdma->dd.src_addr_widths = BIT(DMA_SLAVE_BUSWIDTH_1_BYTE) |
		BIT(DMA_SLAVE_BUSWIDTH_2_BYTES) |
		BIT(DMA_SLAVE_BUSWIDTH_4_BYTES);
	hsdma->dd.dst_addr_widths = hsdma->dd.src_addr_widths;
	hsdma->dd.directions = BIT(DMA_DEV_TO_MEM) | BIT(DMA_MEM_TO_DEV);
	hsdma->dd.residue_granularity = DMA_RESIDUE_GRANULARITY_BURST;
	hsdma->dd.descriptor_reuse = true;
	hsdma->dd.device_alloc_chan_resources = hsdma_alloc_chan_resources;
	hsdma->dd.device_free_chan_resources = hsdma_free_chan_resources;
	hsdma->dd.device_prep_slave_sg = hsdma_prep_slave_sg;
	hsdma->dd.device_prep_interleaved_dma = hsdma_prep_interleaved;
	hsdma->dd.device_config = hsdma_config;
	hsdma->dd.device_terminate_all = hsdma_terminate_all;
	hsdma->dd.device_tx_status = hsdma_tx_status;
	hsdma->dd.device_issue_pending = hsdma_issue_pending;
	if (pdata) {
		hsdma->dd.filter.map = pdata->slave_map;
		hsdma->dd.filter.mapcnt = pdata->slavecnt;
		hsdma->dd.filter.fn = hsdma_filter;
	}
	INIT_LIST_HEAD(&hsdma->dd.channels);
	for (x = 0; x < HSDMA_CHANNELS; x++) {
		struct hsdma_chan *c = &hsdma->chans[x];

		c->hsdma = hsdma;
		c->id = x;
		c->control = HSDMA_DUAL;
		spin_lock_init(&c->lock);
		INIT_LIST_HEAD(&c->prepared);
		INIT_LIST_HEAD(&c->submitted);
		INIT_LIST_HEAD(&c->issued);
		c->chan.device = &hsdma->dd;
		list_add_tail(&c->chan.device_node, &hsdma->dd.channels);
	}
	hsdma_reset(hsdma);

	ret = dmaenginem_async_device_register(&hsdma->dd);
	if (ret)
		return ret;
	dev_info(dev, "%d channels, completion polled\n", HSDMA_CHANNELS);
	return 0;
}

static struct platform_driver hsdma_driver = {
	.driver.name = "s1c33-hsdma",
	.probe = hsdma_probe,
};

/* Early, for the card that holds the root file system. */
static int __init hsdma_init(void)
{
	return platform_driver_register(&hsdma_driver);
}
subsys_initcall(hsdma_init);

MODULE_DESCRIPTION("Epson S1C33E07 high-speed DMA");
MODULE_LICENSE("GPL");
