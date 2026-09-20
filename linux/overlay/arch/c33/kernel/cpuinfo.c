// SPDX-License-Identifier: GPL-2.0
#include <linux/delay.h>
#include <linux/seq_file.h>
#include <linux/smp.h>

static int c33_cpuinfo_show(struct seq_file *m, void *v)
{
	seq_printf(m,
		   "processor\t: 0\n"
		   "cpu family\t: Epson S1C33\n"
		   "cpu model\t: S1C33E07\n"
		   "MMU\t\t: none\n"
		   "clock\t\t: 60.00 MHz\n"
		   "BogoMIPS\t: %lu.%02lu\n",
		   (loops_per_jiffy * HZ) / 500000,
		   ((loops_per_jiffy * HZ) / 5000) % 100);
	return 0;
}

static void *c33_cpuinfo_start(struct seq_file *m, loff_t *pos)
{
	return *pos == 0 ? (void *)1 : NULL;
}

static void *c33_cpuinfo_next(struct seq_file *m, void *v, loff_t *pos)
{
	++*pos;
	return NULL;
}

static void c33_cpuinfo_stop(struct seq_file *m, void *v)
{
}

const struct seq_operations cpuinfo_op = {
	.start = c33_cpuinfo_start,
	.next = c33_cpuinfo_next,
	.stop = c33_cpuinfo_stop,
	.show = c33_cpuinfo_show,
};
