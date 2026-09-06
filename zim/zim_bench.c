/* On-device timing for the ZIM reader; see zim_bench.h. */
#include "zim_bench.h"

#include <grifo.h>
#include <regs.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BENCH_FILE "bench.txt"
#define TICKS_PER_US TIMER_CountsPerMicroSecond

static unsigned long marks[ZIM_BENCH_MARKS];
static unsigned long slot_ticks[ZIM_BENCH_SLOTS];
static unsigned long slot_bytes[ZIM_BENCH_SLOTS];
static unsigned long slot_calls[ZIM_BENCH_SLOTS];
static size_t size_raw, size_text, size_stream;
static uint32_t pending_index;
static int pending;
static int cached;

/* One line to the console and to the end of bench.txt. */
static void bench_line(const char *format, ...)
{
	char line[200];
	unsigned long length;
	va_list ap;
	int handle;
	int n;

	va_start(ap, format);
	n = vsnprintf(line, sizeof(line) - 1, format, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n > sizeof(line) - 2)
		n = sizeof(line) - 2;
	line[n++] = '\n';
	line[n] = '\0';
	debug_print(line);

	if (file_size(BENCH_FILE, &length) == FILE_ERROR_OK) {
		handle = file_open(BENCH_FILE, FILE_OPEN_WRITE);
		if (handle >= 0 && file_lseek(handle, length) != FILE_ERROR_OK) {
			file_close(handle);
			handle = -1;
		}
	} else {
		handle = file_create(BENCH_FILE, FILE_OPEN_WRITE);
	}
	if (handle < 0)
		return;
	file_write(handle, line, (size_t)n);
	file_close(handle);
}

/* Milliseconds with one decimal, from timer ticks. */
static unsigned long tenths_ms(unsigned long ticks)
{
	return ticks / (TICKS_PER_US * 100);
}

#define MS(ticks) tenths_ms(ticks) / 10, tenths_ms(ticks) % 10

/* Cycles per operation with one decimal; the timer counts MCLK cycles. */
static void report(const char *name, unsigned long count, unsigned long ticks)
{
	unsigned long tenths = count ? (ticks * 10 + count / 2) / count : 0;

	bench_line("bench %-14s %10lu ops %7lu.%lu ms %6lu.%lu cyc/op",
		   name, count, MS(ticks), tenths / 10, tenths % 10);
}

/* --- micro-benchmarks --------------------------------------------------- */

static unsigned long run_cpu(unsigned long n)
{
	unsigned long t = timer_get();
	unsigned long count = n;

	/* Two instructions an iteration, no memory operand. */
	__asm__ volatile ("1:\n\tsub\t%0, 1\n\tjrne\t1b" : "+r"(count));
	return timer_get() - t;
}

static unsigned long run_fetch(unsigned long n)
{
	unsigned long t = timer_get();
	unsigned long count = n;

	unsigned int scratch = 0;

	/* 1 KB of straight-line code an iteration: instruction fetch cost.
	 * A register move rather than nop, whose all-zero encoding the
	 * emulator's runaway guard would stop on. */
	__asm__ volatile ("1:\n\t.rept 512\n\tld.w\t%1, %1\n\t.endr\n\t"
			  "sub\t%0, 1\n\txjrne\t1b"
			  : "+r"(count), "+r"(scratch));
	return timer_get() - t;
}

static unsigned long run_read_words(unsigned char *p, unsigned long n)
{
	unsigned long t = timer_get();
	unsigned long count = n;
	unsigned int scratch;

	__asm__ volatile ("1:\n\tld.w\t%2, [%0]+\n\tsub\t%1, 1\n\tjrne\t1b"
			  : "+r"(p), "+r"(count), "=&r"(scratch) : : "memory");
	return timer_get() - t;
}

static unsigned long run_read_bytes(unsigned char *p, unsigned long n)
{
	unsigned long t = timer_get();
	unsigned long count = n;
	unsigned int scratch;

	__asm__ volatile ("1:\n\tld.ub\t%2, [%0]+\n\tsub\t%1, 1\n\tjrne\t1b"
			  : "+r"(p), "+r"(count), "=&r"(scratch) : : "memory");
	return timer_get() - t;
}

