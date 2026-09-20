/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_FLAT_H
#define _ASM_C33_FLAT_H

#include <linux/swab.h>
#include <linux/unaligned.h>

/*
 * A C33 absolute address is split across two ext instructions and the
 * six-bit immediate of the following instruction.  The bFLT converter marks
 * such a relocation with the top bit of the relocation-table entry.
 */
#define C33_FLAT_SPLIT_RELOC	0x80000000U

static inline int flat_get_addr_from_rp(u32 __user *rp, u32 relval, u32 flags,
					u32 *addr)
{
	void *p = (__force void *)rp;
	u32 value;

	if (relval & C33_FLAT_SPLIT_RELOC) {
		u16 high = get_unaligned((u16 *)p);
		u16 middle = get_unaligned((u16 *)p + 1);
		u16 low = get_unaligned((u16 *)p + 2);

		value = ((high & 0x1fff) << 19) |
			((middle & 0x1fff) << 6) | ((low >> 4) & 0x3f);
	} else {
		value = get_unaligned((u32 *)p);
	}

	/* binfmt_flat applies ntohl() to non-GOT relocation values. */
	*addr = swab32(value);
	return 0;
}

static inline int flat_put_addr_at_rp(u32 __user *rp, u32 addr, u32 relval)
{
	void *p = (__force void *)rp;

	if (relval & C33_FLAT_SPLIT_RELOC) {
		u16 high = get_unaligned((u16 *)p);
		u16 middle = get_unaligned((u16 *)p + 1);
		u16 low = get_unaligned((u16 *)p + 2);

		high = (high & 0xe000) | ((addr >> 19) & 0x1fff);
		middle = (middle & 0xe000) | ((addr >> 6) & 0x1fff);
		low = (low & 0xfc0f) | ((addr & 0x3f) << 4);
		put_unaligned(high, (u16 *)p);
		put_unaligned(middle, (u16 *)p + 1);
		put_unaligned(low, (u16 *)p + 2);
	} else {
		put_unaligned(addr, (u32 *)p);
	}

	return 0;
}

#define flat_get_relocate_addr(rel)	((rel) & ~C33_FLAT_SPLIT_RELOC)

/* The ABI reserves r15 as the default-data-area base. */
#define FLAT_PLAT_INIT(regs) \
	do { \
		if (current->mm) \
			(regs)->r[15] = current->mm->start_data; \
	} while (0)

#endif
