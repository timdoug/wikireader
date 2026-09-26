/*
 * Peanut-GB behind the WikiReader's display: every scanline is rendered
 * straight into the panel's one-bit framebuffer (render.h).
 * SPDX-License-Identifier: MIT
 */
#include "gb.h"

#include "fast_types.h"

#define ENABLE_SOUND 0
/* Shades only: the object/background palette bits would need masking off
   every pixel, and a one-bit panel has no use for them. */
#define PEANUT_GB_12_COLOUR 0
/* Peanut's renderer only runs under GBW_CHECK_RENDER, as the reference. */
struct gb_s;
static void render_line(struct gb_s *g);
/* The renderer runs from A0 RAM, beyond the reach of a call from SDRAM. */
static void (*volatile draw_line)(struct gb_s *g) = render_line;
#define PEANUT_GB_DRAW_LINE draw_line
/* memory.h */
static inline uint8_t fast_read(struct gb_s *g, uint_fast16_t address);
static inline void fast_write(struct gb_s *g, uint_fast16_t address,
			      uint8_t value);
static inline uint32_t batch(struct gb_s *g, uint32_t cycles);
static inline void ticked(struct gb_s *g);
static void catch_up(struct gb_s *g);
#define PEANUT_GB_READ fast_read
#define PEANUT_GB_WRITE fast_write
#define PEANUT_GB_BATCH batch
#define PEANUT_GB_TICKED ticked
#define PEANUT_GB_HALT catch_up
static void step_ticks(struct gb_s *g, uint32_t cycles);
#define PEANUT_GB_TICKS step_ticks
#include "peanut_gb.h"

/* The machine.  On the C33 it lives in the kernel's default framebuffer,
   6.6 KB of zero-wait IVRAM that nothing shows once gameboy.c has drawn
   its controls and moved the panel to its own buffers (memory.lds places
   it); in SDRAM every field the timing and the renderer touch was a
   data-queue fill.  Work RAM and VRAM are too big for it and stay out. */
#ifdef __c33__
extern struct gb_s gb __asm__("gbw_machine");
/* memory.lds gives it the framebuffer's first kilobyte. */
_Static_assert(sizeof(struct gb_s) <= 1024, "the machine outgrew its kilobyte");
#else
static struct gb_s gb;
#endif
static uint8_t wram[WRAM_SIZE] __attribute__((aligned(4)));
static uint8_t vram[VRAM_SIZE] __attribute__((aligned(4)));
static const uint8_t *rom;
static uint8_t *cart_ram;
static size_t cart_ram_bytes;
static uint8_t *framebuffer;
static unsigned lines_drawn;
static int dithered = 1;

static uint8_t rom_read(struct gb_s *g, const uint_fast32_t address)
{
	(void)g;
	return rom[address];
}

#ifdef GBW_LOCKSTEP
/* The reference instance's own cartridge RAM. */
static uint8_t *reference_ram;
#define CART_RAM(g) ((g) == &gb ? cart_ram : reference_ram)
#else
#define CART_RAM(g) cart_ram
#endif

static uint8_t cart_ram_read(struct gb_s *g, const uint_fast32_t address)
{
	(void)g;
	return address < cart_ram_bytes ? CART_RAM(g)[address] : 0xff;
}

static void cart_ram_write(struct gb_s *g, const uint_fast32_t address,
			   const uint8_t value)
{
	(void)g;
	if (address < cart_ram_bytes)
		CART_RAM(g)[address] = value;
}

static void error(struct gb_s *g, const enum gb_error_e code,
		  const uint16_t address)
{
	static const char *const names[GB_INVALID_MAX] = {
		"unknown error", "invalid opcode", "invalid read",
		"invalid write", "halted forever",
	};

	(void)g;
	gbw_error(code < GB_INVALID_MAX ? names[code] : "error", address);
}

/* noipa: ticks() calls these through Peanut's pointers from the window
   buffer, and gcc otherwise speculates on the pointer and calls them, or
   what they call, directly -- out of reach of SDRAM. */
#ifdef __clang__
#define NO_IPA __attribute__((noinline))
#else
#define NO_IPA __attribute__((noipa))
#endif
static NO_IPA void serial_tx(struct gb_s *g, const uint8_t byte)
{
	(void)g;
	gbw_serial_out(byte);
}

