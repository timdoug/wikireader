/* console.c - the guest's terminal on the WikiReader's panel.
 *
 * Geometry, layout and font all follow the terminal the NuttX port already
 * put on this machine,
 * nuttx/overlay/nuttx/boards/c33/s1c33e07/wikireader/src/wikireader_terminal.c,
 * so the two consoles on this device look and behave the same: a 40x13
 * text region 120 pixels tall, four rows of 22-pixel keys under it, and the
 * X11 misc-fixed 6x9 font.  That file is the source of truth; this is a
 * second implementation of the same terminal rather than a second
 * terminal.  Its layout logic -- the rectangles, the hit test, the
 * character and label tables -- is about a hundred and twenty lines of
 * pure arithmetic and worth hoisting into something both can compile.  It
 * is not done here because the NuttX port is Apache-2.0 and deliberately
 * self-contained: its overlay is copied into a fetched tree where samo-lib
 * is not on the include path, which is why it re-declares the Grifo
 * constants it needs too.
 *
 * Drawing goes straight into the frame buffer.  Grifo's lcd_set_pixel is a
 * kernel call per pixel, and the keyboard is twenty thousand of them --
 * redrawing it for a shift took long enough to see.
 *
 * Input arrives from two places and lands in the same queue: a byte on the
 * serial line, which is how the emulator and a host terminal type, and a
 * touch in the keyboard area, which is the only way in on the device.
 */

#include <grifo.h>
#include <string.h>

#include "console.h"
#include "font6x9.h"

enum {
	TERM_HEIGHT = 120,		/* WR_TERM_HEIGHT */
	KEY_HEIGHT = 22,		/* WR_KEY_HEIGHT */
	KEY_ROWS = 4,
	KEY_COLS = 10,
	KEY_WIDTH = LCD_WIDTH / KEY_COLS,
	TERM_COLS = LCD_WIDTH / FONT6X9_WIDTH,
	TERM_ROWS = TERM_HEIGHT / FONT6X9_HEIGHT,
	KEY_COUNT = 37,
	KEY_BACKSPACE = 19,
	KEY_SHIFT = 20,
	KEY_CTRL = 30,
	KEY_MODE = 31,
	STRIDE = LCD_BUFFER_WIDTH_BYTES,
};

/* What each key is currently showing.  A shift changes the twenty-six
   letter faces and nothing else; a control press changes one key.  Keeping
   the drawn appearance and comparing against it is how the NuttX terminal
   avoids repainting the board, and it is the difference between a redraw
   you notice and one you do not. */
struct key_image {
	char label[8];
	unsigned char inverse;
	unsigned char valid;
};
static struct key_image shown[KEY_COUNT];

static int cur_row, cur_col;
static int shifted, symbols, control;
static int escape;   /* 0 normal, 1 saw ESC, 2 inside a CSI sequence */

static const char *const letters[3] = {
	"qwertyuiop", "asdfghjkl", " zxcvbnm.?"
};
static const char *const symbol_rows[2][3] = {
	{"1234567890", "-/:;()$&@", " \\\"'=+*_?!"},
	{"!@#$%^&*()", "[]{}<>|~`", " \\\"'=+_,.:"}
};

/* ---- drawing ------------------------------------------------------------ */

/* A set bit is black (LCD.c: SetPixel), most significant bit leftmost.
   Every key rectangle is byte-aligned -- keys are 24 pixels wide at
   multiples of 24 -- so the common case is a memset per row rather than a
   read-modify-write per pixel.  Pixel at a time this took 26 ms to redraw
   the keyboard, which is visible as a flinch on every shift. */
