// SPDX-License-Identifier: GPL-2.0
/* A0 RAM for hot code and its data: see asm/iram.h. */
#include <linux/align.h>
#include <linux/cache.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#include <asm/iram.h>

extern char __iram_text_start[], __iram_text_end[];

static unsigned long c33_iram_next = C33_IRAM_START;
static DEFINE_SPINLOCK(c33_iram_lock);

void __init c33_iram_init(void)
{
	size_t size = __iram_text_end - __iram_text_start;

	if (C33_IRAM_START + size > C33_IRAM_END) {
		pr_err("C33 A0 RAM: %zu bytes of code do not fit\n", size);
		return;
	}
	memcpy((void *)C33_IRAM_START, __iram_text_start, size);
	c33_iram_next = ALIGN(C33_IRAM_START + size, 4);
}

void *__c33_iram_func(void *func)
{
	char *f = func;

	if (f < __iram_text_start || f >= __iram_text_end ||
	    c33_iram_next == C33_IRAM_START)
		return func;
	return (void *)(C33_IRAM_START + (f - __iram_text_start));
}

/* Never freed: what uses A0 RAM keeps it. */
void *c33_iram_alloc(size_t size)
{
	unsigned long start;
	void *mem = NULL;

	spin_lock(&c33_iram_lock);
	start = ALIGN(c33_iram_next, 4);
	if (start + size <= C33_IRAM_END) {
		mem = (void *)start;
		c33_iram_next = start + size;
	}
	spin_unlock(&c33_iram_lock);
	return mem;
}
