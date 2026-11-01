// SPDX-License-Identifier: GPL-2.0-only
/*
 * The WikiReader panel as wr-console and wr-keyboard draw it: 240x208 at one
 * bit a pixel, the leftmost pixel in a byte's high bit, black set.  The text,
 * in the kernel's 6x8 console font, takes the top 120 rows, and the keyboard,
 * labelled in X's misc-fixed 6x13, the rest (wr-fonts.h has both).  Each
 * program draws into its own copy of the panel and writes only its own rows
 * to /dev/fb0.
 */
#ifndef WR_PANEL_H
#define WR_PANEL_H

#include <stdint.h>
#include <string.h>

#include "wr-fonts.h"

#define LCD_WIDTH	240
#define LCD_HEIGHT	208
#define LCD_STRIDE	32
#define LCD_BYTES	(LCD_STRIDE * LCD_HEIGHT)
#define FONT_WIDTH	CONSOLE_FONT_WIDTH
#define FONT_HEIGHT	CONSOLE_FONT_HEIGHT
#define KEYBOARD_Y	120

static uint8_t framebuffer[LCD_BYTES];


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

/* A glyph six pixels wide, a byte a row with its pixels in the high bits. */
static inline void draw_glyph(unsigned int x, unsigned int y,
			      const uint8_t *rows, unsigned int height,
			      int inverse)
{
	unsigned int gy;
	unsigned int offset = x & 7;
	uint16_t mask = 0xfc00U >> offset;

	for (gy = 0; gy < height; gy++) {
		uint8_t *destination = framebuffer + (y + gy) * LCD_STRIDE +
			(x >> 3);
		uint16_t pixels = ((uint16_t)(rows[gy] & 0xfc) << 8) >> offset;

		if (inverse)
			pixels = ~pixels & mask;
		destination[0] = (destination[0] & ~(mask >> 8)) |
			(pixels >> 8);
		destination[1] = (destination[1] & ~(uint8_t)mask) |
			(uint8_t)pixels;
	}
}

/* A console character, any of the font's 256. */
static inline void draw_character(unsigned int x, unsigned int y,
				  unsigned char character, int inverse)
{
	draw_glyph(x, y, console_font[character], CONSOLE_FONT_HEIGHT, inverse);
}

/* A key label's character, printable ASCII. */
static inline void draw_label_character(unsigned int x, unsigned int y,
					unsigned char character, int inverse)
{
	static const uint8_t blank[KEY_FONT_HEIGHT];

	draw_glyph(x, y, character >= KEY_FONT_FIRST && character <= KEY_FONT_LAST ?
		   key_font[character - KEY_FONT_FIRST] : blank,
		   KEY_FONT_HEIGHT, inverse);
}

static inline void clear_rows(unsigned int first, unsigned int count)
{
	memset(framebuffer + first * LCD_STRIDE, 0, count * LCD_STRIDE);
}

#endif
