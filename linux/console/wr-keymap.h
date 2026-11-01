// SPDX-License-Identifier: GPL-2.0-only
/*
 * Key codes and the characters they stand for, shared by wr-keyboard, which
 * reports a tapped character as its key, and wr-console, which turns keys
 * back into characters for the shell.
 */
#ifndef WR_KEYMAP_H
#define WR_KEYMAP_H

#include <linux/input.h>

/* The US layout, which is what the soft keyboard's labels are. */
static const struct {
	unsigned short code;
	char plain;
	char shifted;
} keymap[] = {
	{ KEY_1, '1', '!' }, { KEY_2, '2', '@' }, { KEY_3, '3', '#' },
	{ KEY_4, '4', '$' }, { KEY_5, '5', '%' }, { KEY_6, '6', '^' },
	{ KEY_7, '7', '&' }, { KEY_8, '8', '*' }, { KEY_9, '9', '(' },
	{ KEY_0, '0', ')' }, { KEY_MINUS, '-', '_' }, { KEY_EQUAL, '=', '+' },
	{ KEY_Q, 'q', 'Q' }, { KEY_W, 'w', 'W' }, { KEY_E, 'e', 'E' },
	{ KEY_R, 'r', 'R' }, { KEY_T, 't', 'T' }, { KEY_Y, 'y', 'Y' },
	{ KEY_U, 'u', 'U' }, { KEY_I, 'i', 'I' }, { KEY_O, 'o', 'O' },
	{ KEY_P, 'p', 'P' }, { KEY_LEFTBRACE, '[', '{' },
	{ KEY_RIGHTBRACE, ']', '}' }, { KEY_A, 'a', 'A' }, { KEY_S, 's', 'S' },
	{ KEY_D, 'd', 'D' }, { KEY_F, 'f', 'F' }, { KEY_G, 'g', 'G' },
	{ KEY_H, 'h', 'H' }, { KEY_J, 'j', 'J' }, { KEY_K, 'k', 'K' },
	{ KEY_L, 'l', 'L' }, { KEY_SEMICOLON, ';', ':' },
	{ KEY_APOSTROPHE, '\'', '"' }, { KEY_GRAVE, '`', '~' },
	{ KEY_BACKSLASH, '\\', '|' }, { KEY_Z, 'z', 'Z' }, { KEY_X, 'x', 'X' },
	{ KEY_C, 'c', 'C' }, { KEY_V, 'v', 'V' }, { KEY_B, 'b', 'B' },
	{ KEY_N, 'n', 'N' }, { KEY_M, 'm', 'M' }, { KEY_COMMA, ',', '<' },
	{ KEY_DOT, '.', '>' }, { KEY_SLASH, '/', '?' }, { KEY_SPACE, ' ', ' ' },
};

static inline int key_for_character(int character, int *shift)
{
	unsigned int i;

	for (i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++) {
		if (keymap[i].plain == character) {
			*shift = 0;
			return keymap[i].code;
		}
		if (keymap[i].shifted == character) {
			*shift = 1;
			return keymap[i].code;
		}
	}
	return -1;
}

static inline int character_for_key(unsigned int code, int shift)
{
	unsigned int i;

	for (i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++)
		if (keymap[i].code == code)
			return shift ? keymap[i].shifted : keymap[i].plain;
	return -1;
}

/* The front buttons, by the codes the device tree gives them. */
static inline const char *button_name(unsigned int code)
{
	switch (code) {
	case KEY_F1:
		return "random";
	case KEY_SEARCH:
		return "search";
	case KEY_BACK:
		return "history";
	case KEY_POWER:
		return "power";
	default:
		return NULL;
	}
}

#endif
