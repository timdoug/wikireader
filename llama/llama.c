/* Native Grifo application entry, GPL-3.0-or-later.
 *
 * Generates a TinyStories continuation on the device, painting each piece
 * as it is produced.  The model, the tokenizer and the generation loop are
 * the same files the host build uses; this supplies the four platform
 * functions and the screen.
 */

#include <grifo.h>
#include <string.h>

#include "runner.h"

#define MODEL_PATH "/model.wrl"
#define VOCAB_PATH "/tok.bin"

/* Stable symbols for emulator scripting and the PC-bucket profiler. */
void __attribute__((noinline)) llama_token_ready(void) { asm volatile("nop"); }
void __attribute__((noinline)) llama_run_done(void) { asm volatile("nop"); }

void *llama_alloc(size_t bytes, const char *tag)
{
	return memory_allocate(bytes ? bytes : 1, tag);
}

void llama_release(void *p, const char *tag)
{
	if (p)
		memory_free(p, tag);
}

/* A0 internal RAM, through the .fastbss section the standard application
   linker script already places there (fastram, 0xc00 + 0x13c0).  Nothing
   is ever returned: the arena is static and the model outlives the run. */
static unsigned char fast_arena[512] __attribute__((section(".fastbss")));
static size_t fast_used;

void *llama_alloc_fast(size_t bytes)
{
	void *p;

	bytes = (bytes + 3u) & ~(size_t)3u;
	if (bytes == 0 || bytes > sizeof fast_arena - fast_used)
		return NULL;
	p = fast_arena + fast_used;
	fast_used += bytes;
	return p;
}

uint32_t llama_now_us(void)
{
	/* timer_get is a syscall and costs a few hundred cycles, so this is
	   called twice a token and never inside the forward pass. */
	return (uint32_t)(timer_get() / TIMER_CountsPerMicroSecond);
}

/* ---- screen ---------------------------------------------------------- */

static int verbose;

/* The panel is 240x208 with a 6x9 font, so 39 columns by 23 rows.  Rather
   than scroll -- which would mean repainting the whole frame for every
   token, at about a hundred milliseconds each -- the text wraps and then
   stops, and the run ends when the page is full. */
enum {
	SCREEN_COLS = 39,
	SCREEN_ROWS = 23,
};

typedef struct {
	int col, row, full;
} screen;

static void screen_putc(screen *s, char c)
{
	if (s->full)
		return;
	if (c == '\n') {
		s->col = 0;
		if (++s->row >= SCREEN_ROWS) {
			s->full = 1;
			return;
		}
		/* The panel needs the newline too, not just the column
		   counter: without this the model's own line breaks -- and
		   TinyStories emits them -- were counted and then dropped,
		   so the text ran on and the row bookkeeping described a
		   layout the screen did not have. */
		lcd_print_char(c);
		return;
	}
	if (c < ' ')
		return;
	if (s->col >= SCREEN_COLS) {
		s->col = 0;
		if (++s->row >= SCREEN_ROWS) {
			s->full = 1;
			return;
		}
	}
	lcd_print_char(c);
	s->col++;
}

static void emit(void *ctx, const char *piece)
{
	screen *s = ctx;
	const char *c;

	for (c = piece; *c; c++)
		screen_putc(s, *c);
	if (verbose)
		debug_print(piece);
	llama_token_ready();
	watchdog(WATCHDOG_KEY);
}

/* ---- loading --------------------------------------------------------- */

/* Reads a whole file into one allocation, with a spare byte past the end:
   the tokenizer terminates its last piece there. */
static void *load(const char *path, size_t *bytes, const char *tag)
{
	unsigned long length;
	int handle;
	unsigned char *buf;
	size_t got = 0;

	if (file_size(path, &length) < 0)
		return NULL;
	handle = file_open(path, FILE_OPEN_READ);
	if (handle < 0)
		return NULL;

	buf = memory_allocate((size_t)length + 1, tag);
	if (!buf) {
		file_close(handle);
		return NULL;
	}
	/* One call per read, but grifo's file_read already issues long
	   multi-sector transfers; the model is a megabyte at most. */
	while (got < length) {
		ssize_t n = file_read(handle, buf + got, length - got);

		if (n <= 0) {
			memory_free(buf, tag);
			file_close(handle);
			return NULL;
		}
		got += (size_t)n;
	}
	file_close(handle);
	buf[length] = 0;
	*bytes = (size_t)length;
	return buf;
}

/* event_wait calls this after its two-minute timeout; true means keep
   waiting.  The story stays on the panel until the reader is done with
   it, which is the whole point of stopping here rather than powering
   off. */
static bool idle_forever(void *arg)
{
	(void)arg;
	return true;
}

static void fail(const char *what, const char *why)
{
	lcd_printf("\n%s:\n%s\n", what, why);
	debug_printf("llama: %s: %s\n", what, why);
}

