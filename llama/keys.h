/* A small on-screen keyboard, GPL-3.0-or-later.
 *
 * The wiki application has a full one in wiki/keyboard.c, but it carries
 * guilib, the glyph renderer, the image tables and the language handling
 * with it -- the whole GUI stack, for an app whose entire binary is
 * fifteen kilobytes.  This draws three rows of letters and reads taps.
 *
 * The geometry is not arbitrary: it matches what the emulator's `-K`
 * already assumes (src/touch.c, `touch_key_pos`) -- ten columns on a
 * 24-pixel pitch centred at x = 12 + 24*i, rows centred at y = 139, 168
 * and 195, QWERTY with backspace at the end of the middle row and the
 * space bar spanning the middle two columns of the bottom.  Matching it
 * means `-K 0,HELLO` types on this keyboard with no emulator change.
 */

#ifndef WR_LLAMA_KEYS_H
#define WR_LLAMA_KEYS_H

#include <stddef.h>

enum {
	KEYS_TOP = 130,		/* first pixel row of the keyboard */
	KEYS_BACKSPACE = '\b',
	KEYS_ENTER = '\n',	/* the '#' key, bottom right */
};

/* Draw the keyboard.  The area above KEYS_TOP is the caller's. */
void keys_paint(void);

/* The character at a touch, or 0 if the tap missed every key. */
char keys_at(int x, int y);

/* Flash a key so a press is visible on a display with no other feedback. */
void keys_flash(char key);

/* Collect a line of text, returning when Enter or the Search button is
   pressed.  `buffer` is shown as it is typed, in the area above the
   keyboard.  Returns the length, or -1 if the machine should power off. */
int keys_read_line(char *buffer, size_t size, const char *caption);

#endif
