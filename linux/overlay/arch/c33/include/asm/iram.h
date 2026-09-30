/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Code and data in the S1C33E07's A0 RAM, the on-chip RAM at address zero
 * with no wait states.  Code in SDRAM runs from a fetch queue that holds
 * only a short loop; anything longer fetches from SDRAM as it runs, at about
 * 2.5 cycles an instruction word, where A0 RAM costs none.  So a hot loop too
 * long for the queue can run from here, as ARM runs code from its TCM.
 *
 * Grifo keeps its own code below C33_IRAM_START and its suspend scratch
 * above C33_IRAM_END, and leaves the rest to applications, of which this
 * kernel is one.
 *
 * An __iramfunc is linked with the rest of the kernel, and setup_arch()
 * copies all of them into A0 RAM; c33_iram_func() gives the copy's
 * address.  So the function has to run anywhere: it calls nothing, libgcc
 * included, and refers to no data by address, taking what it needs as
 * arguments.  Branches are PC-relative, so its own code moves with it.
 * c33_iram_alloc() hands out the rest of the area, for the data.  Neither
 * works for modules, where an __iramfunc stays an ordinary function.
 */
#ifndef __ASM_C33_IRAM_H
#define __ASM_C33_IRAM_H

#define C33_IRAM_START	0x00000c00
#include <uapi/asm/tls.h>
#define C33_IRAM_END	C33_TLS_SLOT_ADDRESS

#include <linux/compiler.h>
#include <linux/types.h>

#ifdef MODULE
#define __iramfunc
#else
/*
 * flatten: everything the function calls is inlined into it, since a call
 * out of the copy would land wherever the linked address is relative to A0
 * RAM.  At -Os GCC otherwise keeps small inlines out of line (__fswab32).
 */
#define __iramfunc	__section(".iram.text") noinline __attribute__((flatten))
#endif

void c33_iram_init(void);
void *__c33_iram_func(void *func);
void *c33_iram_alloc(size_t size);

/* The A0 RAM copy of @func, or @func itself if it has none. */
#define c33_iram_func(func)	((typeof(&(func)))__c33_iram_func(func))

#endif /* __ASM_C33_IRAM_H */