static void fill(int x, int y, int w, int h, int black)
{
	uint8_t *fb = lcd_get_framebuffer() + y * STRIDE;
	if (((x | w) & 7) == 0) {
		uint8_t value = black ? 0xff : 0x00;
		int bytes = w >> 3;
		fb += x >> 3;
		/* A key is three bytes wide, so a memset call per row costs
		   more in call overhead than the stores it saves. */
		for (int row = 0; row < h; ++row, fb += STRIDE)
			for (int i = 0; i < bytes; ++i)
				fb[i] = value;
		return;
	}
	for (int row = 0; row < h; ++row, fb += STRIDE)
		for (int i = x; i < x + w; ++i) {
			uint8_t bit = (uint8_t)(0x80 >> (i & 7));
			if (black)
				fb[i >> 3] |= bit;
			else
				fb[i >> 3] &= (uint8_t)~bit;
		}
}

/* Six pixels never line up with a byte, so each row of a glyph lands in
   one or two bytes; build it in a 16-bit window and split. */
/* The left edge of a key.  Keys start at multiples of 24, so x is always a
   multiple of eight and the edge is the top bit of one byte. */
static void vline(int x, int y, int h)
{
	uint8_t *fb = lcd_get_framebuffer() + y * STRIDE + (x >> 3);
	uint8_t bit = (uint8_t)(0x80 >> (x & 7));
	for (int r = 0; r < h; ++r, fb += STRIDE)
		*fb |= bit;
}

static void glyph_at(int x, int y, unsigned char ch, int inverse)
{
	if (ch < FONT6X9_FIRST || ch > FONT6X9_LAST)
		ch = '?';
	const unsigned char *g = font6x9[ch - FONT6X9_FIRST];
	uint8_t *fb = lcd_get_framebuffer() + y * STRIDE + (x >> 3);
	int shift = x & 7;
	for (int r = 0; r < FONT6X9_HEIGHT; ++r, fb += STRIDE) {
		/* Inverting the glyph here is what saves a second pass over
		   the label; NuttX keeps a pre-inverted copy for the same
		   reason. */
		uint32_t row = inverse ? (uint32_t)(~g[r] & 0xfc)
				       : (uint32_t)(g[r] & 0xfc);
		uint32_t bits = row << 8;
		uint32_t mask = (uint32_t)0xfc << 8;
		bits >>= shift;
		mask >>= shift;
		fb[0] = (uint8_t)((fb[0] & ~(mask >> 8)) | (bits >> 8));
		fb[1] = (uint8_t)((fb[1] & ~(mask & 0xff)) | (bits & 0xff));
	}
}

static void glyph(int x, int y, unsigned char ch)
{
	glyph_at(x, y, ch, 0);
}

static void text(int x, int y, const char *s, int inverse)
{
	for (; *s; ++s, x += FONT6X9_WIDTH)
		glyph_at(x, y, (unsigned char)*s, inverse);
}

/* ---- input queue -------------------------------------------------------- */

enum { QUEUE = 64 };
static unsigned char queue[QUEUE];
static unsigned queue_head, queue_tail;

static void queue_put(int c)
{
	unsigned next = (queue_tail + 1) % QUEUE;
	if (next != queue_head) {
		queue[queue_tail] = (unsigned char)c;
		queue_tail = next;
	}
}

/* Grifo's serial FIFO is a few bytes deep, and bytes can arrive faster than
   a once-per-batch poll drains it -- the first characters of a typed line
   went missing that way.  Draining on the guest's own reads as well keeps
   it empty. */
static void drain_serial(void)
{
	while (serial_inputavailable())
		queue_put((unsigned char)serial_getchar());
}

int console_get(void)
{
	drain_serial();
	if (queue_head == queue_tail)
		return -1;
	int c = queue[queue_head];
	queue_head = (queue_head + 1) % QUEUE;
	return c;
}

/* ---- keyboard ----------------------------------------------------------- */

/* The bottom row is not ten equal keys: space is three wide and enter two. */
static void key_rect(int key, int *x, int *w, int *row)
{
	int col = key % 10;
	*row = key / 10;
	*w = KEY_WIDTH;
	if (key >= KEY_CTRL) {
		static const uint8_t columns[7] = { 0, 1, 2, 3, 6, 7, 8 };
		col = columns[key - KEY_CTRL];
		*w = key == 33 ? 3 * KEY_WIDTH : key == 36 ? 2 * KEY_WIDTH : KEY_WIDTH;
		*row = 3;
	}
	*x = col * KEY_WIDTH;
}

