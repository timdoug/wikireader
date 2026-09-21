// SPDX-License-Identifier: GPL-2.0
#include <linux/font.h>
#include <linux/spinlock.h>
#include <linux/string.h>

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
#define C33_LCD_FONT_WIDTH     8
#define C33_LCD_FONT_HEIGHT    16
#define C33_LCD_COLUMNS        (C33_LCD_WIDTH / C33_LCD_FONT_WIDTH)
#define C33_LCD_TEXT_ROWS      7
#define C33_LCD_STATUS_Y       (C33_LCD_TEXT_ROWS * C33_LCD_FONT_HEIGHT)
#define C33_LCD_STATUS_SIZE    6
#define C33_LCD_STATUS_PITCH   16
#define C33_LCD_KEYBOARD_Y     120
#define C33_LCD_KEY_WIDTH      24
#define C33_LCD_KEY_ROWS       3

static DEFINE_RAW_SPINLOCK(c33_lcd_lock);
static unsigned int c33_lcd_column;
static unsigned int c33_lcd_row;
static const char *const c33_lcd_key_labels[C33_LCD_KEY_ROWS][10] = {
	{ "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P" },
	{ "A", "S", "D", "F", "G", "H", "J", "K", "L", "BS" },
	{ "Z", "X", "C", "V", "SP", "SP", "B", "N", "M", "EN" },
};

static void c33_lcd_set_pixel(unsigned int x, unsigned int y)
{
	C33_LCD_FB[y * C33_LCD_STRIDE + (x >> 3)] |= 0x80 >> (x & 7);
}

static void c33_lcd_draw_key(unsigned int row, unsigned int column,
			     const char *label)
{
	unsigned int y0 = C33_LCD_KEYBOARD_Y +
		row * (C33_LCD_HEIGHT - C33_LCD_KEYBOARD_Y) /
		C33_LCD_KEY_ROWS;
	unsigned int y1 = C33_LCD_KEYBOARD_Y +
		(row + 1) * (C33_LCD_HEIGHT - C33_LCD_KEYBOARD_Y) /
		C33_LCD_KEY_ROWS - 1;
	unsigned int x0 = column * C33_LCD_KEY_WIDTH;
	unsigned int x1 = x0 + C33_LCD_KEY_WIDTH - 1;
	unsigned int gx = x0 + (C33_LCD_KEY_WIDTH -
		strlen(label) * C33_LCD_FONT_WIDTH) / 2;
	unsigned int gy = y0 + (y1 - y0 + 1 - C33_LCD_FONT_HEIGHT) / 2;
	unsigned int character;
	unsigned int x;
	unsigned int y;

	for (x = x0; x <= x1; x++) {
		c33_lcd_set_pixel(x, y0);
		c33_lcd_set_pixel(x, y1);
	}
	for (y = y0; y <= y1; y++) {
		c33_lcd_set_pixel(x0, y);
		c33_lcd_set_pixel(x1, y);
	}
	for (character = 0; label[character]; character++) {
		const u8 *glyph = font_vga_8x16.data +
			label[character] * C33_LCD_FONT_HEIGHT;

		for (y = 0; y < C33_LCD_FONT_HEIGHT; y++)
			C33_LCD_FB[(gy + y) * C33_LCD_STRIDE + (gx >> 3) +
				   character] |= glyph[y];
	}
}

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
	volatile u8 *cell;
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
	if (ch < 32 || ch >= font_vga_8x16.charcount)
		ch = '?';

	glyph = font_vga_8x16.data + ch * C33_LCD_FONT_HEIGHT;
	cell = C33_LCD_FB + c33_lcd_row * C33_LCD_FONT_HEIGHT *
		C33_LCD_STRIDE + c33_lcd_column;
	for (y = 0; y < C33_LCD_FONT_HEIGHT; y++)
		cell[y * C33_LCD_STRIDE] = glyph[y];

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

void c33_lcd_write(const char *text, size_t count)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&c33_lcd_lock, flags);
	while (count--)
		c33_lcd_putc(*text++);
	raw_spin_unlock_irqrestore(&c33_lcd_lock, flags);
}

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

void c33_lcd_keyboard_init(void)
{
	unsigned long flags;
	unsigned int column;
	unsigned int row;

	raw_spin_lock_irqsave(&c33_lcd_lock, flags);
	c33_lcd_clear_rows(C33_LCD_KEYBOARD_Y,
			   C33_LCD_HEIGHT - C33_LCD_KEYBOARD_Y);
	for (row = 0; row < C33_LCD_KEY_ROWS; row++)
		for (column = 0; column < 10; column++)
			c33_lcd_draw_key(row, column,
					 c33_lcd_key_labels[row][column]);
	raw_spin_unlock_irqrestore(&c33_lcd_lock, flags);
}

void c33_lcd_keyboard_press(int key, bool pressed)
{
	unsigned long flags;
	unsigned int column;
	unsigned int row;
	unsigned int y0;
	unsigned int y1;
	unsigned int y;

	if (key < 0 || key >= C33_LCD_KEY_ROWS * 10)
		return;
	row = key / 10;
	column = key % 10;
	y0 = C33_LCD_KEYBOARD_Y +
		row * (C33_LCD_HEIGHT - C33_LCD_KEYBOARD_Y) /
		C33_LCD_KEY_ROWS;
	y1 = C33_LCD_KEYBOARD_Y +
		(row + 1) * (C33_LCD_HEIGHT - C33_LCD_KEYBOARD_Y) /
		C33_LCD_KEY_ROWS - 1;
	raw_spin_lock_irqsave(&c33_lcd_lock, flags);
	for (y = y0; y <= y1; y++) {
		volatile u8 *cell = C33_LCD_FB + y * C33_LCD_STRIDE + column * 3;

		cell[0] = pressed && y >= y0 + 2 && y <= y1 - 2 ? 0xff : 0;
		cell[1] = pressed && y >= y0 + 2 && y <= y1 - 2 ? 0xff : 0;
		cell[2] = pressed && y >= y0 + 2 && y <= y1 - 2 ? 0xff : 0;
	}
	if (!pressed)
		c33_lcd_draw_key(row, column, c33_lcd_key_labels[row][column]);
	raw_spin_unlock_irqrestore(&c33_lcd_lock, flags);
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
