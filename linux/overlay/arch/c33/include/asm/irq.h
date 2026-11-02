/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_C33_IRQ_H
#define _ASM_C33_IRQ_H

/* Linux numbers, handed out by the ITC's domain for the device tree's vectors. */
#define NR_IRQS 64

/* The trap table %ttbr points at once the kernel owns the traps: irq-entry.S. */
extern unsigned long c33_vector_table[];

static inline int irq_canonicalize(int irq)
{
	return irq;
}

#endif /* _ASM_C33_IRQ_H */
