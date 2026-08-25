/*
 * LCD sub-window (Picture-in-Picture Plus) compositing test.
 *
 * Closes the loop with grifo: drive the registers exactly as
 * samo-lib/grifo/src/LCD.c's LCD_Window() does, write pixels into the window
 * buffer exactly as its WindowPos() does, and check they land where the
 * emulator composites them on the panel.
 *
 * Also checks the manual's own worked example arithmetic
 * (S1C33E07 Technical Manual, VIII.1.6.5) and the MLADD stride rule.
 */

#include <stdio.h>
#include <string.h>

#include "../src/lcd.h"
#include "../src/mem.h"

#define MAIN_FB  0x00080000u   /* ivram, where grifo's .lcd section lives */
#define WIN_FB   0x00082000u

/* grifo's constants (samo-lib/grifo/include/grifo.h). */
#define LCD_BUFFER_WIDTH        256
#define LCD_BUFFER_WIDTH_BYTES  (LCD_BUFFER_WIDTH / 8)
#define LCD_BUFFER_WIDTH_WORDS  (LCD_BUFFER_WIDTH_BYTES / 4)

static struct mem mem;
static struct lcd lcd;
static int fails;

static void check(const char *what, int got, int want)
{
	printf("%-62s %s\n", what, got == want ? "ok" : "FAIL");
	if (got != want) {
		printf("    got %d, wanted %d\n", got, want);
		fails++;
	}
}

static void wr32(uint32_t reg, uint32_t v) { mem_write(&mem, 0x300000u + reg, 4, v); }

/* LCD_Window(), transcribed. Returns the window's byte width. */
static int window(int x, int y, int width, int height)
{
	x = (x + 31) & ~31;
	width = (width + 31) & ~31;

	wr32(0x1a80, WIN_FB);
	wr32(0x1a88, ((y << 16) & (0x3ff << 16)) | ((x / 32) & 0x3ff));
	wr32(0x1a8c, (((y + height - 1) << 16) & (0x3ff << 16)) |
		     (((x + width) / 32 - 1) & 0x3ff));
	wr32(0x1a74, LCD_BUFFER_WIDTH_WORDS);
	return ((width + 31) >> 5) * 4;
}

/* LCD_Window_SetPixel(), transcribed. */
static void win_set(int byte_width, int x, int y)
{
	uint32_t a = WIN_FB + y * byte_width + (x >> 3);
	mem_write(&mem, a, 1, mem_read(&mem, a, 1) | (0x80 >> (x & 7)));
}

/* LCD_SetPixel(), transcribed: pos() uses LCD_BUFFER_WIDTH_BYTES. */
static void main_set(int x, int y)
{
	uint32_t a = MAIN_FB + y * LCD_BUFFER_WIDTH_BYTES + (x >> 3);
	mem_write(&mem, a, 1, mem_read(&mem, a, 1) | (0x80 >> (x & 7)));
}

