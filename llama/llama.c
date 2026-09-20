/* Native Grifo application entry, GPL-3.0-or-later.
 *
 * Generates a TinyStories continuation on the device, painting each piece
 * as it is produced.  The model, the tokenizer and the generation loop are
 * the same files the host build uses; this supplies the four platform
 * functions and the screen.
 */

#include <grifo.h>
#include <string.h>

#include "keys.h"
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

/* grifo's LCD.c wraps at the panel edge and scrolls at the bottom -- see
   scroll() there, called from both paths.  An earlier version of this
   counted columns and rows itself and stopped the run when it thought the
   page was full, which was both redundant and wrong: the arithmetic
   disagreed with the real layout, and it would have cut a long story short
   on a display that handles one perfectly well. */
static void emit(void *ctx, const char *piece)
{
	const char *c;

	(void)ctx;
	for (c = piece; *c; c++)
		lcd_print_char(*c);
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
	int steps = 0, once = 0, ask = 0, temperature = 0, i;
	unsigned long seed = 0;
	char typed[64];
	void *wimage, *timage;
	size_t wbytes, tbytes;
	llama_run_options opt;
	llama_run_stats stats;
	llama_status st;
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
		else if (!strcmp(argv[i], "-k"))
			ask = 1;
		else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
			temperature = llama_parse_temperature(argv[++i]);
			if (temperature < 0)
				temperature = 0;
		} else if (!strcmp(argv[i], "-s") && i + 1 < argc)
			seed = (unsigned long)atoi(argv[++i]);
	}

	/* The arguments come from init.ini on the card, not from the command
	   that started the emulator, so they are invisible unless printed.
	   A run that stops after forty tokens because the card says -n 40 is
	   indistinguishable from a broken one otherwise. */
	debug_print("llama: arguments:");
	for (i = 1; i < argc; i++) {
		debug_print(" ");
		debug_print(argv[i]);
	}
	debug_print(steps > 0 ? "\n" : "  (no -n: until the story ends)\n");

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

	for (;;) {
		if (ask) {
			/* -k puts the keyboard up and uses what is typed.
			   An empty line means the unprompted story, which is
			   what the model writes from BOS alone. */
			if (keys_read_line(typed, sizeof typed,
					   "Prompt, then GO:") < 0)
				power_off();
			prompt = typed[0] ? typed : NULL;
		}

		lcd_clear(LCD_WHITE);
		memset(&opt, 0, sizeof opt);
		opt.emit = emit;
		opt.steps = steps;
		opt.prompt = prompt;
		opt.temperature_q8 = temperature;
		/* Zero asks llama_run for the clock.  On hardware that is
		   different every run, because it counts the time the reader
		   spent typing; headless in the emulator it is not, because
		   the guest clock is derived from cycles so that runs stay
		   reproducible.  -s pins it either way. */
		opt.seed = seed;

		if (llama_run(&model, &tok, &opt, &stats) < 0) {
			fail("Generation failed", "out of memory");
			return 1;
		}
		llama_run_done();

		/* Milliseconds a token and cycles per multiply-accumulate:
		   the two numbers that say whether this part can run a
		   transformer at all.  Integer throughout -- a division into
		   a float would be a libgcc call, and this is the one place
		   the answer has to be exact. */
		{
			uint32_t per_token = stats.tokens ?
					     stats.forward_us / stats.tokens : 0;
			uint32_t cyc_x100 = stats.macs ?
				(uint32_t)(((uint64_t)stats.forward_us *
					    TIMER_CountsPerMicroSecond * 100u) /
					   stats.macs) : 0;

			debug_printf("llama: %d tokens, %u ms forward, "
				     "%u us/token, %u MACs, "
				     "%u.%02u cycles/MAC; stopped: %s\n",
				     stats.tokens,
				     (unsigned)(stats.forward_us / 1000u),
				     (unsigned)per_token,
				     (unsigned)stats.macs,
				     (unsigned)(cyc_x100 / 100u),
				     (unsigned)(cyc_x100 % 100u),
				     llama_stop_text(stats.stop));

			lcd_printf("\n\n[%d tokens, %u ms each: %s]",
				   stats.tokens,
				   (unsigned)(per_token / 1000u),
				   llama_stop_text(stats.stop));
		}

		if (!ask)
			break;

		/* Wait for a touch, then offer the keyboard again, so a
		   second prompt does not mean rebooting the machine. */
		{
			event_t e;

			lcd_print("  (touch for another)");
			do {
				if (event_wait(&e, idle_forever, NULL) ==
				    EVENT_BATTERY_LOW)
					power_off();
			} while (e.item_type != EVENT_TOUCH_DOWN &&
				 e.item_type != EVENT_BUTTON_DOWN);
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
