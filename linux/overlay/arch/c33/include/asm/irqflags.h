/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_IRQFLAGS_H
#define _ASM_C33_IRQFLAGS_H

#define C33_PSR_IE (1 << 4)

static inline unsigned long arch_local_save_flags(void)
{
	unsigned long flags;
	__asm__ volatile ("ld.w %0, %%psr" : "=r" (flags));
	return flags;
}

static inline void arch_local_irq_disable(void)
{
	__asm__ volatile ("psrclr 4" : : : "memory");
}

static inline void arch_local_irq_enable(void)
{
	__asm__ volatile ("psrset 4" : : : "memory");
}

static inline unsigned long arch_local_irq_save(void)
{
	unsigned long flags = arch_local_save_flags();
	arch_local_irq_disable();
	return flags;
}

static inline void arch_local_irq_restore(unsigned long flags)
{
	if (flags & C33_PSR_IE)
		arch_local_irq_enable();
	else
		arch_local_irq_disable();
}

static inline int arch_irqs_disabled_flags(unsigned long flags)
{
	return !(flags & C33_PSR_IE);
}

static inline int arch_irqs_disabled(void)
{
	return arch_irqs_disabled_flags(arch_local_save_flags());
}
#endif
