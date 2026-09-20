/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_PGTABLE_H
#define _ASM_C33_PGTABLE_H

#include <asm-generic/pgtable-nopud.h>
#include <asm/page.h>

#define pgd_present(pgd) (1)
#define pgd_none(pgd) (0)
#define pgd_bad(pgd) (0)
#define pgd_clear(pgdp) do { } while (0)
#define pmd_offset(a, b) ((void *)0)

#define PAGE_NONE __pgprot(0)
#define PAGE_SHARED __pgprot(0)
#define PAGE_COPY __pgprot(0)
#define PAGE_READONLY __pgprot(0)
#define PAGE_KERNEL __pgprot(0)

#define swapper_pg_dir ((pgd_t *)0)

extern void *empty_zero_page;
#define ZERO_PAGE(vaddr) (virt_to_page(empty_zero_page))

#define VMALLOC_START 0
#define VMALLOC_END 0xffffffffUL
#define KMAP_START 0
#define KMAP_END 0xffffffffUL

extern void paging_init(void);
#endif