static unsigned long run_write_words(unsigned char *p, unsigned long n)
{
	unsigned long t = timer_get();
	unsigned long count = n;
	unsigned int value = 0x5a5a5a5a;

	__asm__ volatile ("1:\n\tld.w\t[%0]+, %2\n\tsub\t%1, 1\n\tjrne\t1b"
			  : "+r"(p), "+r"(count) : "r"(value) : "memory");
	return timer_get() - t;
}

static unsigned long run_write_bytes(unsigned char *p, unsigned long n)
{
	unsigned long t = timer_get();
	unsigned long count = n;
	unsigned int value = 0x5a;

	__asm__ volatile ("1:\n\tld.b\t[%0]+, %2\n\tsub\t%1, 1\n\tjrne\t1b"
			  : "+r"(p), "+r"(count) : "r"(value) : "memory");
	return timer_get() - t;
}

/* Alternate word reads between two addresses: the cost of whatever the
 * controller has to do between them (nothing, a row change, or a bank
 * switch). */
static unsigned long run_pairs(const unsigned char *a, const unsigned char *b,
			       unsigned long n)
{
	unsigned long t = timer_get();
	unsigned long count = n;
	unsigned int scratch;

	__asm__ volatile ("1:\n\tld.w\t%3, [%0]\n\tld.w\t%3, [%1]\n\t"
			  "sub\t%2, 1\n\tjrne\t1b"
			  : "+r"(a), "+r"(b), "+r"(count), "=&r"(scratch)
			  : : "memory");
	return timer_get() - t;
}

/* Alternate a word write to one row with a word read from another. */
static unsigned long run_write_read_pairs(unsigned char *a,
					  const unsigned char *b,
					  unsigned long n)
{
	unsigned long t = timer_get();
	unsigned long count = n;
	unsigned int scratch;

	__asm__ volatile ("1:\n\tld.w\t[%0], %3\n\tld.w\t%3, [%1]\n\t"
			  "sub\t%2, 1\n\tjrne\t1b"
			  : "+r"(a), "+r"(b), "+r"(count), "=&r"(scratch)
			  : : "memory");
	return timer_get() - t;
}

static unsigned long run_copy_bytes(unsigned char *d, const unsigned char *s,
				    unsigned long n)
{
	unsigned long t = timer_get();
	unsigned long count = n;
	unsigned int scratch;

	/* Byte at a time, load then store, as a C loop compiles. */
	__asm__ volatile ("1:\n\tld.ub\t%3, [%1]+\n\tsub\t%2, 1\n\t"
			  "ld.b\t[%0]+, %3\n\tjrne\t1b"
			  : "+r"(d), "+r"(s), "+r"(count), "=&r"(scratch)
			  : : "memory");
	return timer_get() - t;
}

static unsigned long run_copy_batch8(unsigned char *d, const unsigned char *s,
				     unsigned long n)
{
	unsigned long t = timer_get();
	long count = (long)n;   /* bytes; the loop steps by eight */
	unsigned int t0, t1, t2, t3, t4, t5, t6, t7;

	/* Eight loads, then eight stores: the decoder's batched copy. */
	__asm__ volatile ("1:\n\t"
			  "ld.ub\t%3, [%1]+\n\tld.ub\t%4, [%1]+\n\t"
			  "ld.ub\t%5, [%1]+\n\tld.ub\t%6, [%1]+\n\t"
			  "ld.ub\t%7, [%1]+\n\tld.ub\t%8, [%1]+\n\t"
			  "ld.ub\t%9, [%1]+\n\tld.ub\t%10, [%1]+\n\t"
			  "ld.b\t[%0]+, %3\n\tld.b\t[%0]+, %4\n\t"
			  "ld.b\t[%0]+, %5\n\tld.b\t[%0]+, %6\n\t"
			  "ld.b\t[%0]+, %7\n\tld.b\t[%0]+, %8\n\t"
			  "ld.b\t[%0]+, %9\n\tsub\t%2, 8\n\t"
			  "ld.b\t[%0]+, %10\n\tjrgt\t1b"
			  : "+r"(d), "+r"(s), "+r"(count),
			    "=&r"(t0), "=&r"(t1), "=&r"(t2), "=&r"(t3),
			    "=&r"(t4), "=&r"(t5), "=&r"(t6), "=&r"(t7)
			  : : "memory");
	return timer_get() - t;
}

