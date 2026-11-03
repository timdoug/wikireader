/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _C33_KERNEL_IRAM_BENCH_H
#define _C33_KERNEL_IRAM_BENCH_H

#include <linux/types.h>

/* The loops iram_bench.c times: iram_bench_loops.S. */
void c33_bench_alu(unsigned int passes);
void c33_bench_load(const u32 *p, unsigned int blocks);
void c33_bench_store(u32 *p, unsigned int blocks, u32 v);
u32 c33_bench_lookup(const u32 *p, unsigned int words, const u16 *table);
extern char c33_bench_loops_start[], c33_bench_loops_end[];
/* lib/div.S, in A0 RAM. */
u32 __udivsi3(u32 n, u32 d);

#endif /* _C33_KERNEL_IRAM_BENCH_H */