static NO_IPA enum gb_serial_rx_ret_e
serial_rx(struct gb_s *g, uint8_t *byte)
{
	(void)g;
	(void)byte;
	return GB_SERIAL_RX_NO_CONNECTION;
}

#include "render.h"
#include "memory.h"

size_t gbw_rom_bytes(const uint8_t *header, size_t file_bytes)
{
	/* 0x148: 32 KiB << n.  Peanut masks bank numbers by this, so every
	   address below it is one the game can read. */
	size_t declared = header[0x148] <= 8 ? (size_t)0x8000 << header[0x148] : 0;

	return declared > file_bytes ? declared : file_bytes;
}

const char *gbw_init(uint8_t *image, size_t file_bytes)
{
	memset(&gb, 0, sizeof gb);
	gb.wram = wram;
	gb.vram = vram;
	size_t padded = gbw_rom_bytes(image, file_bytes);

	for (size_t i = file_bytes; i < padded; ++i)
		image[i] = 0xff;
	rom = image;
	build_reversed();

	switch (gb_init(&gb, rom_read, cart_ram_read, cart_ram_write, error,
			NULL)) {
	case GB_INIT_NO_ERROR:
		break;
	case GB_INIT_CARTRIDGE_UNSUPPORTED:
		return "unsupported cartridge type";
	case GB_INIT_INVALID_CHECKSUM:
		return "bad header checksum";
	default:
		return "cannot start";
	}
#ifdef GBW_CHECK_RENDER
	gb_init_lcd(&gb, check_line);
#else
	/* Peanut's lcd_draw_line is left unset: render_line draws. */
	gb_init_lcd(&gb, 0);
#endif
	gb_init_serial(&gb, serial_tx, serial_rx);
	map_regions();
	map_pages(&gb);
	hot.hram = (uintptr_t)gb.hram_io;
	hot.hram_biased = (uintptr_t)gb.hram_io - 0xff00;
	return 0;
}

size_t gbw_save_bytes(void)
{
	size_t bytes;

	return gb_get_save_size_s(&gb, &bytes) == 0 ? bytes : 0;
}

void gbw_set_cart_ram(uint8_t *ram, size_t bytes)
{
	cart_ram = ram;
	cart_ram_bytes = bytes;
	map_pages(&gb);
}

const char *gbw_title(void)
{
	static char title[17];

	return gb_get_rom_name(&gb, title);
}

void gbw_set_framebuffer(uint8_t *fb)
{
	framebuffer = fb;
}

void gbw_set_buttons(uint8_t buttons)
{
	gb.direct.joypad = (uint8_t)~buttons;
}

void gbw_set_dither(int on)
{
	dithered = on;
}

void gbw_set_frame_skip(int skip)
{
	gb.direct.frame_skip = skip != 0;
}


static uint32_t fnv(uint32_t hash, const uint8_t *bytes, size_t length)
{
	while (length--)
		hash = (hash ^ *bytes++) * 16777619u;
	return hash;
}

uint32_t gbw_hash(const uint8_t *picture)
{
	uint32_t hash = 2166136261u;

	for (int y = 0; y < GBW_HEIGHT; ++y)
		hash = fnv(hash, picture + (GBW_TOP + y) * GBW_STRIDE
			   + GBW_LEFT_BYTE, GBW_WIDTH / 8);
	hash = fnv(hash, gb.wram, WRAM_SIZE);
	hash = fnv(hash, gb.vram, VRAM_SIZE);
	hash = fnv(hash, gb.oam, sizeof gb.oam);
	return fnv(hash, gb.hram_io, sizeof gb.hram_io);
}

/* "FRAME:BUTTONS,..." -- each entry holds its buttons from that frame
   until the next.  Letters are A, B, s(elect), S(tart), R, L, U, D; "-" is
   none. */
uint8_t gbw_script_buttons(const char *script, unsigned frame)
{
	uint8_t held = 0;

	while (script && *script) {
		unsigned at = 0;
		uint8_t buttons = 0;

		while (*script >= '0' && *script <= '9')
			at = at * 10 + (unsigned)(*script++ - '0');
		if (*script == ':')
			++script;
		for (; *script && *script != ','; ++script) {
			static const char letters[] = "ABsSRLUD";

			for (int bit = 0; letters[bit]; ++bit)
				if (*script == letters[bit])
					buttons |= (uint8_t)(1 << bit);
		}
		if (at > frame)
			break;
		held = buttons;
		if (*script == ',')
			++script;
	}
	return held;
}

