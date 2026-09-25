/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_IRQCHIP_S1C33_ITC_H
#define __LINUX_IRQCHIP_S1C33_ITC_H

#include <linux/types.h>

/*
 * The Epson S1C33 interrupt controller, for an arch without a device tree:
 * the arch calls the init from init_IRQ(), maps the trap vectors it hands to
 * platform devices, and routes each trap to the domain from its entry path.
 */
int s1c33_itc_init(void);
bool s1c33_itc_is_source(unsigned int vector);
int s1c33_itc_irq(unsigned int vector);
int s1c33_itc_handle(unsigned int vector);

#endif
