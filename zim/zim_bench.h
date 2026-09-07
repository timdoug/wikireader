/* On-device timing for the ZIM reader.
 *
 * Built with ZIM_BENCH=YES, the app times a set of memory and card
 * micro-benchmarks at start-up and every article load by phase, and appends
 * each result as one line to bench.txt on the boot volume while also
 * printing it to the serial console (which the emulator echoes).  The same
 * app run in wremu on a writable card image produces the same lines from the
 * model, so the two files can be compared row by row to calibrate the
 * emulator.  A normal build compiles none of this. */
#ifndef WIKIREADER_ZIM_BENCH_H
#define WIKIREADER_ZIM_BENCH_H

#include <inttypes.h>
#include <stddef.h>

#if defined(ZIM_BENCH_AB)
enum {
	ZIM_BENCH_BOOT_INIT,
	ZIM_BENCH_BOOT_ARCHIVE,
	ZIM_BENCH_BOOT_SETTINGS,
	ZIM_BENCH_BOOT_UI,
	ZIM_BENCH_BOOT_FONTS,
	ZIM_BENCH_BOOT_MARKS
};
void zim_bench_boot_begin(void);
void zim_bench_boot_mark(int stage);
void zim_bench_font_sample(const char *path, unsigned long open_ticks,
	unsigned long resident_ticks, unsigned long map_ticks);
void zim_bench_boot_ready(void);
#else
#define zim_bench_boot_begin() ((void)0)
#define zim_bench_boot_mark(stage) ((void)0)
#define zim_bench_boot_ready() ((void)0)
#endif

enum {
	ZIM_BENCH_MARK_START,	/* retrieve_article entered */
	ZIM_BENCH_MARK_BLOB,	/* article HTML available (card reads + decode) */
	ZIM_BENCH_MARK_HTML,	/* converted to text */
	ZIM_BENCH_MARK_WRAP,	/* wrapped into the article stream */
	ZIM_BENCH_MARK_PAINT,	/* first page on the panel */
	ZIM_BENCH_MARKS
};

/* Work accounted inside the blob layer between the START and BLOB marks. */
enum {
	ZIM_BENCH_SLOT_CARD,	/* archive reads through the file adapter */
	ZIM_BENCH_SLOT_ZSTD,	/* ZSTD_decompressStream calls */
	ZIM_BENCH_SLOT_ALLOC,	/* releasing and placing the cluster buffers */
	ZIM_BENCH_SLOTS
};

#if defined(ZIM_BENCH)

typedef int (*zim_bench_read_fn)(void *opaque, uint64_t offset, void *buffer,
				 size_t length);

/* Run the micro-benchmarks and write the header lines. */
void zim_bench_startup(zim_bench_read_fn read, void *opaque, uint64_t size);

void zim_bench_article_begin(uint32_t index);
void zim_bench_article_cached(void);
void zim_bench_article_sizes(size_t raw, size_t text, size_t stream);
void zim_bench_article_hash(const unsigned char *raw, size_t raw_size);
void zim_bench_mark(int mark);
/* Called from the renderer when the first page reaches the panel; emits
 * the article line if a load is pending. */
void zim_bench_painted(void);
void zim_bench_account(int slot, unsigned long ticks, size_t bytes);
/* Successful WebP work only; archive extraction is outside these timers. */
void zim_bench_image(unsigned width, unsigned height, size_t compressed,
		     unsigned long setup, unsigned long decode,
		     unsigned long dither, const unsigned char *bitmap, size_t size);

#else

#define zim_bench_startup(read, opaque, size) ((void)0)
#define zim_bench_article_begin(index) ((void)0)
#define zim_bench_article_cached() ((void)0)
#define zim_bench_article_sizes(raw, text, stream) ((void)0)
#define zim_bench_article_hash(raw, size) ((void)0)
#define zim_bench_mark(mark) ((void)0)
#define zim_bench_painted() ((void)0)
#define zim_bench_account(slot, ticks, bytes) ((void)0)
#define zim_bench_image(width, height, compressed, setup, decode, dither, bitmap, size) ((void)0)

#endif

#endif