static int key_at(int x, int y)
{
	if (x < 0 || x >= LCD_WIDTH || y < TERM_HEIGHT || y >= LCD_HEIGHT)
		return -1;
	int col = x / KEY_WIDTH;
	int row = (y - TERM_HEIGHT) / KEY_HEIGHT;
	if (row < 3)
		return row * 10 + col;
	if (col < 3)
		return KEY_CTRL + col;
	return col < 6 ? 33 : col < 8 ? col + 28 : 36;
}

static char key_char(int key)
{
	if (key == KEY_BACKSPACE)
		return '\b';
	if (key == 32)
		return '\t';
	if (key == 33)
		return ' ';
	if (key == 36)
		return '\r';
	if (key == 34 || key == 35)
		return 0;   /* the arrows are escape sequences, sent below */
	if (key >= KEY_CTRL)
		return 0;
	char ch = symbols ? symbol_rows[shifted][key / 10][key % 10]
			  : letters[key / 10][key % 10];
	if (!symbols && shifted) {
		if (ch >= 'a' && ch <= 'z')
			ch -= 'a' - 'A';
		else if (ch == '.')
			ch = '>';
	}
	return ch;
}

static const char *key_label(int key, char *one)
{
	switch (key) {
	/* Three characters is the most a 24-pixel key holds at six pixels
	   each and still leaves its border alone; these are the NuttX
	   terminal's labels. */
	case KEY_BACKSPACE: return "BS";
	case KEY_SHIFT:     return "Sh";
	case KEY_CTRL:      return "Ctl";
	case KEY_MODE:      return symbols ? "ABC" : "123";
	case 32:            return "Tab";
	case 33:            return "Space";
	case 34:            return "<";
	case 35:            return ">";
	case 36:            return "Enter";
	default:
		one[0] = key_char(key);
		one[1] = 0;
		return one;
	}
}

/* Inverting a held key is what tells you the panel registered the press;
   a modifier stays inverted while it is on. */
static void draw_key(int key, int force)
{
	struct key_image *image = &shown[key];
	char one[2];
	const char *label = key_label(key, one);
	int on = (key == KEY_SHIFT && shifted) || (key == KEY_MODE && symbols) ||
		 (key == KEY_CTRL && control);

	if (!force && image->valid && image->inverse == (unsigned char)on &&
	    strcmp(image->label, label) == 0)
		return;

	int x, w, row;
	key_rect(key, &x, &w, &row);
	int y = TERM_HEIGHT + row * KEY_HEIGHT;

	/* Clear the whole key and put the two border edges back, rather than
	   filling the interior: the interior is 23 pixels wide, which is not
	   a whole number of bytes, and the pixel-at-a-time path that forces
	   costs more than repainting the edges ever saves. */
	fill(x, y, w, KEY_HEIGHT, on);
	vline(x, y, KEY_HEIGHT);
	fill(x, y, w, 1, 1);

	/* Centred in the interior, inside the border, the way NuttX insets
	   its rectangle before placing the label. */
	int len = (int)strlen(label);
	if (len) {
		text(x + 1 + (w - 1 - len * FONT6X9_WIDTH) / 2,
		     y + 1 + (KEY_HEIGHT - 1 - FONT6X9_HEIGHT) / 2, label, on);
	}
	strncpy(image->label, label, sizeof image->label - 1);
	image->label[sizeof image->label - 1] = 0;
	image->inverse = (unsigned char)on;
	image->valid = 1;
}

static void draw_keyboard(int force)
{
	for (int key = 0; key < KEY_COUNT; ++key)
		draw_key(key, force);
}

