// SPDX-License-Identifier: GPL-2.0-only
/*
 * The WikiReader panel as wr-console and wr-keyboard draw it: 240x208 at one
 * bit a pixel, the leftmost pixel in a byte's high bit, black set, and a 5x7
 * font in 6x8 cells.  The text takes the top 120 rows and the keyboard the
 * rest; each program draws into its own copy of the panel and writes only
 * its own rows to /dev/fb0.
 */
#ifndef WR_PANEL_H
#define WR_PANEL_H

#include <stdint.h>
#include <string.h>

#define LCD_WIDTH	240
#define LCD_HEIGHT	208
#define LCD_STRIDE	32
#define LCD_BYTES	(LCD_STRIDE * LCD_HEIGHT)
#define FONT_WIDTH	6
#define FONT_HEIGHT	8
#define KEYBOARD_Y	120

struct glyph {
	char character;
	uint8_t rows[7];
};

static const struct glyph glyphs[] = {
	{ 'A', { 14, 17, 17, 31, 17, 17, 17 } },
	{ 'B', { 30, 17, 17, 30, 17, 17, 30 } },
	{ 'C', { 14, 17, 16, 16, 16, 17, 14 } },
	{ 'D', { 30, 17, 17, 17, 17, 17, 30 } },
	{ 'E', { 31, 16, 16, 30, 16, 16, 31 } },
	{ 'F', { 31, 16, 16, 30, 16, 16, 16 } },
	{ 'G', { 14, 17, 16, 23, 17, 17, 15 } },
	{ 'H', { 17, 17, 17, 31, 17, 17, 17 } },
	{ 'I', { 14, 4, 4, 4, 4, 4, 14 } },
	{ 'J', { 7, 2, 2, 2, 18, 18, 12 } },
	{ 'K', { 17, 18, 20, 24, 20, 18, 17 } },
	{ 'L', { 16, 16, 16, 16, 16, 16, 31 } },
	{ 'M', { 17, 27, 21, 21, 17, 17, 17 } },
	{ 'N', { 17, 25, 21, 19, 17, 17, 17 } },
	{ 'O', { 14, 17, 17, 17, 17, 17, 14 } },
	{ 'P', { 30, 17, 17, 30, 16, 16, 16 } },
	{ 'Q', { 14, 17, 17, 17, 21, 18, 13 } },
	{ 'R', { 30, 17, 17, 30, 20, 18, 17 } },
	{ 'S', { 15, 16, 16, 14, 1, 1, 30 } },
	{ 'T', { 31, 4, 4, 4, 4, 4, 4 } },
	{ 'U', { 17, 17, 17, 17, 17, 17, 14 } },
	{ 'V', { 17, 17, 17, 17, 17, 10, 4 } },
	{ 'W', { 17, 17, 17, 21, 21, 21, 10 } },
	{ 'X', { 17, 17, 10, 4, 10, 17, 17 } },
	{ 'Y', { 17, 17, 10, 4, 4, 4, 4 } },
	{ 'Z', { 31, 1, 2, 4, 8, 16, 31 } },
	{ 'a', { 0, 0, 14, 1, 15, 17, 15 } },
	{ 'b', { 16, 16, 30, 17, 17, 17, 30 } },
	{ 'c', { 0, 0, 14, 16, 16, 17, 14 } },
	{ 'd', { 1, 1, 15, 17, 17, 17, 15 } },
	{ 'e', { 0, 0, 14, 17, 31, 16, 14 } },
	{ 'f', { 6, 9, 8, 28, 8, 8, 8 } },
	{ 'g', { 0, 0, 15, 17, 15, 1, 14 } },
	{ 'h', { 16, 16, 30, 17, 17, 17, 17 } },
	{ 'i', { 4, 0, 12, 4, 4, 4, 14 } },
	{ 'j', { 2, 0, 6, 2, 2, 18, 12 } },
	{ 'k', { 16, 16, 18, 20, 24, 20, 18 } },
	{ 'l', { 12, 4, 4, 4, 4, 4, 14 } },
	{ 'm', { 0, 0, 26, 21, 21, 17, 17 } },
	{ 'n', { 0, 0, 30, 17, 17, 17, 17 } },
	{ 'o', { 0, 0, 14, 17, 17, 17, 14 } },
	{ 'p', { 0, 0, 30, 17, 30, 16, 16 } },
	{ 'q', { 0, 0, 15, 17, 15, 1, 1 } },
	{ 'r', { 0, 0, 22, 25, 16, 16, 16 } },
	{ 's', { 0, 0, 15, 16, 14, 1, 30 } },
	{ 't', { 8, 8, 28, 8, 8, 9, 6 } },
	{ 'u', { 0, 0, 17, 17, 17, 19, 13 } },
	{ 'v', { 0, 0, 17, 17, 17, 10, 4 } },
	{ 'w', { 0, 0, 17, 17, 21, 21, 10 } },
	{ 'x', { 0, 0, 17, 10, 4, 10, 17 } },
	{ 'y', { 0, 0, 17, 17, 15, 1, 14 } },
	{ 'z', { 0, 0, 31, 2, 4, 8, 31 } },
	{ '0', { 14, 17, 19, 21, 25, 17, 14 } },
	{ '1', { 4, 12, 4, 4, 4, 4, 14 } },
	{ '2', { 14, 17, 1, 2, 4, 8, 31 } },
	{ '3', { 30, 1, 1, 14, 1, 1, 30 } },
	{ '4', { 2, 6, 10, 18, 31, 2, 2 } },
	{ '5', { 31, 16, 16, 30, 1, 1, 30 } },
	{ '6', { 14, 16, 16, 30, 17, 17, 14 } },
	{ '7', { 31, 1, 2, 4, 8, 8, 8 } },
	{ '8', { 14, 17, 17, 14, 17, 17, 14 } },
	{ '9', { 14, 17, 17, 15, 1, 1, 14 } },
	{ '#', { 10, 31, 10, 10, 31, 10, 0 } },
	{ '/', { 1, 2, 2, 4, 8, 8, 16 } },
	{ '-', { 0, 0, 0, 31, 0, 0, 0 } },
	{ '_', { 0, 0, 0, 0, 0, 0, 31 } },
	{ '.', { 0, 0, 0, 0, 0, 12, 12 } },
	{ ':', { 0, 12, 12, 0, 12, 12, 0 } },
	{ '>', { 16, 8, 4, 2, 4, 8, 16 } },
	{ '<', { 1, 2, 4, 8, 4, 2, 1 } },
	{ '=', { 0, 0, 31, 0, 31, 0, 0 } },
	{ '+', { 0, 4, 4, 31, 4, 4, 0 } },
	{ '*', { 0, 17, 10, 31, 10, 17, 0 } },
	{ '!', { 4, 4, 4, 4, 4, 0, 4 } },
	{ '?', { 14, 17, 1, 2, 4, 0, 4 } },
	{ '[', { 14, 8, 8, 8, 8, 8, 14 } },
	{ ']', { 14, 2, 2, 2, 2, 2, 14 } },
	{ '(', { 2, 4, 8, 8, 8, 4, 2 } },
	{ ')', { 8, 4, 2, 2, 2, 4, 8 } },
	{ '@', { 14, 17, 23, 21, 23, 16, 14 } },
	{ '$', { 4, 15, 20, 14, 5, 30, 4 } },
	{ '%', { 17, 2, 4, 8, 16, 17, 0 } },
	{ '\'', { 4, 4, 8, 0, 0, 0, 0 } },
	{ '"', { 10, 10, 20, 0, 0, 0, 0 } },
	{ ';', { 0, 12, 12, 0, 12, 4, 8 } },
	{ '&', { 12, 18, 20, 8, 21, 18, 13 } },
	{ '^', { 4, 10, 17, 0, 0, 0, 0 } },
	{ '|', { 4, 4, 4, 4, 4, 4, 4 } },
	{ '\\', { 16, 8, 8, 4, 2, 2, 1 } },
	{ '`', { 8, 4, 0, 0, 0, 0, 0 } },
	{ '{', { 2, 4, 4, 8, 4, 4, 2 } },
	{ '}', { 8, 4, 4, 2, 4, 4, 8 } },
	{ '~', { 0, 0, 9, 22, 0, 0, 0 } },
};

