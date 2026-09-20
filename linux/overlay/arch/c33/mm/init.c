// SPDX-License-Identifier: GPL-2.0
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/memblock.h>
#include <linux/mm.h>

void *empty_zero_page;
EXPORT_SYMBOL(empty_zero_page);

void __init paging_init(void)
{
	unsigned long max_zone_pfn[MAX_NR_ZONES] = { 0 };

	high_memory = (void *)(memory_end & PAGE_MASK);
	empty_zero_page = memblock_alloc_or_panic(PAGE_SIZE, PAGE_SIZE);
	max_zone_pfn[ZONE_NORMAL] = memory_end >> PAGE_SHIFT;
	free_area_init(max_zone_pfn);
}

void mem_init(void)
{
}

void free_initmem(void)
{
	free_initmem_default(-1);
}