#ifdef GBW_CHECK_RENDER
/* Random VRAM, OAM and LCD registers through both renderers, every line
   compared by check_line. */
void gbw_fuzz_render(unsigned rounds)
{
	uint32_t x = 2463534242u;

	for (unsigned round = 0; round < rounds; ++round) {
#define RANDOM() (x ^= x << 13, x ^= x >> 17, x ^= x << 5)
		for (size_t i = 0; i < VRAM_SIZE; ++i)
			gb.vram[i] = (uint8_t)RANDOM();
		for (size_t i = 0; i < sizeof gb.oam; ++i)
			gb.oam[i] = (uint8_t)RANDOM();
		/* Mostly on-screen sprites, and some crowded lines. */
		for (unsigned s = 0; s < NUM_SPRITES; ++s) {
			gb.oam[4 * s] = (uint8_t)(RANDOM() % 176);
			gb.oam[4 * s + 1] = (uint8_t)(round & 1 ? 40 + RANDOM() % 16
						       : RANDOM() % 176);
		}
		gb.hram_io[IO_LCDC] = (uint8_t)(RANDOM() | LCDC_ENABLE);
		gb.hram_io[IO_SCX] = (uint8_t)RANDOM();
		gb.hram_io[IO_SCY] = (uint8_t)RANDOM();
		gb.hram_io[IO_WX] = (uint8_t)(RANDOM() % 176);
		gb.hram_io[IO_BGP] = (uint8_t)RANDOM();
		gb.hram_io[IO_OBP0] = (uint8_t)RANDOM();
		gb.hram_io[IO_OBP1] = (uint8_t)RANDOM();
		gb.display.WY = (uint8_t)(RANDOM() % 160);
		gb.display.window_clear = 0;
		for (int i = 0; i < 4; ++i) {
			gb.display.bg_palette[i] = (gb.hram_io[IO_BGP] >> (2 * i)) & 3;
			gb.display.sp_palette[i] = (gb.hram_io[IO_OBP0] >> (2 * i)) & 3;
			gb.display.sp_palette[i + 4] = (gb.hram_io[IO_OBP1] >> (2 * i)) & 3;
		}
		sprites_dirty = 1;
		for (unsigned ly = 0; ly < LCD_HEIGHT; ++ly) {
			gb.hram_io[IO_LY] = (uint8_t)ly;
			render_line(&gb);
		}
#undef RANDOM
	}
}
#endif

#ifdef GBW_LOCKSTEP
#include <stdio.h>

/* Peanut as it comes, stepped beside the fast paths one instruction at a
   time: the registers must agree after every instruction, and everything
   else wherever no cycles are deferred. */
void ref___gb_step_cpu(struct gb_s *g);

static struct gb_s reference;
static unsigned long long steps;

static void reference_line(struct gb_s *g, const uint8_t *pixels,
			   const uint_fast8_t line)
{
	(void)g;
	(void)pixels;
	(void)line;
}

static void reference_serial(struct gb_s *g, const uint8_t byte)
{
	(void)g;
	(void)byte;
}

static void differ(const char *what)
{
	char text[96];

	snprintf(text, sizeof text, "lockstep: %s differs after step %llu, pc",
		 what, steps);
	for (int i = 0; i < 2; ++i) {
		struct gb_s *g = i ? &reference : &gb;

		fprintf(stderr, "%s: LCDC %02x STAT %02x LY %3u lcd %4u off %5u tima %4u div %3u IF %02x IE %02x halt %d frame %d pending %d budget %d\n",
			i ? "reference" : "fast", g->hram_io[IO_LCDC], g->hram_io[IO_STAT],
			g->hram_io[IO_LY], (unsigned)g->counter.lcd_count,
			(unsigned)g->counter.lcd_off_count, (unsigned)g->counter.tima_count,
			(unsigned)g->counter.div_count, g->hram_io[IO_IF], g->hram_io[IO_IE],
			g->gb_halt, g->gb_frame, (int)pending, (int)budget);
	}
	gbw_error(text, reference.cpu_reg.pc.reg);
}

