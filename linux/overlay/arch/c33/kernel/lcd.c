// SPDX-License-Identifier: GPL-2.0
#include <linux/console.h>
#include <linux/font.h>
#include <linux/init.h>

#include <asm/wikireader.h>

/*
 * The card loader leaves the WikiReader LCD scanning this internal-RAM
 * framebuffer.  Keep this renderer independent of the framebuffer and VT
 * subsystems: it is an observability path for the failures that happen before
 * either of those could exist.
 */
#define C33_LCD_FB             ((volatile u8 *)0x00080000UL)
#define C33_LCD_WIDTH          240
#define C33_LCD_HEIGHT         208
#define C33_LCD_STRIDE         32
#define C33_LCD_FONT_WIDTH     6
#define C33_LCD_FONT_HEIGHT    8
#define C33_LCD_COLUMNS        (C33_LCD_WIDTH / C33_LCD_FONT_WIDTH)
#define C33_LCD_TEXT_ROWS      24
#define C33_LCD_STATUS_Y       (C33_LCD_TEXT_ROWS * C33_LCD_FONT_HEIGHT)
#define C33_LCD_STATUS_SIZE    6
#define C33_LCD_STATUS_PITCH   16

static unsigned int c33_lcd_column;
static unsigned int c33_lcd_row;
static bool c33_lcd_console_registered;
static void c33_lcd_clear_rows(unsigned int first, unsigned int count)
{
	volatile u8 *fb = C33_LCD_FB + first * C33_LCD_STRIDE;
	unsigned int bytes = count * C33_LCD_STRIDE;

	while (bytes--)
		*fb++ = 0;
}

static void c33_lcd_scroll(void)
{
	volatile u8 *fb = C33_LCD_FB;
	unsigned int source = C33_LCD_FONT_HEIGHT * C33_LCD_STRIDE;
	unsigned int bytes = (C33_LCD_STATUS_Y - C33_LCD_FONT_HEIGHT) *
		C33_LCD_STRIDE;
	unsigned int i;

	for (i = 0; i < bytes; i++)
		fb[i] = fb[source + i];
	c33_lcd_clear_rows(C33_LCD_STATUS_Y - C33_LCD_FONT_HEIGHT,
			   C33_LCD_FONT_HEIGHT);
}

static void c33_lcd_newline(void)
{
	c33_lcd_column = 0;
	if (++c33_lcd_row == C33_LCD_TEXT_ROWS) {
		c33_lcd_scroll();
		c33_lcd_row--;
	}
}

static void c33_lcd_putc(unsigned char ch)
{
	const u8 *glyph;
	volatile u8 *destination;
	u16 mask;
	u16 pixels;
	unsigned int x;
	unsigned int offset;
	unsigned int y;

	if (ch == '\r') {
		c33_lcd_column = 0;
		return;
	}
	if (ch == '\n') {
		c33_lcd_newline();
		return;
	}
	if (ch == '\t') {
		do {
			c33_lcd_putc(' ');
		} while (c33_lcd_column & 3);
		return;
	}
	if (ch < 32 || ch >= font_6x8.charcount)
		ch = '?';

	glyph = font_6x8.data + ch * C33_LCD_FONT_HEIGHT;
	x = c33_lcd_column * C33_LCD_FONT_WIDTH;
	offset = x & 7;
	mask = 0xfc00U >> offset;
	for (y = 0; y < C33_LCD_FONT_HEIGHT; y++) {
		destination = C33_LCD_FB +
			(c33_lcd_row * C33_LCD_FONT_HEIGHT + y) *
			C33_LCD_STRIDE + (x >> 3);
		pixels = ((u16)glyph[y] << 8) >> offset;
		destination[0] = (destination[0] & ~(mask >> 8)) |
			(pixels >> 8);
		destination[1] = (destination[1] & ~(u8)mask) |
			(u8)pixels;
	}

	if (++c33_lcd_column == C33_LCD_COLUMNS)
		c33_lcd_newline();
}

void c33_lcd_init(void)
{
	static const char banner[] = "C33 LINUX\n";
	unsigned int i;

	c33_lcd_clear_rows(0, C33_LCD_HEIGHT);
	c33_lcd_column = 0;
	c33_lcd_row = 0;
	for (i = 0; i < sizeof(banner) - 1; i++)
		c33_lcd_putc(banner[i]);
	c33_lcd_checkpoint(0);
}

static void c33_lcd_console_write(struct console *console, const char *text,
				  unsigned int length)
{
	unsigned int i;

	for (i = 0; i < length; i++)
		c33_lcd_putc(text[i]);
}

static struct console c33_lcd_console = {
	.name = "c33lcd",
	.write = c33_lcd_console_write,
	.flags = CON_PRINTBUFFER | CON_ENABLED | CON_ANYTIME,
	.index = 0,
};

void __init c33_lcd_console_register(void)
{
	register_console(&c33_lcd_console);
	c33_lcd_console_registered = true;
}

static int __init c33_lcd_console_unregister(void)
{
	if (c33_lcd_console_registered) {
		unregister_console(&c33_lcd_console);
		c33_lcd_console_registered = false;
	}
	return 0;
}
late_initcall(c33_lcd_console_unregister);

void c33_lcd_checkpoint(unsigned int stage)
{
	volatile u8 *fb;
	unsigned int x;
	unsigned int y;

	x = stage * C33_LCD_STATUS_PITCH;
	if (x + C33_LCD_STATUS_SIZE > C33_LCD_WIDTH)
		return;
	for (y = 0; y < C33_LCD_STATUS_SIZE; y++) {
		fb = C33_LCD_FB + (C33_LCD_STATUS_Y + y) * C33_LCD_STRIDE;
		fb[x >> 3] = 0xff;
	}
}

void c33_lcd_fault(unsigned int vector)
{
	volatile u8 *fb;
	unsigned int x;
	unsigned int y;

	/* A full-width black bar is unmistakable even if text output is wedged. */
	for (y = C33_LCD_STATUS_Y; y < C33_LCD_HEIGHT; y++) {
		fb = C33_LCD_FB + y * C33_LCD_STRIDE;
		for (x = 0; x < C33_LCD_WIDTH / 8; x++)
			fb[x] = 0xff;
	}

	/* Cut the low six vector bits into the bar as white squares. */
	for (x = 0; x < 6; x++) {
		if (vector & (1U << x))
			continue;
		for (y = C33_LCD_STATUS_Y + 4; y < C33_LCD_HEIGHT - 4; y++)
			C33_LCD_FB[y * C33_LCD_STRIDE + 2 + x * 2] = 0;
	}
}
