// SPDX-License-Identifier: GPL-2.0
#include <linux/interrupt.h>
#include <linux/hardirq.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdesc.h>
#include <linux/io.h>
#include <linux/syscalls.h>

#include <asm/irq.h>
#include <asm/irq_regs.h>
#include <asm/ptrace.h>
#include <asm/syscalls.h>
#include <asm/wikireader.h>

#define C33_REG_BASE          0x00300000UL
#define C33_IRQ_ENABLE_FIRST  (C33_REG_BASE + 0x270)
#define C33_IRQ_FLAG_FIRST    (C33_REG_BASE + 0x280)
#define C33_IRQ_RESET_MODE    (C33_REG_BASE + 0x29f)
#define C33_SYSCALL_VECTOR    12

struct c33_irq_source {
	u16 enable;
	u16 flag;
	u8 mask;
};

#define C33_IRQ_SOURCE(v, e, f, b) \
	[(v)] = { .enable = (e), .flag = (f), .mask = BIT(b) }

static const struct c33_irq_source c33_irq_sources[NR_IRQS] = {
	C33_IRQ_SOURCE(19, 0x270, 0x280, 3),
	C33_IRQ_SOURCE(20, 0x270, 0x280, 4),
	C33_IRQ_SOURCE(22, 0x271, 0x281, 0),
	C33_IRQ_SOURCE(23, 0x271, 0x281, 1),
	C33_IRQ_SOURCE(24, 0x271, 0x281, 2),
	C33_IRQ_SOURCE(25, 0x271, 0x281, 3),
	C33_IRQ_SOURCE(30, 0x272, 0x282, 2),
	C33_IRQ_SOURCE(31, 0x272, 0x282, 3),
	C33_IRQ_SOURCE(34, 0x272, 0x282, 6),
	C33_IRQ_SOURCE(35, 0x272, 0x282, 7),
	C33_IRQ_SOURCE(38, 0x273, 0x283, 2),
	C33_IRQ_SOURCE(39, 0x273, 0x283, 3),
	C33_IRQ_SOURCE(42, 0x273, 0x283, 6),
	C33_IRQ_SOURCE(43, 0x273, 0x283, 7),
	C33_IRQ_SOURCE(46, 0x274, 0x284, 2),
	C33_IRQ_SOURCE(47, 0x274, 0x284, 3),
	C33_IRQ_SOURCE(50, 0x274, 0x284, 6),
	C33_IRQ_SOURCE(51, 0x274, 0x284, 7),
	C33_IRQ_SOURCE(56, 0x276, 0x286, 0),
	C33_IRQ_SOURCE(57, 0x276, 0x286, 1),
	C33_IRQ_SOURCE(58, 0x276, 0x286, 2),
	C33_IRQ_SOURCE(60, 0x276, 0x286, 3),
	C33_IRQ_SOURCE(61, 0x276, 0x286, 4),
	C33_IRQ_SOURCE(62, 0x276, 0x286, 5),
};

extern unsigned long c33_vector_table[];
extern void *const c33_sys_call_table[];
unsigned long c33_boot_ttbr;
int c33_grifo_booted;
asmlinkage struct pt_regs *c33_handle_irq(unsigned int vector,
					  struct pt_regs *regs);

typedef long (*c33_syscall_fn_t)(unsigned long, unsigned long,
				 unsigned long, unsigned long,
				 unsigned long, unsigned long);

static const struct c33_irq_source *c33_irq_source(unsigned int vector)
{
	if (vector >= ARRAY_SIZE(c33_irq_sources) ||
	    !c33_irq_sources[vector].mask)
		return NULL;
	return &c33_irq_sources[vector];
}

static void c33_irq_mask(struct irq_data *data)
{
	const struct c33_irq_source *source = c33_irq_source(data->irq);
	void __iomem *reg = (void __iomem *)(C33_REG_BASE + source->enable);

	writeb(readb(reg) & ~source->mask, reg);
}

static void c33_irq_unmask(struct irq_data *data)
{
	const struct c33_irq_source *source = c33_irq_source(data->irq);
	void __iomem *reg = (void __iomem *)(C33_REG_BASE + source->enable);

	writeb(readb(reg) | source->mask, reg);
}

