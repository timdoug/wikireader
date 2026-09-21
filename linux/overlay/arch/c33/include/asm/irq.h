/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_C33_IRQ_H
#define _ASM_C33_IRQ_H

#define NR_IRQS 64

#define C33_IRQ_HSDMA0       22
#define C33_IRQ_HSDMA1       23
#define C33_IRQ_HSDMA2       24
#define C33_IRQ_HSDMA3       25
#define C33_IRQ_TIMER2       38
#define C33_IRQ_UART0_ERROR  56
#define C33_IRQ_UART0_RX     57
#define C33_IRQ_UART0_TX     58
#define C33_IRQ_UART1_ERROR  60
#define C33_IRQ_UART1_RX     61
#define C33_IRQ_UART1_TX     62

static inline int irq_canonicalize(int irq)
{
	return irq;
}

#endif /* _ASM_C33_IRQ_H */
