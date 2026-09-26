/*
 * A Game Boy for the WikiReader: Grifo glue around gb.c.
 *
 *   gameboy.app GAME.gb [threshold] [serial] [frames=N] [script=...]
 *               [window=N]
 *
 * threshold draws the grays as white and black instead of dithering them;
 * serial copies what the game sends on the link port to the console.
 * With frames= it is a benchmark: the script plays the buttons, the run
 * goes flat out with no pacing, and after N frames it reports the speed
 * of the frames after window= to the serial console and gbbench.txt, then
 * powers off.
 * SPDX-License-Identifier: MIT
 */
#include <grifo.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gb.h"

enum {
	TIMER_HZ = 60000000,
	/* 70224 Game Boy cycles at 4194304 Hz */
	FRAME_TICKS = 1004562,
	/* The picture is top right (gb.h); the column left of it and the
	   strip below hold the controls.  Quit, small, top left; the D-pad
	   bottom left; Select bottom right. */
	QUIT_BOTTOM = 32,
	PAD_X = 40,		/* the D-pad's centre */
	PAD_Y = 164,
	PAD_ARM = 36,		/* centre to the end of an arm */
	PAD_HALF = 12,		/* half an arm's width */
	DEAD_ZONE = 10,
	STRIP_TOP = GBW_TOP + GBW_HEIGHT,
	SELECT_LEFT = 160,
	SELECT_TOP = 152,
};

static uint32_t buffers[2][LCD_BUFFER_SIZE_WORDS] __attribute__((aligned(4)));
static int front;

static uint8_t *cart_ram;
static size_t cart_ram_bytes;
static char save_path[64];

static uint8_t touch_pad, strip_buttons, front_buttons;
/* What the finger went down on; it keeps that until it lifts, so a thumb
   sliding off the D-pad still steers. */
static enum { TOUCH_NONE, TOUCH_PAD, TOUCH_SELECT } touch_owner;
static int quit;

static char report_text[2048];
static size_t report_length;

static void report(const char *format, ...) __attribute__((format(printf, 1, 2)));

/* Frames run on a private stack in the chip's DSTRAM: the 1 KB from
   0x84400 that the kernel's SD DMA descriptors leave free, as the ZIM
   reader's decoder uses it.  Every C call in the emulator saves and
   restores registers on the stack, and in SDRAM that was a row miss and a
   posted-write queue each time.  Nothing on it may call deep into the
   kernel: the serial port is buffered and printed between frames, and an
   error goes back to the SDRAM stack before it does anything.  Interrupts
   push onto it too; the canary at the bottom catches an overflow into the
   descriptors, and the benchmark reports the depth reached. */
enum {
	FAST_STACK_BOTTOM = 0x84400,
	FAST_STACK_TOP = 0x84800,
	FAST_STACK_FILL = 0x5a5a5a5a,
};

static uint32_t sdram_sp;
static char serial_text[256];
static size_t serial_length;

static void fill_fast_stack(void)
{
	for (uint32_t *p = (uint32_t *)FAST_STACK_BOTTOM;
	     p < (uint32_t *)FAST_STACK_TOP; ++p)
		*p = FAST_STACK_FILL;
}

static unsigned fast_stack_depth(void)
{
	const uint32_t *p = (const uint32_t *)FAST_STACK_BOTTOM;

	while (p < (const uint32_t *)FAST_STACK_TOP && *p == FAST_STACK_FILL)
		++p;
	return (unsigned)(FAST_STACK_TOP - (uintptr_t)p);
}

/* gbw_run_frame on the DSTRAM stack.  noinline: the asm claims every
   call-clobbered register, which cannot be satisfied inlined. */
static __attribute__((noinline)) unsigned run_frame(void)
{
	unsigned lines;

	__asm__ volatile(
		"ld.w\t%%r1, %%sp\n\t"
		"ld.w\t[%1], %%r1\n\t"
		"xld.w\t%%r2, %2\n\t"
		"ld.w\t%%sp, %%r2\n\t"
		"xcall\tgbw_run_frame\n\t"
		"ld.w\t%%sp, %%r1\n\t"
		"ld.w\t%0, %%r4"
		: "=r"(lines)
		: "r"(&sdram_sp), "i"(FAST_STACK_TOP)
		: "r1", "r2", "r4", "r5", "r6", "r7", "r8", "r9",
		  "r10", "r11", "r12", "r13", "r14", "memory");
	if (serial_length) {
		serial_text[serial_length] = 0;
		debug_print(serial_text);
		serial_length = 0;
	}
	if (*(const uint32_t *)FAST_STACK_BOTTOM != FAST_STACK_FILL)
		gbw_error("frame stack overflowed", fast_stack_depth());
	return lines;
}

