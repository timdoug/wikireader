// SPDX-License-Identifier: GPL-2.0
/* A0 RAM for hot code and data: see asm/iram.h. */
#include <linux/align.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/spinlock.h>

#include <asm/iram.h>
#include <asm/sections.h>

static unsigned long c33_iram_next;
static DEFINE_SPINLOCK(c33_iram_lock);

void __init c33_iram_init(void)
{
	c33_iram_next = ALIGN((unsigned long)__a0_data_end, 4);
	pr_info("C33 A0 RAM: %u bytes of code, %u of data, %lu free\n",
		(unsigned int)(__a0_text_end - __a0_text_start),
		(unsigned int)(__a0_data_end - __a0_data_start),
		C33_A0_DATA_END - c33_iram_next);
}

/* Never freed: what uses A0 RAM keeps it. */
void *c33_iram_alloc(size_t size)
{
	unsigned long start;
	void *mem = NULL;

	spin_lock(&c33_iram_lock);
	start = ALIGN(c33_iram_next, 4);
	if (start + size <= C33_A0_DATA_END) {
		mem = (void *)start;
		c33_iram_next = start + size;
	}
	spin_unlock(&c33_iram_lock);
	return mem;
}
