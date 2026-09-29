/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_IRQCHIP_S1C33_ITC_H
#define __LINUX_IRQCHIP_S1C33_ITC_H

#include <linux/types.h>

/*
 * The Epson S1C33 interrupt controller, "epson,s1c33-itc" in the device
 * tree: the arch's entry path asks it whether a trap vector is one of its
 * sources and routes the trap to its domain.
 */
bool s1c33_itc_is_source(unsigned int vector);
int s1c33_itc_handle(unsigned int vector);

#endif