/* The link port's output on the serial console, for test ROMs that
   report there ("serial" on the command line): a byte costs the console
   about 10,000 cycles, and some games transmit all the time. */
static int serial_console;

void gbw_serial_out(uint8_t byte)
{
	if (serial_console && serial_length < sizeof serial_text - 1)
		serial_text[serial_length++] = (char)byte;
}

static void __attribute__((noreturn)) stopped(const char *what, unsigned address)
{
	debug_printf("gb: %s at %04x\n", what, address);
	lcd_set_default_framebuffer();
	lcd_clear(LCD_WHITE);
	lcd_printf("Game Boy stopped:\n%s at %04x\n\nTouch to return.", what,
		   address);
	for (;;) {
		event_t event;

		if (event_wait(&event, NULL, NULL) == EVENT_TOUCH_DOWN)
			chain("init.app");
	}
}

static const char *stopped_what;
static unsigned stopped_address;

static void __attribute__((noreturn, used)) stopped_off_stack(void)
{
	stopped(stopped_what, stopped_address);
}

void gbw_error(const char *what, unsigned address)
{
	/* Off the DSTRAM stack first, if a frame was running on it: the
	   kernel calls stopped() makes go deep. */
	uint32_t sp;

	stopped_what = what;
	stopped_address = address;
	__asm__ volatile("ld.w\t%0, %%sp" : "=r"(sp));
	if (sp >= FAST_STACK_BOTTOM && sp <= FAST_STACK_TOP)
		__asm__ volatile("ld.w\t%%sp, %0\n\txcall\tstopped_off_stack"
				 : : "r"(sdram_sp) : "memory");
	stopped(what, address);
}

static void report(const char *format, ...)
{
	char line[160];
	va_list arguments;
	int length;

	va_start(arguments, format);
	length = vsnprintf(line, sizeof line, format, arguments);
	va_end(arguments);
	if (length < 0)
		return;
	if ((size_t)length >= sizeof line)
		length = sizeof line - 1;
	debug_print(line);
	if (report_length + (size_t)length < sizeof report_text) {
		memcpy(report_text + report_length, line, (size_t)length);
		report_length += (size_t)length;
	}
}

static long load_file(const char *path, uint8_t **data, size_t (*size_for)(const uint8_t *, size_t))
{
	unsigned long bytes;
	int handle;
	size_t want;
	ssize_t got;
	uint8_t header[0x150];

	if (file_size(path, &bytes) != FILE_ERROR_OK || bytes < sizeof header)
		return -1;
	handle = file_open(path, FILE_OPEN_READ);
	if (handle < 0)
		return -1;
	if (file_read(handle, header, sizeof header) != (ssize_t)sizeof header) {
		file_close(handle);
		return -1;
	}
	want = size_for ? size_for(header, bytes) : bytes;
	*data = memory_allocate(want, "gameboy rom");
	if (!*data) {
		file_close(handle);
		return -1;
	}
	memcpy(*data, header, sizeof header);
	for (size_t done = sizeof header; done < bytes; done += (size_t)got) {
		size_t chunk = bytes - done > 32768 ? 32768 : bytes - done;

		got = file_read(handle, *data + done, chunk);
		if (got <= 0) {
			file_close(handle);
			return -1;
		}
		watchdog(WATCHDOG_KEY);
	}
	file_close(handle);
	return (long)bytes;
}

static void load_save(const char *rom_path)
{
	size_t length = strlen(rom_path);
	const char *dot = strrchr(rom_path, '.');
	unsigned long bytes;
	int handle;

	cart_ram_bytes = gbw_save_bytes();
	if (!cart_ram_bytes)
		return;
	cart_ram = memory_allocate(cart_ram_bytes, "gameboy save");
	if (!cart_ram) {
		cart_ram_bytes = 0;
		return;
	}
	memset(cart_ram, 0xff, cart_ram_bytes);
	gbw_set_cart_ram(cart_ram, cart_ram_bytes);
	if (dot)
		length = (size_t)(dot - rom_path);
	if (length + 5 > sizeof save_path)
		return;
	memcpy(save_path, rom_path, length);
	strcpy(save_path + length, ".sav");
	if (file_size(save_path, &bytes) == FILE_ERROR_OK
	    && bytes == cart_ram_bytes
	    && (handle = file_open(save_path, FILE_OPEN_READ)) >= 0) {
		(void)file_read(handle, cart_ram, cart_ram_bytes);
		file_close(handle);
	}
}

