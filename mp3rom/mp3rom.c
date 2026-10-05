/*
 * mp3rom: run the S1C33E07 boot ROM's MP3 decoder over test files and save
 * what it produces, to learn what it does (romdump/BOOTROM.md).
 *
 * Copyright (c) 2026 Tim Douglas
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The decoder's own `open` switches P04-P07 to I2S, and on the WikiReader
 * P04 is the touch controller's transmit line.  So this never calls the
 * entry points: it sets up the decoder's state as `open` would, without the
 * pins, the interrupt controller or the DMA, and calls the routine that
 * decodes one frame (0x27822) directly, with interrupts off.  The input is
 * read into RAM first, so nothing touches the card while the decoder runs.
 *
 * The decoder needs IVRAM in area 0 (0x2000-0x4fff).  That is where the
 * kernel keeps the LCD's frame, so IVRAM is saved first and restored after,
 * and nothing is drawn in between.
 *
 * Outputs go to 0:/mp3test/out/ under the first unused run number, every
 * file created new: rNNN.txt, the report, and rNNNxM.pcm, the 0x1200-byte
 * output half after each frame, x the input (a, b, c) and M the mode.
 */

#include <grifo.h>
#include <regs.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
	WORKSPACE_BYTES = 0x40000,      // far more than it needs: measured below
	INPUT_BYTES = 0x1000,           // the decoder uses 0xf80
	MP3_MAX = 0x10000,
	HALF_BYTES = 0x1200,
	MAX_FRAMES = 120,
	GUARD = 0xa5,
	MAX_RUNS = 999,
};

// ROM internals (romdump/BOOTROM.md)
enum : uint32_t {
	ROM_RESET_FLAGS = 0x274f6,      // void (void)
	ROM_PRIME = 0x27520,            // void (void): first input, frame decoder set up
	ROM_DECODE_FRAME = 0x27822,     // int (void *half): 1 decoded, 0 failed
	IVRAM_AREA0 = 0x2000,
	IVRAM_BYTES = 0x3000,
	STATE_POINTER = 0x4ff8,
	DMA_POINTER = 0x4ffc,
	LCDC_IRAM = 0x301a64,
};

// the decoder's state, at workspace + 0x2400, in words
enum {
	S_STATUS = 0x00 / 4, S_DECODER = 0x04 / 4, S_STATUS_CALLBACK = 0x08 / 4,
	S_READ = 0x0c / 4, S_READ_SIZE = 0x14 / 4, S_HALF = 0x18 / 4,
	S_INPUT = 0x1c / 4, S_WORKSPACE = 0x20 / 4, S_INPUT_BYTES = 0x24 / 4,
	S_OUTPUT_BYTES = 0x28 / 4, S_PLAY = 0x2c / 4, S_END = 0x34 / 4,
	S_CLOCK = 0x3c / 4, S_MODE = 0x44 / 4,
};

static uint8_t workspace[WORKSPACE_BYTES] __attribute__((aligned(16)));
static uint8_t input[INPUT_BYTES] __attribute__((aligned(16)));
static uint8_t mp3[MP3_MAX];
static uint8_t pcm[MAX_FRAMES * HALF_BYTES];
static uint8_t ivram_saved[IVRAM_BYTES];
static uint8_t ivram_after[IVRAM_BYTES];
static size_t mp3_length, mp3_position;
static unsigned reads;
static bool dry;        // 0:/mp3test/dry: everything but the ROM, for emulators without it

static char report[8192];
static size_t report_length, report_written;
static int report_handle = -1;


static void say(const char *format, ...) __attribute__ ((format (printf, 1, 2)));
static void say(const char *format, ...)
{
	char line[160];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	debug_print(line);
	debug_print("\n");
	if (report_length + strlen(line) + 2 < sizeof(report)) {
		strcpy(report + report_length, line);
		report_length += strlen(line);
		report[report_length++] = '\n';
		report[report_length] = '\0';
	}
}


// what has been said since last time, into the report, durably
static void report_flush(void)
{
	if (report_handle < 0 || report_written == report_length)
		return;
	file_write(report_handle, report + report_written, report_length - report_written);
	file_sync(report_handle);
	report_written = report_length;
}


// the decoder's input callback: C ABI, r6 buffer, r7 bytes, r4 result
static int read_input(void *buffer, int bytes)
{
	size_t n = mp3_length - mp3_position;

	if (n > (size_t)bytes)
		n = bytes;
	memcpy(buffer, mp3 + mp3_position, n);
	mp3_position += n;
	++reads;
	return (int)n;
}


