/* On-device timing for the ZIM reader; see zim_bench.h. */
#include "zim_bench.h"

#include <grifo.h>
#include <regs.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ZIM_BENCH_AB
#include "zim_catalog.h"
#include "wiki_info.h"
#define BENCH_FILE "ab-" ZIM_BENCH_VARIANT ".txt"
#else
#define BENCH_FILE "bench.txt"
#endif
#define TICKS_PER_US TIMER_CountsPerMicroSecond

static unsigned long marks[ZIM_BENCH_MARKS];
static unsigned long slot_ticks[ZIM_BENCH_SLOTS];
static unsigned long slot_bytes[ZIM_BENCH_SLOTS];
static unsigned long slot_calls[ZIM_BENCH_SLOTS];
static size_t size_raw, size_text, size_stream;
static uint32_t pending_index;
static int pending;
static int cached;
static int blob_pending;

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

#ifdef ZIM_BENCH_AB
static unsigned long boot_start;

void zim_bench_boot_begin(void)
{
	boot_start = timer_get();
}

void zim_bench_boot_ready(void)
{
	/* Capture before formatting or writing any benchmark output. This
	 * ends at the same point as the normal app's startup profile: fonts
	 * loaded, just before the first search-renderer call. */
	unsigned long elapsed = timer_get() - boot_start;
	const char *path = zim_catalog_path(nCurrentWiki);

	bench_line("bench variant: %s; build: %s %s gcc %s",
		ZIM_BENCH_VARIANT, __DATE__, __TIME__, __VERSION__);
	bench_line("startup initialize %lu.%lu ms, archive %s",
		MS(elapsed), path ? path : "none");
	bench_line("bench start: sdram ctl 0x%08lx ref 0x%08lx app 0x%08lx, "
		"timer %u ticks/us", (unsigned long)REG_SDRAMC_CTL,
		(unsigned long)REG_SDRAMC_REF, (unsigned long)REG_SDRAMC_APP,
		(unsigned)TICKS_PER_US);
}
#endif

#ifndef ZIM_BENCH_AB
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

/* The same two tests executed from internal RAM: what an instruction fetch
 * costs there. The code is position independent (local labels, a register
 * argument in %r6, no calls). Keep its source in SDRAM and copy it to each
 * test region: the decoder now fills almost all of the application's A0
 * RAM, so the startup-only test saves and restores the bytes it borrows. */
__asm__(".section .text.bench_pic,\"ax\"\n"
	".global bench_pic_start\n"
	"bench_pic_start:\n"
	".global bench_pic_cpu\n"
	"bench_pic_cpu:\n"
	"1:\n\tsub\t%r6, 1\n\tjrne\t1b\n\tret\n"
	".global bench_pic_fetch\n"
	"bench_pic_fetch:\n"
	"1:\n\t.rept 256\n\tld.w\t%r7, %r7\n\t.endr\n\t"
	"sub\t%r6, 1\n\txjrne\t1b\n\tret\n"
	/* Data accesses issued by code running from internal RAM: %r6 counts,
	   %r7 and %r8 point into SDRAM. */
	".global bench_pic_load\n"
	"bench_pic_load:\n"
	"1:\n\t.rept 8\n\tld.w\t%r9, [%r7]\n\t.endr\n\t"
	"sub\t%r6, 1\n\tjrne\t1b\n\tret\n"
	".global bench_pic_load2\n"
	"bench_pic_load2:\n"
	"1:\n\t.rept 4\n\tld.w\t%r9, [%r7]\n\tld.w\t%r9, [%r8]\n\t.endr\n\t"
	"sub\t%r6, 1\n\tjrne\t1b\n\tret\n"
	".global bench_pic_storeload\n"
	"bench_pic_storeload:\n"
	"1:\n\t.rept 4\n\tld.w\t[%r7], %r9\n\tld.w\t%r9, [%r7]\n\t.endr\n\t"
	"sub\t%r6, 1\n\tjrne\t1b\n\tret\n"
	/* A load whose value the next instruction uses, against the same
	   loads with an independent add between them: the difference is the
	   pipeline interlock, which real code pays on nearly every load and
	   no streaming benchmark shows. */
	".global bench_pic_loadindep\n"
	"bench_pic_loadindep:\n"
	"1:\n\t.rept 4\n\tld.w\t%r9, [%r7]\n\tadd\t%r4, %r5\n\t.endr\n\t"
	"sub\t%r6, 1\n\tjrne\t1b\n\tret\n"
	".global bench_pic_loaduse\n"
	"bench_pic_loaduse:\n"
	"1:\n\t.rept 4\n\tld.w\t%r9, [%r7]\n\tadd\t%r4, %r9\n\t.endr\n\t"
	"sub\t%r6, 1\n\tjrne\t1b\n\tret\n"
	".global bench_pic_end\n"
	"bench_pic_end:\n"
	".section .text\n");