static void write_save(void)
{
	int handle;

	if (!cart_ram_bytes || !save_path[0])
		return;
	handle = file_create(save_path, FILE_OPEN_WRITE);
	if (handle < 0)
		return;
	(void)file_write(handle, cart_ram, cart_ram_bytes);
	file_close(handle);
}

static void box(int left, int top, int right, int bottom)
{
	lcd_move_to(left, top);
	lcd_line_to(right, top);
	lcd_line_to(right, bottom);
	lcd_line_to(left, bottom);
	lcd_line_to(left, top);
}

/* An outline coordinate: 3 is the end of an arm, 1 its side. */
static int arm(int v)
{
	return v == 3 ? PAD_ARM : v == -3 ? -PAD_ARM : v * PAD_HALF;
}

static void draw_controls(void)
{
	/* The D-pad's outline, clockwise from the top arm's top left. */
	static const signed char cross[][2] = {
		{ -1, -3 }, { 1, -3 }, { 1, -1 }, { 3, -1 }, { 3, 1 },
		{ 1, 1 }, { 1, 3 }, { -1, 3 }, { -1, 1 }, { -3, 1 },
		{ -3, -1 }, { -1, -1 }, { -1, -3 },
	};

	/* Into the emulator's own buffer: the default framebuffer holds the
	   machine and code (memory.lds). */
	front = 0;
	(void)lcd_set_framebuffer(buffers[front]);
	lcd_clear(LCD_WHITE);
	for (unsigned i = 0; i < sizeof cross / sizeof cross[0]; ++i) {
		int x = PAD_X + arm(cross[i][0]), y = PAD_Y + arm(cross[i][1]);

		if (i == 0)
			lcd_move_to(x, y);
		else
			lcd_line_to(x, y);
	}
	box(0, 0, GBW_LEFT_BYTE * 8 - 1, QUIT_BOTTOM - 1);
	lcd_at_xy(3, 1);
	lcd_print("QUIT");
	box(SELECT_LEFT, SELECT_TOP, LCD_WIDTH - 1, LCD_HEIGHT - 1);
	lcd_at_xy(SELECT_LEFT / 8 + 2, 13);
	lcd_print("SELECT");
	memcpy(buffers[1], buffers[0], LCD_BUFFER_SIZE_BYTES);
	gbw_set_framebuffer((uint8_t *)buffers[!front]);
}

static void flip(void)
{
	front = !front;
	(void)lcd_set_framebuffer(buffers[front]);
	gbw_set_framebuffer((uint8_t *)buffers[!front]);
}

/* The direction from the D-pad's centre.  Each axis counts once it is
   more than tan 22.5 degrees of the other, so the diagonals get their fair
   eighth. */
static uint8_t pad_from_touch(int x, int y)
{
	int dx = x - PAD_X, dy = y - PAD_Y;
	int ax = abs(dx), ay = abs(dy);
	uint8_t pad = 0;

	if (ax * ax + ay * ay < DEAD_ZONE * DEAD_ZONE)
		return 0;
	if (ax * 5 > ay * 2)
		pad |= dx > 0 ? GBW_RIGHT : GBW_LEFT;
	if (ay * 5 > ax * 2)
		pad |= dy > 0 ? GBW_DOWN : GBW_UP;
	return pad;
}

