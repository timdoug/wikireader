/* A small on-screen keyboard, GPL-3.0-or-later. */

#include <grifo.h>
#include <string.h>

#include "keys.h"

/* Ten columns on a 24-pixel pitch, three rows.  Key i occupies
   [24*i, 24*i + 24) horizontally; the emulator aims at the centre of that,
   12 + 24*i, which is why the pitch has to match rather than merely look
   similar. */
/* The text cursor is addressed in character cells, not pixels -- grifo's
   LCD_AtXY sets TextColumn and TextRow -- and the font is 8x13, so the
   panel is 30 columns by 16 rows. Key rows are two text rows tall and
   start at pixel 130, which puts the emulator's three tap heights (139,
   168, 195) one in each row and lets a label sit on a cell boundary. */
enum {
	KEY_W = 24,		/* pixels: 3 cells */
	KEY_H = 26,		/* pixels: 2 text rows */
	KEY_CELLS = KEY_W / 8,
	COLS = 10,
	ROWS = 3,
	ROW0 = 130,
	ROW0_CELL = ROW0 / 13,
	CELLS_PER_KEY_ROW = KEY_H / 13,
};

/* '<' is backspace and '#' is enter; the emulator's table spells them the
   same way, and the space bar spans columns 4 and 5. */
static const char *const layout[ROWS] = {
	"QWERTYUIOP",
	"ASDFGHJKL<",
	"ZXCV  BNM#",
};

static void key_box(int row, int col, int *x0, int *y0)
{
	*x0 = col * KEY_W;
	*y0 = ROW0 + row * KEY_H;
}

static void frame(int x0, int y0, int w, int h)
{
	lcd_move_to(x0, y0);
	lcd_line_to(x0 + w - 1, y0);
	lcd_line_to(x0 + w - 1, y0 + h - 1);
	lcd_line_to(x0, y0 + h - 1);
	lcd_line_to(x0, y0);
}

static void draw_key(int row, int col, int invert)
{
	char c = layout[row][col];
	int x0, y0, w = KEY_W, i, j;
	const char *label = NULL;
	char one[2];

	if (c == ' ') {
		/* The space bar is drawn once, from its left half. */
		if (col != 4)
			return;
		w = KEY_W * 2;
		label = "";
	}
	key_box(row, col, &x0, &y0);

	if (invert) {
		for (j = y0 + 1; j < y0 + KEY_H - 1; j++)
			for (i = x0 + 1; i < x0 + w - 1; i++)
				lcd_set_pixel(i, j, LCD_BLACK);
	}
	frame(x0, y0, w, KEY_H);
	if (label)
		return;

	if (c == '<')
		label = "BS";
	else if (c == '#')
		label = "GO";
	else {
		one[0] = c;
		one[1] = '\0';
		label = one;
	}
	/* Inverted keys would need white-on-black glyphs, which the text
	   path does not do; the frame and the filled body are feedback
	   enough for a press that lasts a moment. */
	if (!invert) {
		int cells = (int)strlen(label);
		int span = (c == ' ' ? 2 : 1) * KEY_CELLS;

		lcd_at_xy(col * KEY_CELLS + (span - cells) / 2,
			  ROW0_CELL + row * CELLS_PER_KEY_ROW);
		lcd_print(label);
	}
}

void keys_paint(void)
{
	int row, col;

	for (row = 0; row < ROWS; row++)
		for (col = 0; col < COLS; col++)
			draw_key(row, col, 0);
}

char keys_at(int x, int y)
{
	int row = (y - ROW0) / KEY_H;
	int col = x / KEY_W;
	char c;

	if (y < ROW0 || row >= ROWS || col < 0 || col >= COLS)
		return 0;
	c = layout[row][col];
	if (c == ' ')
		return ' ';
	if (c == '<')
		return KEYS_BACKSPACE;
	if (c == '#')
		return KEYS_ENTER;
	return c;
}

void keys_flash(char key)
{
	int row, col;

	for (row = 0; row < ROWS; row++)
		for (col = 0; col < COLS; col++) {
			char c = layout[row][col];

			if ((c == '<' && key == KEYS_BACKSPACE) ||
			    (c == '#' && key == KEYS_ENTER) ||
			    (c == key && c != ' ') ||
			    (c == ' ' && key == ' ' && col == 4)) {
				draw_key(row, col, 1);
				delay_us(60000);
				draw_key(row, col, 0);
				return;
			}
		}
}

/* The text being typed, wrapped over the rows above the keyboard. */
static void show(const char *caption, const char *text)
{
	lcd_clear(LCD_WHITE);
	keys_paint();
	lcd_at_xy(0, 0);
	lcd_print(caption);
	/* Two rows down, leaving rows 2..8 for the prompt. It cannot reach
	   the keyboard at row 10: the buffer is shorter than four rows of
	   thirty columns, and a scroll would take the keyboard with it. */
	lcd_at_xy(0, 2);
	lcd_print(text);
	lcd_print("_");
}

int keys_read_line(char *buffer, size_t size, const char *caption)
{
	size_t len = 0;

	buffer[0] = '\0';
	show(caption, buffer);

	for (;;) {
		event_t e;

		if (event_wait(&e, NULL, NULL) == EVENT_NONE)
			continue;

		if (e.item_type == EVENT_BATTERY_LOW)
			return -1;

		/* The Search button submits too: it is the one the reader's
		   thumb is already on, and it works when the panel is not
		   being touched accurately. */
		if (e.item_type == EVENT_BUTTON_DOWN &&
		    e.button.code == BUTTON_SEARCH)
			break;

		if (e.item_type != EVENT_TOUCH_DOWN)
			continue;

		{
			char c = keys_at(e.touch.x, e.touch.y);

			if (c == 0)
				continue;
			keys_flash(c);
			if (c == KEYS_ENTER)
				break;
			if (c == KEYS_BACKSPACE) {
				if (len)
					buffer[--len] = '\0';
			} else if (len + 1 < size) {
				/* Lower case: the tokenizer's vocabulary is
				   TinyStories text, which is nearly all lower
				   case, and an upper-case run encodes to byte
				   fallbacks and generates nonsense. */
				if (c >= 'A' && c <= 'Z' && len > 0)
					c = (char)(c - 'A' + 'a');
				buffer[len++] = c;
				buffer[len] = '\0';
			}
			show(caption, buffer);
		}
	}
	return (int)len;
}