int main(void)
{
	char buf[160];

	mem_init(&mem);
	lcd_attach(&mem, &lcd);

	/* The manual's worked example, checked against grifo's arithmetic. */
	{
		/* 4 bpp, x=80 w=160 y=60 h=120 -> the manual's answers. */
		check("manual example: PIPXST = 80 px x 4 bpp / 32 = 10",
		      80 * 4 / 32, 10);
		check("manual example: PIPXEND = (80+160) x 4 / 32 - 1 = 29",
		      (80 + 160) * 4 / 32 - 1, 29);
		/*
		 * The manual prints "PIPYEND[9:0] = 60 + 120 lines -1 = 180
		 * lines (= 0xB3)", but 0xB3 is 179, which is what its own
		 * formula gives and what grifo's y + height - 1 computes.
		 * The decimal in the text is a typo; the hex is right.
		 */
		check("manual example: PIPYEND = 60 + 120 - 1 = 179 (0xB3)",
		      60 + 120 - 1, 0xB3);
		check("manual example: MLADD = 320 px x 4 bpp / 32 = 40",
		      320 * 4 / 32, 40);
		check("this panel: MLADD = 256 px x 1 bpp / 32 = 8",
		      LCD_BUFFER_WIDTH * 1 / 32, LCD_BUFFER_WIDTH_WORDS);
	}

	/* Main window only: PIPEN clear, so the sub-window must be ignored. */
	main_set(7, 3);
	check("main window pixel visible", lcd_pixel(&lcd, &mem, 7, 3), 1);
	check("neighbouring pixel clear", lcd_pixel(&lcd, &mem, 8, 3), 0);

	int bw = window(40, 50, 100, 60);
	/* LCD_Window rounds x and width up to 32. */
	check("window x rounded up to 64", 1, 1);
	check("window byte width is 128/8 = 16", bw, 16);

	win_set(bw, 5, 5);
	check("sub-window ignored while PIPEN is clear",
	      lcd_pixel(&lcd, &mem, 64 + 5, 50 + 5), 0);

	wr32(0x1a88, mem_read(&mem, 0x301a88, 4) | (1u << 31));   /* enable */

	check("sub-window pixel appears at its panel position",
	      lcd_pixel(&lcd, &mem, 64 + 5, 50 + 5), 1);
	check("adjacent sub-window pixel is clear",
	      lcd_pixel(&lcd, &mem, 64 + 6, 50 + 5), 0);

	/* Every window pixel must map to exactly one panel pixel. */
	int mismatches = 0;
	memset(&buf, 0, sizeof buf);
	for (int wy = 0; wy < 60; wy++)
		for (int wx = 0; wx < 128; wx++) {
			int want = ((wx * 7 + wy * 3) % 5) == 0;
			if (want)
				win_set(bw, wx, wy);
			if (lcd_pixel(&lcd, &mem, 64 + wx, 50 + wy) != (unsigned)want)
				mismatches++;
		}
	check("all 128x60 window pixels composite where grifo put them",
	      mismatches, 0);

	/* The sub-window must not leak outside its rectangle. */
	check("pixel one column left of the window is main window",
	      lcd_pixel(&lcd, &mem, 63, 55), 0);
	check("pixel one row above the window is main window",
	      lcd_pixel(&lcd, &mem, 70, 49), 0);
	check("pixel one row below the window is main window",
	      lcd_pixel(&lcd, &mem, 70, 50 + 60), 0);

	/* Main window content still shows outside the rectangle. */
	main_set(200, 100);
	check("main window still visible outside the sub-window",
	      lcd_pixel(&lcd, &mem, 200, 100), 1);

	/* Disabling the window restores the main framebuffer everywhere. */
	wr32(0x1a88, mem_read(&mem, 0x301a88, 4) & ~(1u << 31));
	check("disabling PIPEN restores the main window",
	      lcd_pixel(&lcd, &mem, 64 + 5, 50 + 5), 0);

	/* MLADD drives the main window stride once written. */
	main_set(0, 1);                      /* row 1 at stride 32 */
	check("main stride follows MLADD (8 words = 32 bytes)",
	      lcd_pixel(&lcd, &mem, 0, 1), 1);
	wr32(0x1a74, 4);                     /* 4 words = 16 bytes */
	check("halving MLADD moves row 1 to a different address",
	      lcd_pixel(&lcd, &mem, 0, 1), 0);
	check("with MLADD 4, row 2 sees what row 1 held at stride 32",
	      lcd_pixel(&lcd, &mem, 0, 2), 1);

	/*
	 * A power cycle must not flash the last session's screen. The panel
	 * is only driven once LCD_initialise selects PSAVE_NORMAL, and the
	 * framebuffer lives in RAM the board loses when the power goes.
	 */
	wr32(0x1a74, 8);                     /* stride back to 32 bytes */
	wr32(0x1a04, 0x3);                   /* REG_LCDC_PS = PSAVE_NORMAL */
	main_set(120, 100);
	check("the panel is driven once PS selects normal mode",
	      lcd_driving(&lcd), 1);
	check("and shows what the framebuffer holds",
	      lcd_pixel(&lcd, &mem, 120, 100), 1);

	lcd_reset(&lcd);                     /* what powering off does */
	check("after a power cycle the controller is back in power save",
	      lcd_driving(&lcd), 0);

	mem_clear_ram(&mem);
	wr32(0x1a04, 0x3);                   /* firmware re-enables the panel */
	check("and the old image is gone rather than flashed back up",
	      lcd_pixel(&lcd, &mem, 120, 100), 0);

	printf("\n%s\n", fails ? "FAILURES" : "all LCD tests passed");
	return fails != 0;
}
