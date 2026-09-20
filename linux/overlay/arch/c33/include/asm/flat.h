/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_FLAT_H
#define _ASM_C33_FLAT_H

#include <asm-generic/flat.h>

/* The ABI reserves r15 as the default-data-area base. */
#define FLAT_PLAT_INIT(regs) \
	do { \
		if (current->mm) \
			(regs)->r[15] = current->mm->start_data; \
	} while (0)

#endif
