/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Code and data in the S1C33E07's A0 RAM, the on-chip RAM the CPU reaches
 * without the bus and without waiting.  Code in SDRAM runs from a fetch
 * queue that holds only a short loop; anything longer fetches from SDRAM as
 * it runs, at about 2.5 cycles an instruction word, and a data access waits
 * eight or nine cycles on average.  A0 RAM costs neither.
 *
 * A0 RAM is 8 KB at address zero.  Grifo keeps its own code below
 * C33_A0_DATA_START and its suspend scratch above C33_A0_DATA_END, which is
 * also where the TLS slot is; the kernel has the rest.  The LCD controller's
 * 12 KB of IVRAM joins it at C33_A0_CODE_START when the controller's IRAM
 * bit is set (the S1C33E07 manual, I.6.2); the panel is scanned from SDRAM,
 * so the kernel takes IVRAM for this.
 *
 * The kernel's hottest functions and objects, chosen from wremu profiles
 * (tools in linux/a0-hot.py), are linked to run there: the .a0.text and
 * .a0.data output sections.  The kernel image carries them in SDRAM, and
 * head.S moves the panel's picture out of IVRAM, switches IVRAM into area
 * 0 and copies both sections into place before any C runs.  Nothing in them
 * may be a DMA buffer: A0 RAM cannot be one.
 *
 * __iramfunc puts a function there by hand.  Those in .iram.text are also
 * position-independent, with PC-relative branches and no calls or data
 * addresses, so that the A0 RAM benchmark can run copies of them from
 * SDRAM.  c33_iram_alloc() hands out what A0 RAM the data section leaves,
 * for tables.  Modules get neither.
 */
#ifndef __ASM_C33_IRAM_H
#define __ASM_C33_IRAM_H

#include <uapi/asm/tls.h>

#define C33_A0_DATA_START	0x00000c00
#define C33_A0_DATA_END		C33_TLS_SLOT_ADDRESS
#define C33_A0_CODE_START	0x00002000
#define C33_A0_CODE_END		0x00005000

/* The LCD controller's IVRAM location bit, and the main window's registers. */
#define C33_LCDC_IRAM		0x00301a64
#define C33_LCDC_MADD		0x00301a70
#define C33_LCDC_MLADD		0x00301a74
#define C33_IVRAM_BASE		0x00080000
#define C33_IVRAM_SIZE		0x00003000

#ifndef __ASSEMBLY__
#include <linux/compiler.h>
#include <linux/types.h>

#ifdef MODULE
#define __iramfunc
#else
/*
 * flatten: what the function calls is inlined into it, so it does not
 * fetch its callees from SDRAM, and runs from any copy.  noclone: a
 * specialised copy for some callers would take A0 RAM again.
 */
#define __iramfunc	__section(".iram.text") noinline \
			__attribute__((noclone, flatten))
#endif

void c33_iram_init(void);
void *c33_iram_alloc(size_t size);

/* What Grifo's panel was scanning when it started the kernel (head.S). */
extern u32 c33_boot_madd, c33_boot_mladd;
#endif

#endif /* __ASM_C33_IRAM_H */