extern const unsigned char bench_pic_start[], bench_pic_cpu[],
	bench_pic_fetch[], bench_pic_load[], bench_pic_load2[],
	bench_pic_storeload[], bench_pic_loadindep[], bench_pic_loaduse[],
	bench_pic_end[];

typedef void (*bench_pic_fn)(unsigned long count);
typedef void (*bench_pic_data_fn)(unsigned long count, void *a, void *b);

/* Run one of the position-independent tests from a copied code block. */
static unsigned long run_pic(const unsigned char *base, const unsigned char *which,
			     unsigned long n)
{
	bench_pic_fn fn = (bench_pic_fn)(base + (which - bench_pic_start));
	unsigned long t = timer_get();

	fn(n);
	return timer_get() - t;
}

/* The same, for the tests that touch memory. */
static unsigned long run_pic_data(const unsigned char *base,
				  const unsigned char *which, unsigned long n,
				  void *a, void *b)
{
	bench_pic_data_fn fn =
		(bench_pic_data_fn)(base + (which - bench_pic_start));
	unsigned long t = timer_get();

	fn(n, a, b);
	return timer_get() - t;
}

static const unsigned char *pic_copy(unsigned char *destination)
{
	memcpy(destination, bench_pic_start,
	       (size_t)(bench_pic_end - bench_pic_start));
	return destination;
}

#define BENCH_DSTRAM_FREE ((unsigned char *)0x84400)   /* past the kernel's descriptors */
#define BENCH_A0RAM ((unsigned char *)0xc00)          /* application .fastcode */

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


/* --- hardware probe ------------------------------------------------------
 *
 * What the SDRAM controller register says, and what the memory system
 * actually does.  Everything here only reads outside the benchmark's own
 * buffer, so it is safe on any board: the sweep alternates two reads and
 * times them, and the aliasing test compares two regions.  The register's
 * ADDRC field is the board's claim; the sweep is the measurement, and the
 * two have to agree before code places buffers by bank.
 */

/* S1C33E07 Technical Manual table II.4.1.3.2, indexed by ADDRC. */
static const struct { unsigned char banks, row_bits, col_bits; }
probe_geometry[8] = {
	{ 2, 11,  8 }, { 4, 12,  8 }, { 4, 12,  9 }, { 4, 13,  9 },
	{ 2, 11,  9 }, { 4, 12,  9 }, { 4, 12, 10 }, { 4, 13, 10 },
};

static unsigned long probe_total_bytes(void)
{
	unsigned addrc = (unsigned)(REG_SDRAMC_CTL & ADDRC_MASK);
	return (unsigned long)probe_geometry[addrc].banks <<
		(probe_geometry[addrc].row_bits + probe_geometry[addrc].col_bits + 1);
}

/* Does the region at `offset` read back the same as the region at the
 * start of SDRAM?  On a board smaller than the address window the high
 * copy is the low one seen again. */
static int probe_aliases(unsigned long offset)
{
	const unsigned long *low = (const unsigned long *)0x10000000u;
	const unsigned long *high = (const unsigned long *)(0x10000000u + offset);
	unsigned i;

	for (i = 0; i < 256; i++)
		if (low[i] != high[i])
			return 0;
	return 1;
}

static void probe_hardware(unsigned char *buffer)
{
	unsigned addrc = (unsigned)(REG_SDRAMC_CTL & ADDRC_MASK);
	unsigned long total = probe_total_bytes();
	unsigned long row_bytes = 2ul << probe_geometry[addrc].col_bits;
	unsigned long bank_bytes = total / probe_geometry[addrc].banks;
	unsigned long limit = 0x10000000u + total;
	unsigned long offset;
	void *small;
	void *large;

	bench_line("probe register: addrc %u, %u banks, row %lu B, bank %lu KB, "
		   "total %lu MB",
		   addrc, probe_geometry[addrc].banks, row_bytes,
		   bank_bytes >> 10, total >> 20);

	/* The register's claim against what the address window contains. */
	for (offset = 4ul << 20; offset <= (32ul << 20); offset <<= 1)
		bench_line("probe alias: +%lu MB %s the start of memory",
			   offset >> 20,
			   probe_aliases(offset) ? "REPEATS" : "differs from");

	small = malloc(64);
	large = malloc(512u << 10);
	bench_line("probe heap: 64 B at 0x%08lx, 512 KB at 0x%08lx, "
		   "benchmark buffer at 0x%08lx, stack near 0x%08lx",
		   (unsigned long)(uintptr_t)small, (unsigned long)(uintptr_t)large,
		   (unsigned long)(uintptr_t)buffer,
		   (unsigned long)(uintptr_t)&offset);
	free(small);
	free(large);

	/* Alternate two reads a fixed distance apart.  Both in one row is the
	 * floor; another row of the same bank pays a precharge and activate;
	 * another bank is back at the floor.  The first slow step is the row
	 * size and the return to the floor is the bank stride. */
	for (offset = 4; offset <= (16ul << 20); offset <<= 1) {
		char name[16];
		unsigned long other = (unsigned long)(uintptr_t)buffer + offset;

		if (offset > 4 && offset < 256)
			continue;
		if (other + 4 > limit)
			break;
		if (offset < 1024)
			snprintf(name, sizeof(name), "gap-%luB", offset);
		else if (offset < (1ul << 20))
			snprintf(name, sizeof(name), "gap-%luK", offset >> 10);
		else
			snprintf(name, sizeof(name), "gap-%luM", offset >> 20);
		report(name, 2 * 20000,
		       run_pairs(buffer, (const unsigned char *)other, 20000));
	}
}

