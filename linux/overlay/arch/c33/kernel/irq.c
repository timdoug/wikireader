// SPDX-License-Identifier: GPL-2.0
#include <linux/interrupt.h>
#include <linux/hardirq.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/syscalls.h>

#include <asm/irq_regs.h>
#include <asm/ptrace.h>

#define C33_REG_BASE          0x00300000UL
#define C33_IRQ_ENABLE_FIRST  (C33_REG_BASE + 0x270)
#define C33_IRQ_FLAG_FIRST    (C33_REG_BASE + 0x280)
#define C33_IRQ_RESET_MODE    (C33_REG_BASE + 0x29f)
#define C33_SYSCALL_VECTOR    12
#define C33_TIMER2_VECTOR     38
#define C33_UART0_RX_VECTOR   57

extern unsigned long c33_vector_table[];
extern void c33_timer_interrupt(void);
extern void c33_uart_rx_interrupt(void);
extern void *const c33_sys_call_table[];
asmlinkage struct pt_regs *c33_handle_irq(unsigned int vector,
					  struct pt_regs *regs);

typedef long (*c33_syscall_fn_t)(unsigned long, unsigned long,
				 unsigned long, unsigned long,
				 unsigned long, unsigned long);

void __init init_IRQ(void)
{
	volatile unsigned char *reg;

	for (reg = (void *)C33_IRQ_ENABLE_FIRST;
	     reg < (volatile unsigned char *)C33_IRQ_ENABLE_FIRST + 16; reg++)
		*reg = 0;

	*(volatile unsigned char *)C33_IRQ_RESET_MODE = 1;
	for (reg = (void *)C33_IRQ_FLAG_FIRST;
	     reg < (volatile unsigned char *)C33_IRQ_FLAG_FIRST + 16; reg++)
		*reg = 0xff;

	__asm__ volatile ("ld.w %%ttbr,%0" : : "r" (c33_vector_table)
			  : "memory");
}

asmlinkage struct pt_regs *c33_handle_irq(unsigned int vector,
					  struct pt_regs *regs)
{
	struct pt_regs *old_regs = set_irq_regs(regs);
	unsigned long nr;
	int i;

	if (vector == C33_SYSCALL_VECTOR) {
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
		set_irq_regs(old_regs);
		return regs;
	}

	if (vector != C33_TIMER2_VECTOR && vector != C33_UART0_RX_VECTOR) {
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
	if (vector == C33_TIMER2_VECTOR)
		c33_timer_interrupt();
	else
		c33_uart_rx_interrupt();
	irq_exit();

	set_irq_regs(old_regs);
	return regs;
}
