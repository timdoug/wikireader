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
extern char __c33_memset[], __c33_memcpy[], __c33_memmove[];
extern void *c33_memset_fn, *c33_memcpy_fn, *c33_memmove_fn;
extern char __c33_udivsi3[], __c33_umodsi3[], __c33_div64_32[];
extern void *c33_udivsi3_fn, *c33_umodsi3_fn, *c33_div64_32_fn;

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

	/* From here these run from their copies: lib/string.S and lib/div.S. */
	c33_memset_fn = __c33_iram_func(__c33_memset);
	c33_memcpy_fn = __c33_iram_func(__c33_memcpy);
	c33_memmove_fn = __c33_iram_func(__c33_memmove);
	c33_udivsi3_fn = __c33_iram_func(__c33_udivsi3);
	c33_umodsi3_fn = __c33_iram_func(__c33_umodsi3);
	c33_div64_32_fn = __c33_iram_func(__c33_div64_32);
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
