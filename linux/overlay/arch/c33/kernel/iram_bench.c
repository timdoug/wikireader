// SPDX-License-Identifier: GPL-2.0
/*
 * Time the same code from SDRAM and from A0 RAM, to calibrate wremu's model
 * of the two against the device.  Every __iramfunc exists in both places,
 * so each case runs the identical instructions twice.  Interrupts are off
 * for each timing, and get_cycles() counts MCLK.
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

void c33_bench_alu(unsigned int passes);
void c33_bench_load(const u32 *p, unsigned int blocks);
void c33_bench_store(u32 *p, unsigned int blocks, u32 v);
u32 c33_bench_lookup(const u32 *p, unsigned int words, const u16 *table);
extern char __c33_memcpy[], __c33_memset[], __c33_udivsi3[];

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

static void bench(bool a0, u8 *buf, u16 *table_a0, u16 *table_sdram)
{
	u8 *src = buf, *dst = buf + BUF_BYTES;
	unsigned long flags;
	unsigned int i;
	u32 t, sink = 0;
	void (*alu)(unsigned int) = c33_bench_alu;
	void (*load)(const u32 *, unsigned int) = c33_bench_load;
	void (*store)(u32 *, unsigned int, u32) = c33_bench_store;
	u32 (*lookup)(const u32 *, unsigned int, const u16 *) = c33_bench_lookup;
	copy_fn copy = (copy_fn)__c33_memcpy;
	set_fn set = (set_fn)__c33_memset;
	div_fn udiv = (div_fn)__c33_udivsi3;

	if (a0) {
		alu = c33_iram_func(c33_bench_alu);
		load = c33_iram_func(c33_bench_load);
		store = c33_iram_func(c33_bench_store);
		lookup = c33_iram_func(c33_bench_lookup);
		copy = (copy_fn)__c33_iram_func(__c33_memcpy);
		set = (set_fn)__c33_iram_func(__c33_memset);
		udiv = (div_fn)__c33_iram_func(__c33_udivsi3);
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
	static u16 *table_a0;
	u16 *table_sdram;
	u8 *buf;

	if (!table_a0)
		table_a0 = c33_iram_alloc(512);
	buf = kmalloc(2 * BUF_BYTES, GFP_KERNEL);
	table_sdram = kmalloc(512, GFP_KERNEL);
	if (!table_a0 || !buf || !table_sdram) {
		kfree(buf);
		kfree(table_sdram);
		return -ENOMEM;
	}
	memset(buf, 0x5a, 2 * BUF_BYTES);
	memset(table_sdram, 0x33, 512);
	memset(table_a0, 0x33, 512);
	pr_info("C33 bench: src %px dst %px, tables %px (A0) %px (SDRAM)\n",
		buf, buf + BUF_BYTES, table_a0, table_sdram);
	bench(false, buf, table_a0, table_sdram);
	bench(true, buf, table_a0, table_sdram);
	kfree(table_sdram);
	kfree(buf);
	return 0;
}

static const struct kernel_param_ops c33_bench_ops = {
	.set = c33_bench_run,
};
module_param_cb(run, &c33_bench_ops, NULL, 0200);
