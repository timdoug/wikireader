// SPDX-License-Identifier: GPL-2.0-only
/*
 * The soft keyboard's keys, as wr-keyboard draws them at the bottom of the
 * console and wlkeyboard on a Wayland surface: four rows of ten 24-pixel
 * keys below KEYBOARD_Y, letters or (123) symbols, Shift and Ctl latched
 * for one key.  Drawn into wr-panel.h's framebuffer; key codes are
 * <linux/input.h>'s, the US layout of wr-keymap.h.
 */
#ifndef WR_KEYS_H
#define WR_KEYS_H

#include <linux/input.h>
#include <string.h>

#include "wr-keymap.h"
#include "wr-panel.h"

#define KEY_WIDTH	24
#define KEY_ROWS	4

#define WR_KEY_BACKSPACE	19
#define WR_KEY_SHIFT		20
#define WR_KEY_CONTROL		30
#define WR_KEY_SYMBOLS		31
#define WR_KEY_TAB		32
#define WR_KEY_SPACE_FIRST	33
#define WR_KEY_SPACE_LAST	35
#define WR_KEY_LEFT		36
#define WR_KEY_RIGHT		37
#define WR_KEY_ENTER_FIRST	38
#define WR_KEY_ENTER_LAST	39

static const char letter_keys[3][11] = {
	"qwertyuiop",
	"asdfghjkl",
	"zxcvbnm-?",
};

static const char symbol_keys[2][2][11] = {
	{
		"1234567890",
		"-/:;()$&@",
	},
	{
		"!@#$%^&*()",
		"[]{}<>|~`",
	},
};

static const char symbol_third_row[2][10] = {
	".'\"=+*_!\\",
	",_`~=+*^\\",
};

static int active_key = -1;
static int shift_active;
static int control_active;
static int symbols_active;

static int key_character(int key)
{
	int character;

	if (key >= 0 && key < 10) {
		character = symbols_active ? symbol_keys[shift_active][0][key] :
			letter_keys[0][key];
	} else if (key >= 10 && key < WR_KEY_BACKSPACE) {
		character = symbols_active ?
			symbol_keys[shift_active][1][key - 10] :
			letter_keys[1][key - 10];
	} else if (key > WR_KEY_SHIFT && key < WR_KEY_CONTROL) {
		character = symbols_active ?
			symbol_third_row[shift_active][key - WR_KEY_SHIFT - 1] :
			letter_keys[2][key - WR_KEY_SHIFT - 1];
	} else {
		return -1;
	}
	if (!symbols_active && shift_active &&
	    character >= 'a' && character <= 'z')
		character -= 'a' - 'A';
	return character;
}

static const char *key_label(int key, char label[4])
{
	const char *text = NULL;
	int character;

	switch (key) {
	case WR_KEY_BACKSPACE:
		text = "BS";
		break;
	case WR_KEY_SHIFT:
		text = "Sh";
		break;
	case WR_KEY_CONTROL:
		text = "Ctl";
		break;
	case WR_KEY_SYMBOLS:
		text = symbols_active ? "ABC" : "123";
		break;
	case WR_KEY_TAB:
		text = "Tab";
		break;
	case WR_KEY_SPACE_FIRST ... WR_KEY_SPACE_LAST:
		text = "SP";
		break;
	case WR_KEY_LEFT:
		text = "<";
		break;
	case WR_KEY_RIGHT:
		text = ">";
		break;
	case WR_KEY_ENTER_FIRST ... WR_KEY_ENTER_LAST:
		text = "EN";
		break;
	default:
		character = key_character(key);
		label[0] = character >= 0 ? character : ' ';
		label[1] = '\0';
		return label;
	}
	return text;
}

static void draw_key(int key, int pressed)
{
	unsigned int row = key / 10;
	unsigned int column = key % 10;
	unsigned int y0 = KEYBOARD_Y +
		row * (LCD_HEIGHT - KEYBOARD_Y) / KEY_ROWS;
	unsigned int y1 = KEYBOARD_Y +
		(row + 1) * (LCD_HEIGHT - KEYBOARD_Y) / KEY_ROWS - 1;
	unsigned int x0 = column * KEY_WIDTH;
	unsigned int x1 = x0 + KEY_WIDTH - 1;
	char label_buffer[4];
	const char *label = key_label(key, label_buffer);
	unsigned int label_length = strlen(label);
	unsigned int label_x = x0 + (KEY_WIDTH - label_length * KEY_FONT_WIDTH) / 2;
	unsigned int label_y = y0 + (y1 - y0 + 1 - KEY_FONT_HEIGHT) / 2;
	unsigned int x;
	unsigned int y;

	pressed |= (key == WR_KEY_SHIFT && shift_active) ||
		(key == WR_KEY_CONTROL && control_active) ||
		(key == WR_KEY_SYMBOLS && symbols_active);
	for (y = y0; y <= y1; y++) {
		int solid = pressed || y == y0 || y == y1;

		fill_span(x0, x1, y, solid);
		if (!solid) {
			set_pixel(x0, y, 1);
			set_pixel(x1, y, 1);
		}
	}
	for (x = 0; x < label_length; x++)
		draw_label_character(label_x + x * KEY_FONT_WIDTH, label_y, label[x],
			       pressed);
}

static int key_at(unsigned int x, unsigned int y)
{
	unsigned int row;

	if (x >= LCD_WIDTH || y < KEYBOARD_Y || y >= LCD_HEIGHT)
		return -1;
	row = (y - KEYBOARD_Y) * KEY_ROWS / (LCD_HEIGHT - KEYBOARD_Y);
	return row * 10 + x / KEY_WIDTH;
}

/*
 * A released key: change the keyboard's state, or set *code to the key to
 * report (-1 for none) with *shift and *control, its modifiers.  Returns
 * whether the keyboard needs redrawing.
 */
static int tap_key(int key, int *code, int *shift, int *control)
{
	int redraw = 0;

	*code = -1;
	*shift = 0;
	*control = 0;

	if (key == WR_KEY_SHIFT) {
		shift_active = !shift_active;
		return 1;
	}
	if (key == WR_KEY_CONTROL) {
		control_active = !control_active;
		return 1;
	}
	if (key == WR_KEY_SYMBOLS) {
		symbols_active = !symbols_active;
		shift_active = 0;
		return 1;
	}
	if (key == WR_KEY_BACKSPACE) {
		*code = KEY_BACKSPACE;
	} else if (key == WR_KEY_TAB) {
		*code = KEY_TAB;
	} else if (key >= WR_KEY_SPACE_FIRST && key <= WR_KEY_SPACE_LAST) {
		*code = KEY_SPACE;
	} else if (key == WR_KEY_LEFT || key == WR_KEY_RIGHT) {
		*code = key == WR_KEY_LEFT ? KEY_LEFT : KEY_RIGHT;
	} else if (key >= WR_KEY_ENTER_FIRST && key <= WR_KEY_ENTER_LAST) {
		*code = KEY_ENTER;
	} else {
		int character = key_character(key);
		int found = character < 0 ? -1 :
			key_for_character(character, shift);

		if (found < 0)
			return 0;
		*code = found;
		*control = control_active;
	}
	if (shift_active || control_active) {
		shift_active = 0;
		control_active = 0;
		redraw = 1;
	}
	return redraw;
}

#endif
