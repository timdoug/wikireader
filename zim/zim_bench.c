/* Device benchmark; see zim_bench.h.  Its own object at the end of SOURCES,
 * so building it in moves no existing code. */
#include <grifo.h>
#include <regs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "glyph.h"
#include "guilib.h"
#include "lcd_buf_draw.h"
#include "wiki_info.h"
#include "zim_bench.h"
#include "zim_catalog.h"

#define BENCH_LIST "0:/zimbench.lst"
#define BENCH_LOG "0:/zimbench.txt"
#define MAX_ENTRIES 32
#define MAX_TITLE 64
#define LINE_HEIGHT 13

ZIM_BENCH zim_bench;
extern int display_first_page;   /* lcd_buf_draw.c; older trees lack its declaration */

static const char *label = "zim";
static const char *next_app;
static const char *next_label;
static int calibration;
static unsigned calibration_cases, calibration_failures;
static file_io_stats_t calibration_lookup_io;

static void calibration_record(int pass, int entry, const char *archive,
	const char *title, long index, unsigned long lookup_ticks,
	unsigned long open_ticks, unsigned long paint_ticks, unsigned long screen_ticks);

typedef struct {
	char archive[48];
	char title[MAX_TITLE];
} ENTRY;

static ENTRY entries[MAX_ENTRIES];
static int entry_count;
/* One pass answers most questions: image decoding is CPU work and repeats to
 * the microsecond.  "passes N" in the list asks for more, and "warmup" for an
 * unrecorded pass 0 first, for questions about card reads. */
static int passes = 1;
static int warmup;

/* Used when the card carries no list (zim/zimbench.lst is the usual one). */
static const char default_list[] =
	"wikivoyage | Paris\n" "wikivoyage | Sydney\n" "wikivoyage | Tokyo\n"
	"wikipedia_en_all | Cat\n" "simple | Cat\n";

static char list_text[2048];