static void c33_irq_ack(struct irq_data *data)
{
	const struct c33_irq_source *source = c33_irq_source(data->irq);

	writeb(source->mask,
	       (void __iomem *)(C33_REG_BASE + source->flag));
}

static struct irq_chip c33_irq_chip = {
	.name = "S1C33-ITC",
	.irq_mask = c33_irq_mask,
	.irq_unmask = c33_irq_unmask,
	.irq_ack = c33_irq_ack,
};

void __init init_IRQ(void)
{
	volatile unsigned char *reg;
	unsigned int i;
	unsigned int sources = 0;

	/* Keep a resident Grifo's trap table for poweroff and reboot. */
	__asm__ volatile ("ld.w %0, %%ttbr" : "=r" (c33_boot_ttbr));
	c33_grifo_booted = c33_boot_ttbr == C33_GRIFO_TTBR;
	pr_info("C33 boot: %s (incoming TTBR %08lx)\n",
		c33_grifo_booted ? "Grifo application" : "standalone",
		c33_boot_ttbr);

	for (reg = (void *)C33_IRQ_ENABLE_FIRST;
	     reg < (volatile unsigned char *)C33_IRQ_ENABLE_FIRST + 16; reg++)
		*reg = 0;

	*(volatile unsigned char *)C33_IRQ_RESET_MODE = 1;
	for (reg = (void *)C33_IRQ_FLAG_FIRST;
	     reg < (volatile unsigned char *)C33_IRQ_FLAG_FIRST + 16; reg++)
		*reg = 0xff;
	for (i = 0; i < ARRAY_SIZE(c33_irq_sources); i++) {
		if (!c33_irq_sources[i].mask)
			continue;
		irq_set_chip_and_handler(i, &c33_irq_chip, handle_edge_irq);
		sources++;
	}

	__asm__ volatile ("ld.w %%ttbr,%0" : : "r" (c33_vector_table)
			  : "memory");
	pr_info("C33 IRQ: registered %u interrupt sources\n",
		sources);
	c33_lcd_checkpoint(2);
}

asmlinkage struct pt_regs *c33_handle_irq(unsigned int vector,
					  struct pt_regs *regs)
{
	struct pt_regs *old_regs = set_irq_regs(regs);
	unsigned long nr;
	int i;

	if (vector == C33_SYSCALL_VECTOR) {
		struct pt_regs *old_task_regs = current->thread.regs;

		/* Generic clone and exec code must see this live syscall frame. */
		current->thread.regs = regs;
		nr = regs->r[4];
		regs->orig_r4 = nr;
		if (nr < NR_syscalls) {
			c33_syscall_fn_t fn =
				(c33_syscall_fn_t)c33_sys_call_table[nr];

			pr_info_once("C33: entered userspace syscall path\n");
			__asm__ volatile ("psrset 4" : : : "memory");
			regs->r[4] = fn(regs->r[6], regs->r[7], regs->r[8],
					regs->r[9], regs->r[10], regs->r[11]);
			__asm__ volatile ("psrclr 4" : : : "memory");
		} else {
			regs->r[4] = -ENOSYS;
		}
		c33_do_notify_resume(regs, 1);
		current->thread.regs = old_task_regs;
		set_irq_regs(old_regs);
		return regs;
	}

	if (!c33_irq_source(vector)) {
		c33_lcd_fault(vector);
		pr_emerg("C33 exception %u: pc=%08lx sp=%08lx psr=%08lx\n",
			 vector, regs->pc, regs->sp, regs->psr);
		for (i = 0; i < 16; i += 4)
			pr_emerg("r%d=%08lx r%d=%08lx r%d=%08lx r%d=%08lx\n",
				i, regs->r[i], i + 1, regs->r[i + 1],
				i + 2, regs->r[i + 2], i + 3,
				regs->r[i + 3]);
		panic("unhandled C33 exception");
	}

	irq_enter();
	generic_handle_irq(vector);
	irq_exit();
	if (user_mode(regs))
		c33_do_notify_resume(regs, 0);

	set_irq_regs(old_regs);
	return regs;
}
