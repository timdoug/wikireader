/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_ENTRY_COMMON_H
#define _ASM_C33_ENTRY_COMMON_H

/*
 * The C33 has nothing to save or restore around a trip through userspace, so
 * every hook the generic entry code offers keeps its default.
 */

#include <linux/types.h>

/* Interrupts run on the stack they interrupted; there is no separate one. */
static __always_inline bool on_thread_stack(void)
{
	return true;
}

#endif