static int lower(int c)
{
	return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

static int contains(const char *haystack, const char *needle)
{
	size_t n = strlen(needle);

	for (; *haystack; haystack++) {
		size_t i;

		for (i = 0; i < n && lower(haystack[i]) == lower(needle[i]); i++)
			;
		if (i == n)
			return 1;
	}
	return !n;
}

static void trim_copy(char *out, size_t size, const char *start, const char *end)
{
	size_t n;

	while (start < end && (*start == ' ' || *start == '\t'))
		start++;
	while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
		end--;
	n = (size_t)(end - start);
	if (n >= size)
		n = size - 1;
	memcpy(out, start, n);
	out[n] = '\0';
}

static void load_list(void)
{
	const char *p = default_list;
	int h = file_open(BENCH_LIST, FILE_OPEN_READ);

	if (h >= 0) {
		ssize_t n = file_read(h, list_text, sizeof(list_text) - 1);

		file_close(h);
		if (n > 0) {
			list_text[n] = '\0';
			p = list_text;
		}
	}
	while (*p && entry_count < MAX_ENTRIES) {
		const char *end = strchr(p, '\n');
		const char *bar;

		if (!end)
			end = p + strlen(p);
		bar = memchr(p, '|', (size_t)(end - p));
		if (!strncmp(p, "passes ", 7))
			passes = atoi(p + 7) > 0 ? atoi(p + 7) : 1;
		else if (!strncmp(p, "warmup", 6))
			warmup = 1;
		else if (*p != '#' && bar) {
			ENTRY *e = &entries[entry_count];

			trim_copy(e->archive, sizeof(e->archive), p, bar);
			trim_copy(e->title, sizeof(e->title), bar + 1, end);
			if (e->title[0])
				entry_count++;
		}
		p = *end ? end + 1 : end;
	}
}

static int find_archive(const char *name)
{
	int i;

	for (i = 0; i < zim_catalog_count(); i++)
		if (zim_catalog_path(i) && contains(zim_catalog_path(i), name))
			return i;
	return -1;
}

static void append_log(const char *line, int length)
{
	unsigned long size = 0;
	int h;

	/* The serial port too: the emulator's log, or a cable on the device. */
	debug_printf("%s", line);
	if (file_size(BENCH_LOG, &size) == FILE_ERROR_OK) {
		h = file_open(BENCH_LOG, FILE_OPEN_READ | FILE_OPEN_WRITE);
		if (h >= 0 && file_lseek(h, size) != FILE_ERROR_OK) {
			file_close(h);
			return;
		}
	} else {
		h = file_create(BENCH_LOG, FILE_OPEN_WRITE);
	}
	if (h < 0)
		return;
	file_write(h, (void *)line, (size_t)length);
	file_close(h);
}

static void show(int row, const char *text)
{
	guilib_fb_lock();
	render_string(SEARCH_LIST_FONT_IDX, 2, 2 + row * LINE_HEIGHT,
		      (const unsigned char *)text, (int)strlen(text), 0);
	guilib_fb_unlock();
}

static void status(const char *text)
{
	guilib_fb_lock();
	guilib_clear();
	guilib_fb_unlock();
	show(0, text);
}

static unsigned long us(unsigned long ticks)
{
	return ticks / TIMER_CountsPerMicroSecond;
}

static void load_one(int pass, int entry, const ENTRY *e)
{
	char line[768];
	unsigned long start, opened, idle, painted = 0;
	unsigned long lookup_start = 0, lookup_ticks = 0;
	long index;
	int wiki = find_archive(e->archive);
	int i, n;

	if (wiki < 0) {
		if (calibration) calibration_failures++;
		return;
	}
	if (wiki != nCurrentWiki)
		set_wiki(wiki);
	if (calibration) {
		zim_bench_drop_caches();
		file_profile(&calibration_lookup_io, true);
		lookup_start = timer_get();
	}
	index = zim_bench_find_title(e->title);
	if (calibration) {
		lookup_ticks = timer_get() - lookup_start;
		file_profile(&calibration_lookup_io, false);
	}
	if (!index) {
		if (calibration) calibration_failures++;
		n = snprintf(line, sizeof(line),
			     "ZB label=%s pass=%d archive=%s title=%s missing=1\n",
			     label, pass, zim_catalog_path(wiki), e->title);
		if (n > 0 && n < (int)sizeof(line))
			append_log(line, n);
		return;
	}
	zim_bench_drop_caches();
	memset(&zim_bench, 0, sizeof(zim_bench));
	zim_bench.active = 1;
	watchdog(WATCHDOG_KEY);
	start = timer_get();
	zim_bench.start = start;
	display_link_article(index);
	opened = timer_get();
	/* The main loop's work while idle: the first screen and its images. */
	for (;;) {
		int more = render_article_with_pcf();

		if (!painted && display_first_page)
			painted = timer_get();
		if (!more)
			break;
		watchdog(WATCHDOG_KEY);
	}
	idle = timer_get();
	zim_bench.active = 0;
	{
		const unsigned long *m = zim_bench.mark;

		n = snprintf(line, sizeof(line),
			     "ZB label=%s pass=%d archive=%s title=%s retrieve_us=%lu "
			     "read_us=%lu read_sectors=%lu open_us=%lu screen_us=%lu "
			     "image_us=%lu images=%d blob_us=%lu html_us=%lu "
			     "budget_us=%lu wrap_us=%lu raw=%lu text=%lu article=%lu "
			     "paint_us=%lu preview=%d finish_us=%lu\n",
			     label, pass, zim_catalog_path(wiki), e->title,
			     us(zim_bench.retrieve_ticks),
			     us((unsigned long)zim_bench.io.read_ticks),
			     (unsigned long)zim_bench.io.read_sectors,
			     us(opened - start), us(idle - start),
			     us(zim_bench.image[0].decode_ticks), zim_bench.images,
			     m[ZB_BLOB] ? us(m[ZB_HTML] - m[ZB_BLOB]) : 0,
			     us(m[ZB_BUDGET] - m[ZB_HTML]),
			     us(m[ZB_WRAP] - m[ZB_BUDGET]),
			     us(m[ZB_DONE] - m[ZB_WRAP]),
			     zim_bench.raw_size, zim_bench.text_size,
			     zim_bench.article_size,
			     painted ? us(painted - start) : 0, zim_bench.preview,
			     zim_bench.preview ?
				     us(m[ZB_FINISHED] - m[ZB_FINISH]) : 0);
		if (n > 0 && n < (int)sizeof(line))
			append_log(line, n);
	}
	for (i = 0; i < zim_bench.images; i++) {
		const ZIM_BENCH_IMAGE *b = &zim_bench.image[i];

		n = snprintf(line, sizeof(line),
			     "ZBI label=%s pass=%d title=%s image=%d y=%ld size=%dx%d "
			     "read_us=%lu decode_us=%lu done_us=%lu\n",
			     label, pass, e->title, i, b->y, b->width, b->height,
			     us(b->read_ticks), us(b->decode_ticks),
			     b->done ? us(b->done - start) : 0);
		if (n > 0 && n < (int)sizeof(line))
			append_log(line, n);
	}
	if (calibration && pass > 0)
		calibration_record(pass, entry, zim_catalog_path(wiki), e->title,
			index, lookup_ticks, opened - start,
			painted ? painted - start : 0, idle - start);
}

/* The comparison: for every article, the fastest measured pass of each
 * label's most recent run in the log. */
typedef struct {
	char key[96];
	unsigned long image[2], retrieve[2];
} ROW;

static ROW rows[MAX_ENTRIES];
static int row_count;
static char log_text[24576];

static unsigned long field(const char *line, const char *name)
{
	const char *p = strstr(line, name);

	return p ? strtoul(p + strlen(name), NULL, 10) : 0;
}

static void text_field(char *out, size_t size, const char *line,
		       const char *name, const char *next)
{
	const char *p = strstr(line, name);
	const char *end;

	out[0] = '\0';
	if (!p)
		return;
	p += strlen(name);
	end = strstr(p, next);
	if (!end)
		end = p + strlen(p);
	trim_copy(out, size, p, end);
}

/* V, S or W: which archive a row came from, for the panel's narrow table. */
static char archive_tag(const char *archive)
{
	if (contains(archive, "voyage"))
		return 'V';
	if (contains(archive, "simple"))
		return 'S';
	return 'W';
}

static void keep_min(unsigned long *slot, unsigned long value)
{
	if (value && (!*slot || value < *slot))
		*slot = value;
}

static void summarize(void)
{
	char line[96];
	const char *p, *run_start[2] = { NULL, NULL };
	const char *labels[2];
	unsigned long total[2] = { 0, 0 };
	int h, i, shown = 0;
	ssize_t n;

	labels[0] = next_label ? label : "old";
	labels[1] = label;
	h = file_open(BENCH_LOG, FILE_OPEN_READ);
	if (h < 0)
		return;
	n = file_read(h, log_text, sizeof(log_text) - 1);
	file_close(h);
	if (n <= 0)
		return;
	log_text[n] = '\0';
	/* Each run starts with a ZBRUN line; use the last run of each label. */
	for (p = log_text; (p = strstr(p, "ZBRUN label=")) != NULL; p++)
		for (i = 0; i < 2; i++)
			if (!strncmp(p + 12, labels[i], strlen(labels[i])) &&
			    p[12 + strlen(labels[i])] == ' ')
				run_start[i] = p;
	for (i = 0; i < 2; i++) {
		const char *q = run_start[i];

		if (!q)
			continue;
		for (q = strchr(q, '\n'); q && *++q; q = strchr(q, '\n')) {
			char title[MAX_TITLE], archive[48], key[96];
			int r;

			if (!strncmp(q, "ZBRUN", 5))
				break;
			if (strncmp(q, "ZB label=", 9) || field(q, " pass=") == 0 ||
			    strstr(q, "missing=1"))
				continue;
			text_field(archive, sizeof(archive), q, " archive=", " title=");
			text_field(title, sizeof(title), q, " title=", " retrieve_us=");
			snprintf(key, sizeof(key), "%c %s", archive_tag(archive), title);
			for (r = 0; r < row_count; r++)
				if (!strcmp(rows[r].key, key))
					break;
			if (r == row_count) {
				if (row_count == MAX_ENTRIES)
					continue;
				memset(&rows[r], 0, sizeof(rows[r]));
				snprintf(rows[r].key, sizeof(rows[r].key), "%s", key);
				row_count++;
			}
			keep_min(&rows[r].image[i], field(q, " image_us="));
			keep_min(&rows[r].retrieve[i], field(q, " retrieve_us="));
		}
	}
	guilib_fb_lock();
	guilib_clear();
	guilib_fb_unlock();
	show(shown++, "First image, ms: old -> new");
	for (i = 0; i < row_count; i++) {
		ROW *r = &rows[i];
		int len;

		if (!r->image[0] || !r->image[1])
			continue;
		total[0] += r->image[0];
		total[1] += r->image[1];
		len = snprintf(line, sizeof(line), "%.14s %lu -> %lu  %+ld%%",
			       r->key, r->image[0] / 1000, r->image[1] / 1000,
			       ((long)r->image[1] - (long)r->image[0]) * 100 /
				       (long)r->image[0]);
		if (len > 0 && shown < 13)
			show(shown++, line);
	}
	if (total[0]) {
		snprintf(line, sizeof(line), "All images %lu -> %lu  %+ld%%",
			 total[0] / 1000, total[1] / 1000,
			 ((long)total[1] - (long)total[0]) * 100 / (long)total[0]);
		show(shown++, line);
	}
	show(14, "Results in zimbench.txt. Tap to exit.");
}

static void wait_for_tap(void)
{
	event_t ev;

	/* Halted between events, as the reader's own idle loop is. */
	for (;;) {
		watchdog(WATCHDOG_KEY);
		if (event_wait_timeout(&ev, 1000000) != EVENT_NONE &&
		    (ev.item_type == EVENT_TOUCH_UP || ev.item_type == EVENT_BUTTON_UP))
			return;
	}
}

static int requested;

void zim_bench_args(int argc, char **argv)
{
	requested = 1;
	if (argc > 2)
		label = argv[2];
	if (argc == 4 && !strcmp(argv[3], "calibrate")) {
		calibration = 1;
		return;
	}
	if (argc > 4) {
		next_app = argv[3];
		next_label = argv[4];
	}
}

int zim_bench_requested(void)
{
	return requested;
}

void zim_bench_start(void)
{
	char line[256];
	char command[64];
	int original = nCurrentWiki;
	int pass, i, n;

	load_list();
	if (calibration) {
		event_flush();
		n = snprintf(line, sizeof(line),
			"ZCAL v1 entries=%d passes=%d warmup=%d ticks_per_us=%u scrolls=4 "
			"sdram_ctl=%08lx refresh=%08lx clkcntl=%08lx pll=%08lx\n",
			entry_count, passes, warmup, TIMER_CountsPerMicroSecond,
			(unsigned long)REG_SDRAMC_CTL, (unsigned long)REG_SDRAMC_REF,
			(unsigned long)REG_CMU_CLKCNTL, (unsigned long)REG_CMU_PLL);
		if (n > 0 && n < (int)sizeof(line)) append_log(line, n);
	}
	n = snprintf(line, sizeof(line), "ZBRUN label=%s entries=%d passes=%d\n",
		     label, entry_count, passes);
	if (n > 0 && n < (int)sizeof(line))
		append_log(line, n);
	for (pass = warmup ? 0 : 1; pass <= passes; pass++)
		for (i = 0; i < entry_count; i++)
			load_one(pass, i, &entries[i]);
	if (nCurrentWiki != original)
		set_wiki(original);
	if (calibration) {
		n = snprintf(line, sizeof(line), "END ZCAL cases=%u failures=%u\n",
			calibration_cases, calibration_failures);
		if (n > 0 && n < (int)sizeof(line)) append_log(line, n);
		power_off();
	}
	if (next_app) {
		snprintf(command, sizeof(command), "%s bench %s", next_app, next_label);
		status("Benchmark: next build...");
		chain(command);
	}
	summarize();
	wait_for_tap();
	chain("init.app");
}

/* Hashes and formatting are outside each measured interval. Composite the
 * LCD sub-window so the scroll indicator is checked alongside the text. */
static unsigned long calibration_frame(void)
{
	const unsigned char *frame = lcd_get_framebuffer();
	const unsigned char *window = lcd_window_get_buffer();
	unsigned long start = REG_LCDC_SSP, end = REG_LCDC_SEP;
	unsigned x0 = (start & PIPXST_MASK) * 4;
	unsigned x1 = ((end & PIPXEND_MASK) + 1) * 4;
	unsigned y0 = (start & PIPYST_MASK) >> PIPYST_SHIFT;
	unsigned y1 = ((end & PIPYEND_MASK) >> PIPYEND_SHIFT) + 1;
	unsigned stride = lcd_window_get_byte_width();
	unsigned long hash = 2166136261UL;
	unsigned y, x;
	for (y = 0; y < LCD_HEIGHT; y++)
		for (x = 0; x < LCD_WIDTH / 8; x++) {
			unsigned char byte = frame[y * LCD_BUFFER_WIDTH_BYTES + x];
			if ((start & PIPEN) && y >= y0 && y < y1 && x >= x0 && x < x1)
				byte = window[(y - y0) * stride + x - x0];
			hash = (hash ^ byte) * 16777619UL;
		}
	return hash;
}

static void calibration_record(int pass, int entry, const char *archive,
	const char *title, long index, unsigned long lookup_ticks,
	unsigned long open_ticks, unsigned long paint_ticks, unsigned long screen_ticks)
{
	static const int moves[] = {104, 104, -104, -104};
	extern int lcd_draw_cur_y_pos;
	extern int b_show_scroll_bar;
	char line[768];
	unsigned step;
	int n, ok = lookup_ticks && paint_ticks && screen_ticks &&
		zim_bench.retrieve_ticks && zim_bench.raw_size && zim_bench.article_size &&
		!calibration_lookup_io.read_errors && !calibration_lookup_io.dma_timeouts &&
		!calibration_lookup_io.dma_errors && !calibration_lookup_io.dma_disabled &&
		!zim_bench.io.read_errors && !zim_bench.io.dma_timeouts &&
		!zim_bench.io.dma_errors && !zim_bench.io.dma_disabled;
	calibration_cases++;
	calibration_failures += !ok;
	n = snprintf(line, sizeof(line),
		"ZC pass=%d entry=%d archive=%s title=%s index=%ld lookup_ticks=%lu "
		"retrieve_ticks=%lu open_ticks=%lu paint_ticks=%lu screen_ticks=%lu "
		"lookup_reads=%lu lookup_sectors=%lu lookup_read_ticks=%lu "
		"retrieve_reads=%lu retrieve_sectors=%lu retrieve_read_ticks=%lu "
		"raw=%lu text=%lu article=%lu images=%d frame=%08lx ok=%d\n",
		pass, entry, archive, title, index, lookup_ticks, zim_bench.retrieve_ticks,
		open_ticks, paint_ticks, screen_ticks,
		(unsigned long)calibration_lookup_io.read_calls,
		(unsigned long)calibration_lookup_io.read_sectors,
		(unsigned long)calibration_lookup_io.read_ticks,
		(unsigned long)zim_bench.io.read_calls, (unsigned long)zim_bench.io.read_sectors,
		(unsigned long)zim_bench.io.read_ticks, zim_bench.raw_size,
		zim_bench.text_size, zim_bench.article_size, zim_bench.images,
		calibration_frame(), ok);
	if (n > 0 && n < (int)sizeof(line)) append_log(line, n);
	b_show_scroll_bar = 1; /* the same rendering path used during a drag */
	for (step = 0; step < sizeof(moves) / sizeof(*moves); step++) {
		unsigned long start, ticks;
		int before = lcd_draw_cur_y_pos, after;
		watchdog(WATCHDOG_KEY);
		start = timer_get();
		display_article_with_pcf(moves[step]);
		while (render_article_with_pcf()) watchdog(WATCHDOG_KEY);
		ticks = timer_get() - start;
		after = lcd_draw_cur_y_pos;
		ok = ticks && (moves[step] > 0 ? after > before : after < before);
		calibration_failures += !ok;
		n = snprintf(line, sizeof(line),
			"ZCS pass=%d entry=%d step=%u delta=%d before=%d after=%d "
			"ticks=%lu direct=%u frame=%08lx ok=%d\n", pass, entry, step,
			moves[step], before, after, ticks, !!(REG_LCDC_SSP & PIPEN),
			calibration_frame(), ok);
		if (n > 0 && n < (int)sizeof(line)) append_log(line, n);
	}
	b_show_scroll_bar = 0;
	show_scroll_bar(0);
}