/* The same two tests executed from the application's A0 RAM area
 * (.fastcode, see samo-lib/grifo/lds/application.lds): what an
 * instruction fetch costs there.  They take no timer calls inside so the
 * code needs nothing outside itself; the 256 register moves are 512 bytes,
 * what the area has left beside the decoder. */
#define BENCH_FASTCODE __attribute__((section(".fastcode"), noinline))

static void BENCH_FASTCODE a0_cpu(unsigned long count)
{
	__asm__ volatile ("1:\n\tsub\t%0, 1\n\tjrne\t1b" : "+r"(count));
}

static void BENCH_FASTCODE a0_fetch(unsigned long count)
{
	unsigned int scratch = 0;

	__asm__ volatile ("1:\n\t.rept 256\n\tld.w\t%1, %1\n\t.endr\n\t"
			  "sub\t%0, 1\n\txjrne\t1b"
			  : "+r"(count), "+r"(scratch));
}

static unsigned long run_a0_cpu(unsigned long n)
{
	unsigned long t = timer_get();

	a0_cpu(n);
	return timer_get() - t;
}

static unsigned long run_a0_fetch(unsigned long n)
{
	unsigned long t = timer_get();

	a0_fetch(n);
	return timer_get() - t;
}

static unsigned long run_card(zim_bench_read_fn read, void *opaque,
			      uint64_t offset, unsigned char *buffer,
			      size_t length, unsigned reads, uint64_t stride)
{
	unsigned long t = timer_get();
	unsigned i;

	for (i = 0; i < reads; i++)
		if (read(opaque, offset + (uint64_t)i * stride, buffer, length))
			return 0;
	return timer_get() - t;
}

#define BENCH_BUFFER (1024u * 1024u + 8192u)

void zim_bench_startup(zim_bench_read_fn read, void *opaque, uint64_t size)
{
	unsigned char *raw = malloc(BENCH_BUFFER);
	unsigned char *buffer;
	unsigned char *half;
	unsigned long ticks;
	volatile unsigned int on_stack[2] = { 1, 2 };

	bench_line("bench start: sdram ctl 0x%08lx ref 0x%08lx app 0x%08lx, "
		   "timer %u ticks/us",
		   (unsigned long)REG_SDRAMC_CTL, (unsigned long)REG_SDRAMC_REF,
		   (unsigned long)REG_SDRAMC_APP, (unsigned)TICKS_PER_US);
	if (!raw) {
		bench_line("bench: no memory for the buffer");
		return;
	}
	buffer = (unsigned char *)(((uintptr_t)raw + 4095) & ~(uintptr_t)4095);
	half = buffer + 512 * 1024;
	bench_line("bench buffer at 0x%08lx, stack near 0x%08lx",
		   (unsigned long)(uintptr_t)buffer,
		   (unsigned long)(uintptr_t)on_stack);

	report("cpu-loop", 3000000, run_cpu(3000000));
	report("fetch-1k", 512 * 256, run_fetch(256));
	report("cpu-loop-a0", 3000000, run_a0_cpu(3000000));
	report("fetch-a0", 256 * 512, run_a0_fetch(512));
	report("read-words", 262144, run_read_words(buffer, 262144));
	report("read-bytes", 262144, run_read_bytes(buffer, 262144));
	report("write-words", 262144, run_write_words(buffer, 262144));
	report("write-bytes", 262144, run_write_bytes(buffer, 262144));
	/* 1 KiB rows on the 16 MB boards: +64 stays in the row, +2048 does
	 * not; the stack is in another bank. */
	report("pair-same-row", 100000, run_pairs(buffer, buffer + 64, 100000));
	report("pair-row-change", 100000,
	       run_pairs(buffer, buffer + 2048, 100000));
	report("pair-two-banks", 100000,
	       run_pairs(buffer, (const unsigned char *)on_stack, 100000));
	report("pair-write-read", 100000,
	       run_write_read_pairs(buffer, buffer + 2048, 100000));
	report("copy-bytes-512k", 512 * 1024,
	       run_copy_bytes(half, buffer, 512 * 1024));
	report("copy-batch8-512k", 512 * 1024,
	       run_copy_batch8(half, buffer, 512 * 1024));
	ticks = timer_get();
	memcpy(half, buffer, 512 * 1024);
	report("memcpy-512k", 512 * 1024, timer_get() - ticks);

	if (read && size >= 16u * 1024 * 1024) {
		/* Sequential 256 KiB well past anything cached, twice (the
		 * card may cache the second), then 64 reads of 4 KiB a
		 * megabyte apart for the per-command cost. */
		report("card-256k", 512,
		       run_card(read, opaque, 8u * 1024 * 1024, buffer,
				256 * 1024, 1, 0));
		report("card-256k-again", 512,
		       run_card(read, opaque, 8u * 1024 * 1024, buffer,
				256 * 1024, 1, 0));
		report("card-4k-x64", 64 * 8,
		       run_card(read, opaque, 4u * 1024 * 1024, buffer,
				4096, 64, 1024 * 1024));
	}
	free(raw);
	bench_line("bench columns: article <index> total blob(card KB reads,"
		   " zstd calls) html wrap paint ms, raw text stream bytes");
}