static uint8_t framebuffer[LCD_BYTES];

/* Indexed by character, filled from glyphs[] at startup: searching the list
   for every cell drawn was most of what a redraw cost. */
static const uint8_t *glyph_index[128];

static inline void index_glyphs(void)
{
	static const uint8_t blank[7];
	unsigned int i;

	for (i = 0; i < 128; i++)
		glyph_index[i] = blank;
	for (i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); i++)
		if ((unsigned char)glyphs[i].character < 128)
			glyph_index[(unsigned char)glyphs[i].character] =
				glyphs[i].rows;
}

static inline const uint8_t *glyph_rows(unsigned char character)
{
	return glyph_index[character & 127];
}

static inline void set_pixel(unsigned int x, unsigned int y, int black)
{
	uint8_t mask;

	if (x >= LCD_WIDTH || y >= LCD_HEIGHT)
		return;
	mask = 0x80 >> (x & 7);
	if (black)
		framebuffer[y * LCD_STRIDE + (x >> 3)] |= mask;
	else
		framebuffer[y * LCD_STRIDE + (x >> 3)] &= ~mask;
}

/* Pixels x0..x1 of row y, a byte at a time between the partial ends. */
static inline void fill_span(unsigned int x0, unsigned int x1, unsigned int y,
		      int black)
{
	uint8_t *row = framebuffer + y * LCD_STRIDE;
	unsigned int first = x0 >> 3;
	unsigned int last = x1 >> 3;
	uint8_t head = 0xff >> (x0 & 7);
	uint8_t tail = 0xff << (7 - (x1 & 7));
	unsigned int i;

	if (first == last) {
		head &= tail;
		row[first] = black ? row[first] | head : row[first] & ~head;
		return;
	}
	row[first] = black ? row[first] | head : row[first] & ~head;
	for (i = first + 1; i < last; i++)
		row[i] = black ? 0xff : 0;
	row[last] = black ? row[last] | tail : row[last] & ~tail;
}

static inline void draw_character(unsigned int x, unsigned int y,
			   unsigned char character, int inverse)
{
	const uint8_t *rows = glyph_rows(character);
	unsigned int gy;
	unsigned int offset = x & 7;
	uint16_t mask = 0xfc00U >> offset;

	for (gy = 0; gy < FONT_HEIGHT; gy++) {
		uint8_t *destination = framebuffer + (y + gy) * LCD_STRIDE +
			(x >> 3);
		uint16_t pixels = gy < 7 ?
			((uint16_t)rows[gy] << 11) >> offset : 0;

		if (inverse)
			pixels = ~pixels & mask;
		destination[0] = (destination[0] & ~(mask >> 8)) |
			(pixels >> 8);
		destination[1] = (destination[1] & ~(uint8_t)mask) |
			(uint8_t)pixels;
	}
}

static inline void clear_rows(unsigned int first, unsigned int count)
{
	memset(framebuffer + first * LCD_STRIDE, 0, count * LCD_STRIDE);
}

#endif