#define BENCH_BUFFER (1024u * 1024u + 8192u)
#endif

static void dstram_stack_fill(void);
static unsigned dstram_stack_depth(void);

void zim_bench_startup(zim_bench_read_fn read, void *opaque, uint64_t size)
{
#ifdef ZIM_BENCH_AB
	/* Calibration tests would dominate startup, warm card/cache state,
	 * and perturb the heap. Neither A/B variant runs them. */
	(void)read;
	(void)opaque;
	(void)size;
	dstram_stack_fill();
#else
	unsigned char *raw = malloc(BENCH_BUFFER);
	unsigned char *buffer;
	unsigned char *half;
	unsigned long ticks;
	volatile unsigned int on_stack[2] = { 1, 2 };

	bench_line("bench build: %s %s gcc %s", __DATE__, __TIME__, __VERSION__);
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
	probe_hardware(buffer);
	bench_line("bench buffer at 0x%08lx, stack near 0x%08lx",
		   (unsigned long)(uintptr_t)buffer,
		   (unsigned long)(uintptr_t)on_stack);

	report("cpu-loop", 3000000, run_cpu(3000000));
	report("fetch-1k", 512 * 256, run_fetch(256));
	/* No archive decoder runs during these CPU-only tests. Preserve its
	 * A0 code in the allocated test buffer and restore it before card or
	 * article work resumes. Kernel suspend code and scratch are outside
	 * this 528-byte application region. */
	memcpy(buffer, BENCH_A0RAM, (size_t)(bench_pic_end - bench_pic_start));
	{
		const unsigned char *a0ram = pic_copy(BENCH_A0RAM);
		report("cpu-loop-a0", 3000000,
		       run_pic(a0ram, bench_pic_cpu, 3000000));
		report("fetch-a0", 256 * 512,
		       run_pic(a0ram, bench_pic_fetch, 512));
		/* Code in A0 RAM, data in SDRAM: one row, two rows of one
		   bank, and a store followed by a load of the same word.
		   The model is fast by a fifth on every phase whose code
		   runs from internal RAM while its fetch is right, so what
		   it misses is here. */
		/* Not `buffer`: its first bytes hold the saved A0 code, and
		   the store test would write over it. */
		report("a0-load", 8 * 10000,
		       run_pic_data(a0ram, bench_pic_load, 10000, half, half));
		report("a0-load-2rows", 8 * 10000,
		       run_pic_data(a0ram, bench_pic_load2, 10000,
				    half, half + 4096));
		report("a0-store-load", 8 * 10000,
		       run_pic_data(a0ram, bench_pic_storeload, 10000,
				    half, half));
		report("a0-load-indep", 8 * 10000,
		       run_pic_data(a0ram, bench_pic_loadindep, 10000,
				    half, half));
		report("a0-load-use", 8 * 10000,
		       run_pic_data(a0ram, bench_pic_loaduse, 10000,
				    half, half));
	}
	memcpy(BENCH_A0RAM, buffer, (size_t)(bench_pic_end - bench_pic_start));
	{
		const unsigned char *ivram = pic_copy(lcd_window_get_buffer());
		const unsigned char *dstram = pic_copy(BENCH_DSTRAM_FREE);

		report("cpu-loop-ivram", 3000000,
		       run_pic(ivram, bench_pic_cpu, 3000000));
		report("fetch-ivram", 256 * 512,
		       run_pic(ivram, bench_pic_fetch, 512));
		report("cpu-loop-dstram", 3000000,
		       run_pic(dstram, bench_pic_cpu, 3000000));
		report("fetch-dstram", 256 * 512,
		       run_pic(dstram, bench_pic_fetch, 512));
	}
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
	dstram_stack_fill();
#endif
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
	blob_pending = 1;
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

/* FNV-1a of the decoded article, to check on the device what the emulator
 * checks against the archive: a decoder bug that damages only articles with
 * long matches passed a suite of eight for a whole session. */
static uint32_t article_hash;

void zim_bench_article_hash(const unsigned char *raw, size_t raw_size)
{
	uint32_t hash = 2166136261u;
	size_t k;

	for (k = 0; k < raw_size; k++)
		hash = (hash ^ raw[k]) * 16777619u;
	article_hash = hash;
}

/* The decoder's private stack in DSTRAM (zim/zstd/zstddeclib.c): filled
 * with a pattern once the start-up tests that borrow the same area are
 * done, and scanned after each article for the deepest point reached. */
#define DSTRAM_STACK_BOTTOM ((volatile uint32_t *)0x84400)
#define DSTRAM_STACK_TOP    ((volatile uint32_t *)0x84800)
#define DSTRAM_STACK_FILL   0xa5a5a5a5u

static void dstram_stack_fill(void)
{
	volatile uint32_t *p;

	for (p = DSTRAM_STACK_BOTTOM; p < DSTRAM_STACK_TOP; p++)
		*p = DSTRAM_STACK_FILL;
}

static unsigned dstram_stack_depth(void)
{
	volatile uint32_t *p = DSTRAM_STACK_BOTTOM;

	while (p < DSTRAM_STACK_TOP && *p == DSTRAM_STACK_FILL)
		p++;
	return (unsigned)((DSTRAM_STACK_TOP - p) * 4);
}

void zim_bench_mark(int mark)
{
	if (pending && mark > 0 && mark < ZIM_BENCH_MARKS)
		marks[mark] = timer_get();
	if (mark == ZIM_BENCH_MARK_BLOB)
		blob_pending = 0;
}

void zim_bench_account(int slot, unsigned long ticks, size_t bytes)
{
	/* The first visible image is extracted during painting. Its reads
	 * must not be charged to the earlier article-blob phase, or "other"
	 * underflows when those extra reads exceed that phase's duration. */
	if (pending && blob_pending && slot >= 0 && slot < ZIM_BENCH_SLOTS) {
		slot_ticks[slot] += ticks;
		slot_bytes[slot] += bytes;
		slot_calls[slot]++;
	}
}

void zim_bench_image(unsigned width, unsigned height, size_t compressed,
		     unsigned long setup, unsigned long decode,
		     unsigned long dither, const unsigned char *bitmap, size_t size)
{
	uint32_t hash = 2166136261u;
	size_t i;
	for (i = 0; i < size; i++)
		hash = (hash ^ bitmap[i]) * 16777619u;
	bench_line("image %ux%u %lu bytes: setup %lu.%lu decode %lu.%lu "
		   "dither %lu.%lu ms, bitmap fnv %08lx",
		   width, height, (unsigned long)compressed, MS(setup),
		   MS(decode), MS(dither), (unsigned long)hash);
}

void zim_bench_painted(void)
{
	unsigned long start, total, blob, html, wrap, paint, other;

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
	/* Whatever the slots do not account for: the directory entry, the
	   cluster's offset table, the article cache, and any wait the card
	   makes outside a read call.  It was 14 ms and became 200 on one
	   device run, so it is printed rather than left to subtraction. */
	other = blob - slot_ticks[ZIM_BENCH_SLOT_CARD] -
		slot_ticks[ZIM_BENCH_SLOT_ZSTD] - slot_ticks[ZIM_BENCH_SLOT_ALLOC];
	bench_line("article %lu total %lu.%lu blob %lu.%lu (card %lu.%lu %luK %lu, "
		   "zstd %lu.%lu %lu, alloc %lu.%lu %lu, other %lu.%lu) "
		   "html %lu.%lu wrap %lu.%lu paint %lu.%lu ms, "
		   "%lu %lu %lu bytes",
		   (unsigned long)pending_index, MS(total), MS(blob),
		   MS(slot_ticks[ZIM_BENCH_SLOT_CARD]),
		   slot_bytes[ZIM_BENCH_SLOT_CARD] / 1024,
		   slot_calls[ZIM_BENCH_SLOT_CARD],
		   MS(slot_ticks[ZIM_BENCH_SLOT_ZSTD]),
		   slot_calls[ZIM_BENCH_SLOT_ZSTD],
		   MS(slot_ticks[ZIM_BENCH_SLOT_ALLOC]),
		   slot_calls[ZIM_BENCH_SLOT_ALLOC], MS(other),
		   MS(html), MS(wrap), MS(paint),
		   (unsigned long)size_raw, (unsigned long)size_text,
		   (unsigned long)size_stream);
	bench_line("article %lu fnv %08lx, dstram stack depth %u of 1024",
		   (unsigned long)pending_index, (unsigned long)article_hash,
		   dstram_stack_depth());
}
