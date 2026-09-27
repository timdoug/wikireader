// SPDX-License-Identifier: GPL-2.0
#include <linux/gfp.h>
#include <linux/init.h>
#include <linux/memblock.h>
#include <linux/mm.h>

void __init paging_init(void)
{
	high_memory = (void *)(memory_end & PAGE_MASK);
}

void __init arch_zone_limits_init(unsigned long *max_zone_pfn)
{
	max_zone_pfn[ZONE_NORMAL] = memory_end >> PAGE_SHIFT;
}

void free_initmem(void)
{
	free_initmem_default(-1);
}