static void keyboard_touch(int x, int y)
{
	int key = key_at(x, y);
	if (key < 0)
		return;
	if (key == KEY_SHIFT || key == KEY_MODE || key == KEY_CTRL) {
		if (key == KEY_SHIFT)
			shifted = !shifted;
		else if (key == KEY_MODE)
			symbols = !symbols;
		else
			control = !control;
		/* Only the faces that actually change get repainted. */
		draw_keyboard(0);
		return;
	}
	if (key == 34 || key == 35) {
		queue_put(27);
		queue_put('[');
		queue_put(key == 34 ? 'D' : 'C');
		return;
	}
	char ch = key_char(key);
	if (!ch)
		return;
	if (control) {
		control = 0;
		ch = (char)(ch & 0x1f);
		draw_key(KEY_CTRL, 0);
	} else if (shifted && !symbols) {
		shifted = 0;   /* shift is one-shot for letters */
		draw_keyboard(0);
	}
	queue_put(ch == '\b' ? 0x7f : ch);
}

/* ---- terminal ----------------------------------------------------------- */

static void scroll(void)
{
	/* Only the text region moves; the keyboard below it stays put. */
	uint8_t *fb = lcd_get_framebuffer();
	size_t line = (size_t)FONT6X9_HEIGHT * STRIDE;
	memmove(fb, fb + line, line * (size_t)(TERM_ROWS - 1));
	memset(fb + line * (size_t)(TERM_ROWS - 1), 0, line);
}

void console_put(int c)
{
	/* Swallow CSI sequences.  Without this, ls's colours arrive as the
	   literal text "[1;34m" and the listing is unreadable.  Nothing here
	   acts on them -- the panel has one colour -- so recognising where
	   they end is the whole job. */
	if (escape == 1) {
		escape = (c == '[' || c == 'O') ? 2 : 0;
		return;
	}
	if (escape == 2) {
		if (c >= 0x40 && c <= 0x7e)
			escape = 0;
		return;
	}
	if (c == 27) {
		escape = 1;
		return;
	}
	if (c == '\r') {
		cur_col = 0;
		return;
	}
	if (c == '\b' || c == 0x7f) {
		if (cur_col > 0)
			--cur_col;
		fill(cur_col * FONT6X9_WIDTH, cur_row * FONT6X9_HEIGHT,
		     FONT6X9_WIDTH, FONT6X9_HEIGHT, 0);
		return;
	}
	if (c != '\n') {
		if (c < 0x20 || c > 0x7e)
			return;
		glyph(cur_col * FONT6X9_WIDTH, cur_row * FONT6X9_HEIGHT,
		      (unsigned char)c);
		if (++cur_col < TERM_COLS)
			return;
	}
	cur_col = 0;
	if (++cur_row >= TERM_ROWS) {
		scroll();
		cur_row = TERM_ROWS - 1;
	}
}

/* ---- events ------------------------------------------------------------- */

void console_poll(void)
{
	/* Raw serial bytes: serial_getchar is the byte as it arrived, where an
	   EVENT_KEY has been through Grifo's escape-sequence decoder.  A
	   terminal wants the bytes. */
	drain_serial();

	event_t e;
	while (event_get(&e) != EVENT_NONE) {
		switch (e.item_type) {
		case EVENT_TOUCH_DOWN:
			keyboard_touch(e.touch.x, e.touch.y);
			break;
		case EVENT_BUTTON_DOWN:
			/* The three panel buttons are the modifiers a
			   terminal needs and the keyboard has no room for. */
			if (e.button.code == BUTTON_SEARCH)
				queue_put(3);		/* ctrl-C */
			else if (e.button.code == BUTTON_HISTORY)
				queue_put(27);		/* escape */
			else if (e.button.code == BUTTON_RANDOM)
				queue_put('\t');
			break;
		case EVENT_BATTERY_LOW:
			power_off();
			break;
		default:
			break;
		}
	}
}

void console_init(void)
{
	cur_row = cur_col = 0;
	lcd_clear(LCD_WHITE);
	draw_keyboard(1);
	event_flush();
}