static void compare(void)
{
	if (memcmp(&gb.cpu_reg, &reference.cpu_reg, sizeof gb.cpu_reg))
		differ("registers");
	if (gb.gb_ime != reference.gb_ime)
		differ("ime");
	if (gb.gb_halt != reference.gb_halt)
		differ("halt");
	if (gb.gb_frame != reference.gb_frame)
		differ("frame");
	if (pending)
		return;
	if (memcmp(gb.hram_io, reference.hram_io, sizeof gb.hram_io))
		differ("I/O and HRAM");
	if (memcmp(&gb.counter, &reference.counter, sizeof gb.counter))
		differ("counters");
	if (memcmp(gb.wram, reference.wram, WRAM_SIZE))
		differ("WRAM");
	if (memcmp(gb.vram, reference.vram, VRAM_SIZE))
		differ("VRAM");
	if (memcmp(gb.oam, reference.oam, sizeof gb.oam))
		differ("OAM");
	if (gb.selected_rom_bank != reference.selected_rom_bank
	    || gb.cart_ram_bank != reference.cart_ram_bank
	    || gb.enable_cart_ram != reference.enable_cart_ram
	    || gb.cart_mode_select != reference.cart_mode_select)
		differ("banking");
	if (gb.display.WY != reference.display.WY
	    || gb.display.window_clear != reference.display.window_clear)
		differ("window");
	if (cart_ram_bytes && memcmp(cart_ram, reference_ram, cart_ram_bytes))
		differ("cartridge RAM");
}

unsigned gbw_run_frame(void)
{
	if (!steps) {
		static uint8_t reference_wram[WRAM_SIZE], reference_vram[VRAM_SIZE];

		reference = gb;
		reference.wram = memcpy(reference_wram, wram, WRAM_SIZE);
		reference.vram = memcpy(reference_vram, vram, VRAM_SIZE);
		reference.display.lcd_draw_line = reference_line;
		reference.gb_serial_tx = reference_serial;
		reference_ram = malloc(cart_ram_bytes ? cart_ram_bytes : 1);
		memcpy(reference_ram, cart_ram, cart_ram_bytes);
	}
	reference.direct.joypad = gb.direct.joypad;
	reference.direct.frame_skip = gb.direct.frame_skip;
	lines_drawn = 0;
	gb.gb_frame = false;
	reference.gb_frame = false;
	while (!gb.gb_frame) {
		__gb_step_cpu(&gb);
		ref___gb_step_cpu(&reference);
		++steps;
		compare();
	}
	return lines_drawn;
}
#elif defined GBW_HISTOGRAM
#include <stdio.h>
#include <stdlib.h>

/* How often each opcode runs, for choosing what the hot path covers. */
static unsigned long long opcodes[512];
static unsigned long long ldh[256];
/* LDH (n),A by n, and LD (nn),A by the high byte of nn. */
static unsigned long long ldh_write[256], far_write[256];

static void print_histogram(void)
{
	for (int i = 0; i < 512; ++i)
		if (opcodes[i])
			fprintf(stderr, "%s%02x %llu\n", i < 256 ? "" : "cb", i & 255,
				opcodes[i]);
	for (int i = 0; i < 256; ++i)
		if (ldh[i])
			fprintf(stderr, "ldh ff%02x %llu\n", i, ldh[i]);
	for (int i = 0; i < 256; ++i)
		if (ldh_write[i])
			fprintf(stderr, "ldh-write ff%02x %llu\n", i, ldh_write[i]);
	for (int i = 0; i < 256; ++i)
		if (far_write[i])
			fprintf(stderr, "ld-nn-a %02xxx %llu\n", i, far_write[i]);

}