int grifo_main(int argc, char **argv)
{
	static llama_model model;
	static llama_tokenizer tok;
	const char *prompt = NULL;
	int steps = 0, once = 0, i;
	void *wimage, *timage;
	size_t wbytes, tbytes;
	llama_run_options opt;
	llama_run_stats stats;
	llama_status st;
	screen sc;
	uint32_t load_us;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v"))
			verbose = 1;
		else if (!strcmp(argv[i], "-once"))
			once = 1;
		else if (!strcmp(argv[i], "-n") && i + 1 < argc)
			steps = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-i") && i + 1 < argc)
			prompt = argv[++i];
	}

	lcd_clear(LCD_WHITE);
	lcd_print("Loading model...\n");

	load_us = llama_now_us();
	wimage = load(MODEL_PATH, &wbytes, "llama.weights");
	if (!wimage) {
		fail("Cannot read " MODEL_PATH, "is the card in?");
		return 1;
	}
	timage = load(VOCAB_PATH, &tbytes, "llama.vocab");
	if (!timage) {
		fail("Cannot read " VOCAB_PATH, "is the card in?");
		return 1;
	}

	st = llama_open(&model, wimage, wbytes, "llama");
	if (st != LLAMA_OK) {
		fail(MODEL_PATH, llama_strerror(st));
		return 1;
	}
	if (llama_tokenizer_open(&tok, timage, tbytes, model.cfg.vocab_size,
				 "llama") != 0) {
		fail(VOCAB_PATH, "bad tokenizer");
		return 1;
	}
	load_us = llama_now_us() - load_us;

	debug_printf("llama: dim %d, %d layers, vocab %d, %u bytes; "
		     "loaded in %u ms\n",
		     model.cfg.dim, model.cfg.n_layers, model.cfg.vocab_size,
		     (unsigned)wbytes, (unsigned)(load_us / 1000u));

	lcd_clear(LCD_WHITE);
	memset(&sc, 0, sizeof sc);
	memset(&opt, 0, sizeof opt);
	opt.emit = emit;
	opt.ctx = &sc;
	opt.steps = steps;
	opt.prompt = prompt;

	if (llama_run(&model, &tok, &opt, &stats) < 0) {
		fail("Generation failed", "out of memory");
		return 1;
	}
	llama_run_done();

	/* Milliseconds per token and cycles per multiply-accumulate: the two
	   numbers that say whether this part can run a transformer at all.
	   Integer throughout -- a division into a float would be a libgcc
	   call, and this is the one place the answer has to be exact. */
	{
		uint32_t ms = stats.forward_us / 1000u;
		uint32_t per_token = stats.tokens ?
				     stats.forward_us / stats.tokens : 0;
		uint32_t cycles_per_mac_x100 = stats.macs ?
			(uint32_t)(((uint64_t)stats.forward_us *
				    TIMER_CountsPerMicroSecond * 100u) /
				   stats.macs) : 0;

		debug_printf("llama: %d tokens, %u ms forward, %u us/token, "
			     "%u MACs, %u.%02u cycles/MAC\n",
			     stats.tokens, (unsigned)ms, (unsigned)per_token,
			     (unsigned)stats.macs,
			     (unsigned)(cycles_per_mac_x100 / 100u),
			     (unsigned)(cycles_per_mac_x100 % 100u));

		/* And on the panel, so a run that has finished does not look
		   like one that has stopped responding.  Under the emulator's
		   window timer_get is wall clock rather than guest cycles, so
		   this number is the host's and not the device's -- the
		   headless run is the one to quote. */
		if (!sc.full) {
			screen_putc(&sc, '\n');
			screen_putc(&sc, '\n');
			lcd_printf("[%d tokens, %u ms each]",
				   stats.tokens, (unsigned)(per_token / 1000u));
		}
	}

	/* The profiler's buckets are cumulative over a whole run, so an idle
	   loop waiting for a keypress buries the generation it is meant to
	   measure.  Under -once the machine stops instead.
	 *
	 * It has to be power_off and not a return: returning hands control
	 * back to init, which finds one entry in init.ini and auto-chains
	 * straight back here, so the app runs forever and the flag does the
	 * opposite of what it is called.
	 */
	if (once) {
		/* The serial line transmits at its baud rate -- about ten
		   thousand cycles a character -- and there is no flush in
		   the grifo API, so cutting the power here would truncate
		   the line just printed.  Give the last few hundred
		   characters time to reach the wire. */
		watchdog(WATCHDOG_KEY);
		delay_us(100000);
		watchdog(WATCHDOG_KEY);
		power_off();
	}

	/* Wait on the event, rather than spinning on the absence of one.
	 *
	 * The first version of this polled event_get in a loop and kicked the
	 * watchdog a million times a run.  On the device that is a flat
	 * battery; under the emulator's window it is worse, because the guest
	 * never halts, so the host burns a core simulating a machine doing
	 * nothing and the window stops feeling alive.  event_wait puts the
	 * CPU in HALT until something actually happens.
	 */
	for (;;) {
		event_t e;

		if (event_wait(&e, idle_forever, NULL) == EVENT_BATTERY_LOW)
			power_off();
	}
}
