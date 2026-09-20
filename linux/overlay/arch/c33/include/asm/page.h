/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_PAGE_H
#define _ASM_C33_PAGE_H

#include <linux/const.h>

#define PAGE_SHIFT 12
#define PAGE_SIZE (_AC(1, UL) << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define PAGE_OFFSET CONFIG_PHYSICAL_START

#ifndef __ASSEMBLER__
#include <linux/types.h>

typedef struct { unsigned long pte; } pte_t;
typedef struct { unsigned long pmd; } pmd_t;
typedef struct { unsigned long pgd; } pgd_t;
typedef struct { unsigned long pgprot; } pgprot_t;
typedef pte_t *pgtable_t;

#define pte_val(x) ((x).pte)
#define pmd_val(x) ((x).pmd)
#define pgd_val(x) ((x).pgd)
#define pgprot_val(x) ((x).pgprot)
#define __pte(x) ((pte_t) { (x) })
#define __pmd(x) ((pmd_t) { (x) })
#define __pgd(x) ((pgd_t) { (x) })
#define __pgprot(x) ((pgprot_t) { (x) })

extern unsigned long memory_start;
extern unsigned long memory_end;

#define clear_page(page) memset((page), 0, PAGE_SIZE)
#define copy_page(to, from) memcpy((to), (from), PAGE_SIZE)
#define clear_user_page(page, vaddr, pg) clear_page(page)
#define copy_user_page(to, from, vaddr, pg) copy_page(to, from)

#define __pa(vaddr) ((unsigned long)(vaddr))
#define __va(paddr) ((void *)((unsigned long)(paddr)))
#define virt_to_pfn(kaddr) (__pa(kaddr) >> PAGE_SHIFT)
#define pfn_to_virt(pfn) __va((pfn) << PAGE_SHIFT)
#define virt_to_page(addr) (mem_map + (((unsigned long)(addr) - PAGE_OFFSET) >> PAGE_SHIFT))
#define page_to_virt(page) __va((((page) - mem_map) << PAGE_SHIFT) + PAGE_OFFSET)
#define virt_addr_valid(addr) ((unsigned long)(addr) >= memory_start && \
			      (unsigned long)(addr) < memory_end)
#define ARCH_PFN_OFFSET PFN_UP(CONFIG_PHYSICAL_START)
#endif

#include <asm-generic/getorder.h>
#include <asm-generic/memory_model.h>
#endif