static uint32_t crc32(const void *data, size_t length)
{
	const uint8_t *bytes = data;
	uint32_t crc = 0xffffffff;

	while (length--) {
		crc ^= *bytes++;
		for (int bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (0xedb88320 & -(crc & 1));
	}
	return ~crc;
}


static bool load(const char *name)
{
	int handle = file_open(name, FILE_OPEN_READ);

	if (handle < 0)
		return false;
	ssize_t n = file_read(handle, mp3, sizeof(mp3));
	file_close(handle);
	if (n <= 0)
		return false;
	mp3_length = n;
	return true;
}


static bool exists(const char *name)
{
	unsigned long length;

	return FILE_ERROR_NO_FILE != file_size(name, &length);
}


static bool write_new(const char *name, const void *data, size_t length)
{
	int handle = file_open(name, FILE_OPEN_WRITE | FILE_OPEN_CREATE);

	if (handle < 0)
		return false;
	ssize_t written = file_write(handle, (void *)data, length);
	file_sync(handle);
	file_close(handle);
	return written == (ssize_t)length;
}


typedef void rom_void(void);
typedef int rom_frame(void *half);


// One input through the decoder: returns frames decoded.
static unsigned decode(unsigned mode, uint8_t fill, unsigned *status, unsigned long *cycles,
		       unsigned *samples, unsigned *footprint, unsigned *ivram_touched)
{
	uint32_t *state = (uint32_t *)(workspace + 0x2400);
	uint32_t *dma = (uint32_t *)(workspace + 0x244c);
	unsigned frames = 0;

	memset(workspace, fill, sizeof(workspace));
	memset(input, 0, sizeof(input));
	mp3_position = 0;
	reads = 0;

	// what `open` sets, less the pins, interrupts and DMA
	state[S_STATUS] = 0;
	state[S_DECODER] = (uint32_t)(workspace + 0x246c);
	state[S_STATUS_CALLBACK] = 0;
	state[S_READ] = (uint32_t)read_input;
	state[S_INPUT] = (uint32_t)input;
	state[S_WORKSPACE] = (uint32_t)workspace;
	state[S_INPUT_BYTES] = 0xf80;
	state[S_OUTPUT_BYTES] = 0x2400;
	state[S_READ_SIZE] = (0xf80 >> 1) - 0x3c0;
	state[S_CLOCK] = 60000000;
	state[S_MODE] = mode;
	state[S_PLAY] = 1;
	dma[0] = 0;
	dma[2] = (uint32_t)workspace;
	dma[3] = (uint32_t)workspace + HALF_BYTES;
	dma[4] = 0x2400 >> 2;
	dma[1] = dma[2];

	critical_t interrupts = critcal_enter();
	uint32_t iram = *(volatile uint32_t *)LCDC_IRAM;
	*(volatile uint32_t *)LCDC_IRAM = 1;
	uint8_t *ivram = (uint8_t *)IVRAM_AREA0;
	memcpy(ivram_saved, ivram, IVRAM_BYTES);
	memset(ivram, GUARD, IVRAM_BYTES);
	*(volatile uint32_t *)STATE_POINTER = (uint32_t)state;
	*(volatile uint32_t *)DMA_POINTER = (uint32_t)dma;

	*status = 0;
	*cycles = 0;
	if (!dry) {
		((rom_void *)ROM_RESET_FLAGS)();
		((rom_void *)ROM_PRIME)();
	}
	while (!dry && frames < MAX_FRAMES && !state[S_END]) {
		void *half = (void *)dma[2 + (frames & 1)];
		state[S_HALF] = (uint32_t)half;
		unsigned long t0 = timer_get();
		int ok = ((rom_frame *)ROM_DECODE_FRAME)(half);
		*cycles += timer_get() - t0;
		watchdog(WATCHDOG_KEY);
		if (!ok) {
			*status = *(uint32_t *)(state[S_DECODER] + 0x64);
			break;
		}
		memcpy(pcm + frames * HALF_BYTES, half, HALF_BYTES);
		++frames;
	}
	*samples = dma[5];

	memcpy(ivram_after, ivram, IVRAM_BYTES);
	memcpy(ivram, ivram_saved, IVRAM_BYTES);
	*(volatile uint32_t *)LCDC_IRAM = iram;
	critical_exit(interrupts);

	// the highest workspace byte that no longer holds the fill
	*footprint = 0;
	for (unsigned i = sizeof(workspace); i-- > 0; )
		if (workspace[i] != fill) {
			*footprint = i + 1;
			break;
		}
	*ivram_touched = 0;
	for (unsigned i = 0; i < IVRAM_BYTES; ++i)
		*ivram_touched += ivram_after[i] != GUARD;
	return frames;
}


int grifo_main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	lcd_clear(LCD_WHITE);
	lcd_at_xy(0, 0);
	lcd_print("mp3rom: decoding; the screen\nwill be blank for a while\n");

	directory_create("0:/mp3test/out");
	unsigned run = 0;
	for (unsigned r = 1; r <= MAX_RUNS && !run; ++r) {
		char name[40];
		snprintf(name, sizeof(name), "0:/mp3test/out/r%03u.txt", r);
		// created now, so the number is taken even if a decode never returns
		if (!exists(name))
			report_handle = file_open(name, FILE_OPEN_WRITE | FILE_OPEN_CREATE);
		if (report_handle >= 0)
			run = r;
	}
	if (!run) {
		lcd_print("no run number left\n");
		return 1;
	}
	dry = exists("0:/mp3test/dry");
	say("mp3rom 2, run %03u%s", run, dry ? ", dry" : "");

	// The last pass runs over a workspace of 0xa5 rather than 0, to see what
	// the decoder reads before writing; last, since garbage it takes for a
	// pointer could stop the machine.
	static const struct {
		const char *file;
		char tag;
		unsigned mode;
		bool guard;
	} passes[] = {
		// t44s mode 0 again, as a check that this build decodes what
		// run 001 did; then the open questions.  Pink noise fills the
		// band, so where the output stops says whether the decoder
		// drops a fixed frequency or a fixed count of subbands; mono
		// says where its interpolation carry comes from with one
		// channel; and nothing says the header parser reads MPEG-2.5.
		{ "0:/mp3test/t44s.mp3", 'a', 0, false },
		{ "0:/mp3test/n48s.mp3", 'd', 0, false },      // MPEG-1 48 kHz
		{ "0:/mp3test/n32s.mp3", 'e', 0, false },      // MPEG-1 32 kHz
		{ "0:/mp3test/n22s.mp3", 'f', 0, false },      // MPEG-2 22.05 kHz
		{ "0:/mp3test/n44m.mp3", 'h', 0, false },      // mono
		{ "0:/mp3test/n44m.mp3", 'h', 1, false },      // mono, interpolated
		{ "0:/mp3test/n11s.mp3", 'i', 0, false },      // MPEG-2.5 11.025 kHz
	};
	const char *loaded = NULL;
	for (unsigned k = 0; k < sizeof(passes) / sizeof(passes[0]); ++k) {
		if (loaded != passes[k].file) {
			loaded = NULL;
			if (!load(passes[k].file)) {
				say("%s: not loaded", passes[k].file);
				report_flush();
				continue;
			}
			loaded = passes[k].file;
			say("%s: %u bytes, crc32 %08lx", loaded + 11, (unsigned)mp3_length,
			    (unsigned long)crc32(mp3, mp3_length));
		}
		report_flush();
		{
			bool guard = passes[k].guard;
			unsigned mode = passes[k].mode;
			unsigned status, samples, footprint, ivram;
			unsigned long cycles;
			unsigned frames = decode(mode, guard ? GUARD : 0, &status, &cycles,
						 &samples, &footprint, &ivram);
			say(" mode %u%s: %u frames, status %u, %u reads, %u samples/frame, %lu us/frame, "
			    "workspace used %#x, IVRAM bytes touched %u, pcm crc32 %08lx",
			    mode, guard ? " (0xa5 fill)" : "", frames, status, reads, samples,
			    frames ? cycles / TIMER_CountsPerMicroSecond / frames : 0,
			    footprint, ivram, (unsigned long)crc32(pcm, frames * HALF_BYTES));
			char name[40];
			snprintf(name, sizeof(name), "0:/mp3test/out/r%03u%c%c.pcm", run, passes[k].tag,
				 guard ? 'g' : '0' + mode);
			if (!write_new(name, pcm, frames * HALF_BYTES))
				say(" %s: not written", name + 15);
			if (guard) {
				snprintf(name, sizeof(name), "0:/mp3test/out/r%03uiv.bin", run);
				write_new(name, ivram_after, IVRAM_BYTES);
			}
			report_flush();
		}
	}
	say("done");
	report_flush();
	file_close(report_handle);

	lcd_clear(LCD_WHITE);
	lcd_at_xy(0, 0);
	lcd_print(report);
	lcd_print("press any button\n");
	event_flush();
	for (;;) {
		event_t event;
		event_item_t item = event_wait(&event, NULL, NULL);
		if (EVENT_BUTTON_DOWN == item || EVENT_TOUCH_DOWN == item || EVENT_KEY == item)
			break;
	}
	return 0;
}