unsigned gbw_run_frame(void)
{
	static int registered;

	if (!registered++)
		atexit(print_histogram);
	lines_drawn = 0;
	gb.gb_frame = false;
	while (!gb.gb_frame) {
		uint8_t op = __gb_read(&gb, gb.cpu_reg.pc.reg);

		if (op == 0xcb)
			++opcodes[256 + __gb_read(&gb, gb.cpu_reg.pc.reg + 1)];
		else if (op == 0xf0)
			++ldh[__gb_read(&gb, gb.cpu_reg.pc.reg + 1)];
		else if (op == 0xe0)
			++ldh_write[__gb_read(&gb, gb.cpu_reg.pc.reg + 1)];
		else if (op == 0xea)
			++far_write[__gb_read(&gb, gb.cpu_reg.pc.reg + 2)];


		else
			++opcodes[op];
		__gb_step_cpu(&gb);
	}
	return lines_drawn;
}
#elif defined GBW_HOT
/* hot.s runs the common instructions; C takes interrupts, the instructions
   hot.s gives back, and the events at the end of a batch. */
static void to_hot(void)
{
	uint8_t f = gb.cpu_reg.f.reg;

	hot.a = gb.cpu_reg.a;
	hot.bc = gb.cpu_reg.bc.reg;
	hot.de = gb.cpu_reg.de.reg;
	hot.hl = gb.cpu_reg.hl.reg;
	hot.sp = gb.cpu_reg.sp.reg;
	hot.pc = gb.cpu_reg.pc.reg;
	hot.zv = f & 0x80 ? 0 : 1;
	hot.cf = f & 0x10 ? 0x80000000u : 0;
	hot.hx = (f & 0x20 ? 0x10u : 0) | (f & 0x40 ? 0x200u : 0);
	hot.left = budget - pending;
}

static void from_hot(void)
{
	gb.cpu_reg.a = (uint8_t)hot.a;
	gb.cpu_reg.bc.reg = (uint16_t)hot.bc;
	gb.cpu_reg.de.reg = (uint16_t)hot.de;
	gb.cpu_reg.hl.reg = (uint16_t)hot.hl;
	gb.cpu_reg.sp.reg = (uint16_t)hot.sp;
	gb.cpu_reg.pc.reg = (uint16_t)hot.pc;
	gb.cpu_reg.f.reg = (uint8_t)((hot.zv ? 0 : 0x80) | ((hot.hx >> 3) & 0x40)
		| ((hot.hx << 1) & 0x20) | ((hot.cf >> 27) & 0x10));
	pending = budget - hot.left;
}

/* Called by hot.s, registers pushed, when its budget is spent at an event:
   run the event, and let it go on unless an interrupt is pending or the
   frame is over.  Either way no cycles are deferred afterwards.  In the
   window buffer with ticks(), which it calls three times a scanline. */
TICK_CODE void gb_hot_event(void)
{
	uint32_t cycles = (uint32_t)(budget - hot.left);

	pending = 0;
	budget = event_tick(&gb, cycles);
	hot.left = budget;
	hot.go = !gb.gb_frame && !(gb.gb_ime && gb.hram_io[IO_IF]
				   & gb.hram_io[IO_IE] & ANY_INTR);
	++gbw_counts.events;
}

/* What the loop did, for the benchmark report: calls into hot.s, the
   instructions it gave back by opcode, and steps taken for interrupts. */
struct gbw_counts gbw_counts;

unsigned gbw_run_frame(void)
{
	lines_drawn = 0;
	gb.gb_frame = false;
	while (!gb.gb_frame) {
		if (gb.gb_halt || (gb.gb_ime && gb.hram_io[IO_IF]
				   & gb.hram_io[IO_IE] & ANY_INTR)) {
			++gbw_counts.interrupts;
			__gb_step_cpu(&gb);
			continue;
		}
		to_hot();
		++gbw_counts.hot_calls;
		int given_back = gb_hot_run();
		from_hot();
		if (given_back == 2) {
			/* gb_hot_event ran the event: an interrupt or the
			   frame's end is next. */
		} else if (given_back) {
			++gbw_counts.given_back[__gb_read(&gb, gb.cpu_reg.pc.reg)];
			__gb_step_cpu(&gb);
		} else {
			/* The batch is spent at an event: what Peanut's step
			   does when PEANUT_GB_BATCH hands it the cycles. */
			uint32_t cycles = (uint32_t)pending;

			pending = 0;
			(void)fast_tick(&gb, cycles);
			ticked(&gb);
		}
	}
	return lines_drawn;
}
#else
unsigned gbw_run_frame(void)
{
	lines_drawn = 0;
	gb_run_frame(&gb);
	return lines_drawn;
}
#endif