static void handle(const event_t *event)
{
	switch (event->item_type) {
	case EVENT_TOUCH_DOWN: {
		int x = event->touch.x, y = event->touch.y;

		touch_owner = TOUCH_NONE;
		if (x < GBW_LEFT_BYTE * 8 && y < QUIT_BOTTOM)
			quit = 1;
		else if (x >= SELECT_LEFT - 16 && y >= STRIP_TOP)
			touch_owner = TOUCH_SELECT;
		else if (x < GBW_LEFT_BYTE * 8 || y >= STRIP_TOP)
			touch_owner = TOUCH_PAD;
	}
		/* fall through */
	case EVENT_TOUCH_MOTION:
		touch_pad = touch_owner == TOUCH_PAD
			? pad_from_touch(event->touch.x, event->touch.y) : 0;
		strip_buttons = touch_owner == TOUCH_SELECT ? GBW_SELECT : 0;
		break;
	case EVENT_TOUCH_UP:
		touch_owner = TOUCH_NONE;
		touch_pad = 0;
		strip_buttons = 0;
		break;
	case EVENT_BUTTON_DOWN:
	case EVENT_BUTTON_UP: {
		/* Left to right under the panel: Search, History, Random --
		   Start, then B and A where a Game Boy has them. */
		uint8_t bit = event->button.code == BUTTON_SEARCH ? GBW_START
			: event->button.code == BUTTON_HISTORY ? GBW_B
			: event->button.code == BUTTON_RANDOM ? GBW_A : 0;

		if (event->item_type == EVENT_BUTTON_DOWN)
			front_buttons |= bit;
		else
			front_buttons &= (uint8_t)~bit;
		break;
	}
	case EVENT_BATTERY_LOW:
		write_save();
		power_off();
	default:
		break;
	}
}

static void poll_events(void)
{
	event_t event;

	while (event_get(&event) != EVENT_NONE)
		handle(&event);
}

/* FNV-1a of the cartridge RAM, to see whether the game has saved. */
static uint32_t save_hash(void)
{
	uint32_t hash = 2166136261u;

	for (size_t i = 0; i < cart_ram_bytes; ++i)
		hash = (hash ^ cart_ram[i]) * 16777619u;
	return hash;
}

enum { AUTOSAVE_FRAMES = 300 };		/* about five seconds */

static void play(void)
{
	unsigned long next = timer_get();
	uint32_t saved = save_hash();
	unsigned frame = 0;

	while (!quit) {
		unsigned long now = timer_get();
		/* Behind by more than a frame: draw every other one. */
		long behind = (long)(now - next);

		poll_events();
		gbw_set_buttons(touch_pad | strip_buttons | front_buttons);
		gbw_set_frame_skip(behind > FRAME_TICKS);
		if (run_frame())
			flip();
		watchdog(WATCHDOG_KEY);
		/* The power switch turns the machine off without asking, so
		   a game's save reaches the card once it stops changing. */
		if (cart_ram_bytes && ++frame % AUTOSAVE_FRAMES == 0) {
			uint32_t hash = save_hash();

			if (hash != saved) {
				write_save();
				saved = hash;
			}
		}
		next += FRAME_TICKS;
		now = timer_get();
		if ((long)(now - next) > 4 * FRAME_TICKS)
			next = now - 4 * FRAME_TICKS;
		/* Sleep the CPU until the next frame is due, waking for
		   input. */
		for (;;) {
			long wait = (long)(next - timer_get());
			event_t event;

			if (wait <= 0)
				break;
			if (event_wait_timeout(&event, (unsigned long)wait
					       / (TIMER_HZ / 1000000)) != EVENT_NONE)
				handle(&event);
		}
	}
}

