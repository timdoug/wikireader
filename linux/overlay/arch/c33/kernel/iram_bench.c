// SPDX-License-Identifier: GPL-2.0
/*
 * Time the same code from SDRAM and from A0 RAM, to calibrate wremu's model
 * of the two against the device.  The loops are linked in SDRAM and copied
 * into A0 RAM, the string and division routines the other way round, so
 * each case runs the identical instructions twice.  Interrupts are off for
 * each timing, and get_cycles() counts MCLK.
 *
 *	echo 1 > /sys/module/iram_bench/parameters/run
 *
 * prints one line a case to the kernel log.
 */
#include <linux/irqflags.h>
#include <linux/moduleparam.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timex.h>

#include <asm/iram.h>
#include <asm/sections.h>

#include "iram_bench.h"

#define BUF_BYTES	4096
#define REPEAT		16

typedef void *(*copy_fn)(void *, const void *, size_t);
typedef void *(*set_fn)(void *, int, size_t);
typedef u32 (*div_fn)(u32, u32);

static void report(const char *name, bool a0, u32 cycles, u32 units,
		   const char *unit)
{
	pr_info("C33 bench %-12s %-5s %9u cycles, %4u.%02u a %s\n", name,
		a0 ? "A0" : "SDRAM", cycles, cycles / units,
		(cycles % units) * 100 / units, unit);
}

/* @fn's copy in the block that holds @from's bytes copied to @to. */
static void *moved(void *fn, const char *from, char *to)
{
	return to + ((char *)fn - from);
}

static void bench(bool a0, u8 *buf, u16 *table_a0, u16 *table_sdram,
		  char *loops_a0, char *iram_sdram)
{
	u8 *src = buf, *dst = buf + BUF_BYTES;
	const char *loops = c33_bench_loops_start, *iram = __iram_text_start;
	unsigned long flags;
	unsigned int i;
	u32 t, sink = 0;
	void (*alu)(unsigned int) = c33_bench_alu;
	void (*load)(const u32 *, unsigned int) = c33_bench_load;
	void (*store)(u32 *, unsigned int, u32) = c33_bench_store;
	u32 (*lookup)(const u32 *, unsigned int, const u16 *) = c33_bench_lookup;
	copy_fn copy = memcpy;
	set_fn set = memset;
	div_fn udiv = (div_fn)__udivsi3;

	if (a0) {
		alu = moved(alu, loops, loops_a0);
		load = moved(load, loops, loops_a0);
		store = moved(store, loops, loops_a0);
		lookup = moved(lookup, loops, loops_a0);
	} else {
		copy = moved(copy, iram, iram_sdram);
		set = moved(set, iram, iram_sdram);
		udiv = moved(udiv, iram, iram_sdram);
	}

	local_irq_save(flags);
	t = get_cycles();
	alu(4096);
	report("alu", a0, get_cycles() - t, 4096 * 16, "instruction");

	t = get_cycles();
	for (i = 0; i < REPEAT; i++)
		load((u32 *)src, BUF_BYTES / 32);
	report("load", a0, get_cycles() - t, REPEAT * BUF_BYTES / 4, "word");

	t = get_cycles();
	for (i = 0; i < REPEAT; i++)
		store((u32 *)dst, BUF_BYTES / 32, i);
	report("store", a0, get_cycles() - t, REPEAT * BUF_BYTES / 4, "word");

	t = get_cycles();
	for (i = 0; i < REPEAT; i++)
		copy(dst, src, BUF_BYTES);
	report("memcpy", a0, get_cycles() - t, REPEAT * BUF_BYTES, "byte");

	t = get_cycles();
	for (i = 0; i < REPEAT; i++)
		set(dst, i, BUF_BYTES);
	report("memset", a0, get_cycles() - t, REPEAT * BUF_BYTES, "byte");

	t = get_cycles();
	for (i = 0; i < REPEAT; i++)
		sink += lookup((u32 *)src, BUF_BYTES / 4, table_a0);
	report("lookup-A0tab", a0, get_cycles() - t, REPEAT * BUF_BYTES / 4,
	       "word");

	t = get_cycles();
	for (i = 0; i < REPEAT; i++)
		sink += lookup((u32 *)src, BUF_BYTES / 4, table_sdram);
	report("lookup-SDtab", a0, get_cycles() - t, REPEAT * BUF_BYTES / 4,
	       "word");

	t = get_cycles();
	for (i = 0; i < 4096; i++)
		sink += udiv(0x7654321 + i * 977, 47742 + i);
	report("udiv", a0, get_cycles() - t, 4096, "division");
	local_irq_restore(flags);

	/* Keep the results live. */
	if (sink == 0x5a5a5a5a)
		pr_info("C33 bench sink\n");
}

static int c33_bench_run(const char *val, const struct kernel_param *kp)
{
	size_t loops_size = c33_bench_loops_end - c33_bench_loops_start;
	size_t iram_size = __iram_text_end - __iram_text_start;
	static u16 *table_a0;
	static char *loops_a0;
	char *iram_sdram;
	u16 *table_sdram;
	u8 *buf;

	if (!table_a0)
		table_a0 = c33_iram_alloc(512);
	if (!loops_a0) {
		loops_a0 = c33_iram_alloc(loops_size);
		if (loops_a0)
			memcpy(loops_a0, c33_bench_loops_start, loops_size);
	}
	if (!table_a0 || !loops_a0) {
		/* The hot data and the SD driver's tables may have it all. */
		pr_info("C33 bench: no A0 RAM free for the A0 cases\n");
		return -ENOMEM;
	}
	buf = kmalloc(2 * BUF_BYTES, GFP_KERNEL);
	table_sdram = kmalloc(512, GFP_KERNEL);
	iram_sdram = kmalloc(iram_size, GFP_KERNEL);
	if (!buf || !table_sdram || !iram_sdram) {
		kfree(buf);
		kfree(table_sdram);
		kfree(iram_sdram);
		return -ENOMEM;
	}
	memcpy(iram_sdram, __iram_text_start, iram_size);
	memset(buf, 0x5a, 2 * BUF_BYTES);
	memset(table_sdram, 0x33, 512);
	memset(table_a0, 0x33, 512);
	pr_info("C33 bench: src %px dst %px, tables %px (A0) %px (SDRAM)\n",
		buf, buf + BUF_BYTES, table_a0, table_sdram);
	bench(false, buf, table_a0, table_sdram, loops_a0, iram_sdram);
	bench(true, buf, table_a0, table_sdram, loops_a0, iram_sdram);
	kfree(iram_sdram);
	kfree(table_sdram);
	kfree(buf);
	return 0;
}

static const struct kernel_param_ops c33_bench_ops = {
	.set = c33_bench_run,
};
module_param_cb(run, &c33_bench_ops, NULL, 0200);