/* --- article loads ------------------------------------------------------ */

void zim_bench_article_begin(uint32_t index)
{
	memset(marks, 0, sizeof(marks));
	memset(slot_ticks, 0, sizeof(slot_ticks));
	memset(slot_bytes, 0, sizeof(slot_bytes));
	memset(slot_calls, 0, sizeof(slot_calls));
	size_raw = size_text = size_stream = 0;
	pending_index = index;
	pending = 1;
	cached = 0;
	marks[ZIM_BENCH_MARK_START] = timer_get();
}

void zim_bench_article_cached(void)
{
	cached = 1;
}

void zim_bench_article_sizes(size_t raw, size_t text, size_t stream)
{
	size_raw = raw;
	size_text = text;
	size_stream = stream;
}

void zim_bench_mark(int mark)
{
	if (pending && mark > 0 && mark < ZIM_BENCH_MARKS)
		marks[mark] = timer_get();
}

void zim_bench_account(int slot, unsigned long ticks, size_t bytes)
{
	if (pending && slot >= 0 && slot < ZIM_BENCH_SLOTS) {
		slot_ticks[slot] += ticks;
		slot_bytes[slot] += bytes;
		slot_calls[slot]++;
	}
}

void zim_bench_painted(void)
{
	unsigned long start, total, blob, html, wrap, paint;

	if (!pending)
		return;
	pending = 0;
	marks[ZIM_BENCH_MARK_PAINT] = timer_get();
	start = marks[ZIM_BENCH_MARK_START];
	total = marks[ZIM_BENCH_MARK_PAINT] - start;
	if (cached) {
		bench_line("article %lu cached total %lu.%lu ms",
			   (unsigned long)pending_index, MS(total));
		return;
	}
	blob = marks[ZIM_BENCH_MARK_BLOB] - start;
	html = marks[ZIM_BENCH_MARK_HTML] - marks[ZIM_BENCH_MARK_BLOB];
	wrap = marks[ZIM_BENCH_MARK_WRAP] - marks[ZIM_BENCH_MARK_HTML];
	paint = marks[ZIM_BENCH_MARK_PAINT] - marks[ZIM_BENCH_MARK_WRAP];
	bench_line("article %lu total %lu.%lu blob %lu.%lu (card %lu.%lu %luK %lu, "
		   "zstd %lu.%lu %lu) html %lu.%lu wrap %lu.%lu paint %lu.%lu ms, "
		   "%lu %lu %lu bytes",
		   (unsigned long)pending_index, MS(total), MS(blob),
		   MS(slot_ticks[ZIM_BENCH_SLOT_CARD]),
		   slot_bytes[ZIM_BENCH_SLOT_CARD] / 1024,
		   slot_calls[ZIM_BENCH_SLOT_CARD],
		   MS(slot_ticks[ZIM_BENCH_SLOT_ZSTD]),
		   slot_calls[ZIM_BENCH_SLOT_ZSTD],
		   MS(html), MS(wrap), MS(paint),
		   (unsigned long)size_raw, (unsigned long)size_text,
		   (unsigned long)size_stream);
}