static void benchmark(const char *script, unsigned frames, unsigned window)
{
	unsigned long long ticks = 0;
	unsigned long slowest = 0, fastest = ~0ul;
	unsigned timed = 0, drawn = 0;

	for (unsigned frame = 1; frame <= frames; ++frame) {
		unsigned long start;
		unsigned long spent;

		gbw_set_buttons(gbw_script_buttons(script, frame));
		start = timer_get();
		if (run_frame()) {
			flip();
			++drawn;
		}
		spent = timer_get() - start;
		watchdog(WATCHDOG_KEY);
		if (frame > window) {
			ticks += spent;
			++timed;
			if (spent > slowest)
				slowest = spent;
			if (spent < fastest)
				fastest = spent;
		}
		/* Hashing and printing are outside the timing. */
		if (frame % 60 == 0 || frame == frames)
			report("gb: frame %u hash %08lx\n", frame,
			       (unsigned long)gbw_hash((uint8_t *)buffers[front]));
	}
	if (timed) {
		unsigned long long per_frame = ticks / timed;
		unsigned long percent_x10 = (unsigned long)
			(FRAME_TICKS * 1000ull / per_frame);

		report("gb: %u frames timed after frame %u, %u drawn in all\n",
		       timed, window, drawn);
		report("gb: %lu us a frame, fastest %lu, slowest %lu\n",
		       (unsigned long)(per_frame / (TIMER_HZ / 1000000)),
		       fastest / (TIMER_HZ / 1000000),
		       slowest / (TIMER_HZ / 1000000));
		report("gb: %lu.%lu%% of real time, %lu.%lu fps\n",
		       percent_x10 / 10, percent_x10 % 10,
		       (unsigned long)(TIMER_HZ * 10ull / per_frame) / 10,
		       (unsigned long)(TIMER_HZ * 10ull / per_frame) % 10);
	}
	{
		unsigned long given = 0;
		int top[4] = { -1, -1, -1, -1 };

		for (int op = 0; op < 256; ++op) {
			given += gbw_counts.given_back[op];
			for (int k = 0; k < 4; ++k)
				if (top[k] < 0 || gbw_counts.given_back[op]
				    > gbw_counts.given_back[top[k]]) {
					for (int j = 3; j > k; --j)
						top[j] = top[j - 1];
					top[k] = op;
					break;
				}
		}
		report("gb: per frame: %lu hot.s calls, %lu given back, %lu interrupt or HALT steps\n",
		       gbw_counts.hot_calls / frames, given / frames,
		       gbw_counts.interrupts / frames);
		report("gb: given back most: %02x %lu, %02x %lu, %02x %lu, %02x %lu\n",
		       top[0], gbw_counts.given_back[top[0]] / frames,
		       top[1], gbw_counts.given_back[top[1]] / frames,
		       top[2], gbw_counts.given_back[top[2]] / frames,
		       top[3], gbw_counts.given_back[top[3]] / frames);
	}
	report("gb: frame stack reached %u of %u bytes\n", fast_stack_depth(),
	       (unsigned)(FAST_STACK_TOP - FAST_STACK_BOTTOM));
	report("gb: done\n");

	int handle = file_create("gbbench.txt", FILE_OPEN_WRITE);
	if (handle >= 0) {
		(void)file_write(handle, report_text, report_length);
		file_close(handle);
	}
}

int grifo_main(int argc, char **argv)
{
	const char *path = NULL, *script = NULL;
	unsigned frames = 0, window = 0;
	uint8_t *rom;
	long bytes;
	const char *failure;

	/* Grifo's own argv describes the boot, so a game is recognised by
	   its extension rather than its position. */
	for (int i = 1; i < argc; ++i) {
		size_t length = strlen(argv[i]);

		if (strncmp(argv[i], "frames=", 7) == 0)
			frames = (unsigned)strtoul(argv[i] + 7, NULL, 10);
		else if (strncmp(argv[i], "window=", 7) == 0)
			window = (unsigned)strtoul(argv[i] + 7, NULL, 10);
		else if (strncmp(argv[i], "script=", 7) == 0)
			script = argv[i] + 7;
		else if (strcmp(argv[i], "threshold") == 0)
			gbw_set_dither(0);
		else if (strcmp(argv[i], "serial") == 0)
			serial_console = 1;
		else if (!path && length > 3
			 && (strcmp(argv[i] + length - 3, ".gb") == 0
			     || strcmp(argv[i] + length - 4, ".gbc") == 0))
			path = argv[i];
	}
	if (!path)
		path = "game.gb";

	lcd_window_disable();
	bytes = load_file(path, &rom, gbw_rom_bytes);
	if (bytes >= 0)
		draw_controls();
	failure = bytes < 0 ? "cannot read the game" : gbw_init(rom, (size_t)bytes);
	if (failure) {
		lcd_set_default_framebuffer();
		lcd_clear(LCD_WHITE);
		lcd_printf("Game Boy: %s\n%s\n\nTouch to return.", path, failure);
		debug_printf("gb: %s: %s\n", path, failure);
		for (;;) {
			event_t event;

			if (event_wait(&event, NULL, NULL) == EVENT_TOUCH_DOWN)
				chain("init.app");
		}
	}
	report("gb: %s, %s, %ld bytes, %lu bytes of cartridge RAM\n", path,
	       gbw_title(), bytes, (unsigned long)gbw_save_bytes());
	load_save(path);

	fill_fast_stack();
	if (frames) {
		benchmark(script, frames, window);
		power_off();
	}
	play();
	write_save();
	lcd_set_default_framebuffer();
	chain("init.app");
}
