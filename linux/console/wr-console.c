// SPDX-License-Identifier: GPL-2.0-only
/* WikiReader userspace framebuffer terminal and touchscreen keyboard. */
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <time.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define LCD_WIDTH	240
#define LCD_HEIGHT	208
#define LCD_STRIDE	32
#define LCD_BYTES	(LCD_STRIDE * LCD_HEIGHT)
#define TEXT_COLUMNS	40
#define TEXT_ROWS	14
#define FONT_WIDTH	6
#define FONT_HEIGHT	8
#define STATUS_Y	112
#define KEYBOARD_Y	120
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

#define OUTPUT_BATCH_BYTES	256
#define OUTPUT_QUIET_MS		8
#define SCROLL_FRAME_MS		16

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

static uint8_t framebuffer[LCD_BYTES];
static unsigned char cells[TEXT_ROWS][TEXT_COLUMNS];
static unsigned int cursor_x;
static unsigned int cursor_y;
static unsigned int escape_state;
static unsigned int escape_parameter;
static unsigned int dirty_text_first = TEXT_ROWS;
static unsigned int dirty_text_last;
static int active_key = -1;
static int shift_active;
static int control_active;
static int symbols_active;
static int fb_fd;
static int log_fd;
/*
 * Key delivery.  The soft keyboard is a keyboard as far as Linux is
 * concerned: it is a uinput device, and tapping a key here reports a press
 * and release on it.  Everything that arrives on any keyboard-shaped evdev
 * node -- that device, the front buttons, anything added later -- is turned
 * into bytes for the shell's PTY below.  Keys are therefore visible to any
 * program that reads evdev, not only to this one, and this program is the
 * only keyboard driver a machine without a VT has.
 */
#define MAX_KEYBOARDS		4
#define SOFT_KEYBOARD_NAME	"WikiReader soft keyboard"
static int uinput_fd = -1;
static int keyboard_fds[MAX_KEYBOARDS];
static unsigned int keyboard_count;
static int shift_down;
static int control_down;
static int keyboard_reported;
static int button_reported;
static void set_display_blank(int blank);
static void suspend_until_touch(void);
static unsigned int scrolls_pending;
/*
 * Pacing keeps a burst of output readable, but it must never sit between a
 * key and the first thing that key produced: that delay is the latency the
 * user feels.  The first frame after input is painted immediately and only
 * the continuation of the same burst is paced.
 */
static int burst_painted;
/*
 * There is no VT here, so nothing else would ever turn the panel off on a
 * device that runs from two AA cells. The timeout comes from the kernel
 * command line, which the launcher takes from a line on the card:
 * wr.blank=<seconds>, and wr.blank=0 to keep the panel on.
 */
#define BLANK_SECONDS_DEFAULT	120
static int blank_seconds = BLANK_SECONDS_DEFAULT;
static int suspend_seconds;
static int power_logging;
static int display_blanked;
static long idle_since;

static void log_text(const char *text)
{
	size_t length = strlen(text);

	while (length) {
		ssize_t written = write(log_fd, text, length);

		if (written <= 0)
			return;
		text += written;
		length -= written;
	}
}

static const uint8_t *glyph_rows(unsigned char character)
{
	static const uint8_t blank[7];
	unsigned int i;

	for (i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); i++)
		if ((unsigned char)glyphs[i].character == character)
			return glyphs[i].rows;
	return blank;
}

static void set_pixel(unsigned int x, unsigned int y, int black)
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

static void draw_character(unsigned int x, unsigned int y,
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

static void clear_rows(unsigned int first, unsigned int count)
{
	memset(framebuffer + first * LCD_STRIDE, 0, count * LCD_STRIDE);
}

static void write_rows(unsigned int first, unsigned int count)
{
	size_t offset = first * LCD_STRIDE;
	size_t length = count * LCD_STRIDE;
	size_t done = 0;

	if (!count || first >= LCD_HEIGHT)
		return;
	if (count > LCD_HEIGHT - first)
		length = (LCD_HEIGHT - first) * LCD_STRIDE;
	if (lseek(fb_fd, offset, SEEK_SET) < 0)
		return;
	while (done < length) {
		ssize_t written = write(fb_fd, framebuffer + offset + done,
					length - done);

		if (written <= 0)
			return;
		done += written;
	}
}

static void mark_text_row(unsigned int row)
{
	if (row >= TEXT_ROWS)
		return;
	if (row < dirty_text_first)
		dirty_text_first = row;
	if (row > dirty_text_last)
		dirty_text_last = row;
}

static void mark_all_text(void)
{
	dirty_text_first = 0;
	dirty_text_last = TEXT_ROWS - 1;
}

static void draw_checkpoint(unsigned int stage)
{
	unsigned int x = stage * 16;
	unsigned int y;

	if (x + 8 > LCD_WIDTH)
		return;
	for (y = STATUS_Y; y < STATUS_Y + 6; y++)
		framebuffer[y * LCD_STRIDE + (x >> 3)] = 0xff;
}

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
	unsigned int label_x = x0 + (KEY_WIDTH - label_length * FONT_WIDTH) / 2;
	unsigned int label_y = y0 + (y1 - y0 + 1 - FONT_HEIGHT) / 2;
	unsigned int x;
	unsigned int y;

	pressed |= (key == WR_KEY_SHIFT && shift_active) ||
		(key == WR_KEY_CONTROL && control_active) ||
		(key == WR_KEY_SYMBOLS && symbols_active);
	for (y = y0; y <= y1; y++)
		for (x = x0; x <= x1; x++)
			set_pixel(x, y, pressed || x == x0 || x == x1 ||
				  y == y0 || y == y1);
	for (x = 0; x < label_length; x++)
		draw_character(label_x + x * FONT_WIDTH, label_y, label[x],
			       pressed);
}

static void draw_text_rows(unsigned int first, unsigned int last)
{
	unsigned int row;
	unsigned int column;

	clear_rows(first * FONT_HEIGHT, (last - first + 1) * FONT_HEIGHT);
	for (row = first; row <= last; row++)
		for (column = 0; column < TEXT_COLUMNS; column++)
			draw_character(column * FONT_WIDTH, row * FONT_HEIGHT,
				       cells[row][column], 0);
}

static void flush_text(void)
{
	unsigned int first = dirty_text_first;
	unsigned int last = dirty_text_last;

	if (first >= TEXT_ROWS)
		return;
	write_rows(first * FONT_HEIGHT, (last - first + 1) * FONT_HEIGHT);
	dirty_text_first = TEXT_ROWS;
	dirty_text_last = 0;
	if (scrolls_pending) {
		if (burst_painted)
			poll(NULL, 0, SCROLL_FRAME_MS);
		scrolls_pending = 0;
	}
	burst_painted = 1;
}

static void flush_key(int key)
{
	unsigned int row;
	unsigned int first;
	unsigned int last;

	if (key < 0 || key >= KEY_ROWS * 10)
		return;
	row = key / 10;
	first = KEYBOARD_Y + row * (LCD_HEIGHT - KEYBOARD_Y) / KEY_ROWS;
	last = KEYBOARD_Y +
		(row + 1) * (LCD_HEIGHT - KEYBOARD_Y) / KEY_ROWS - 1;
	draw_key(key, key == active_key);
	write_rows(first, last - first + 1);
}

static void flush_keyboard(void)
{
	int key;

	clear_rows(KEYBOARD_Y, LCD_HEIGHT - KEYBOARD_Y);
	for (key = 0; key < KEY_ROWS * 10; key++)
		draw_key(key, key == active_key);
	write_rows(KEYBOARD_Y, LCD_HEIGHT - KEYBOARD_Y);
}

static void flush_display(void)
{
	int key;

	draw_text_rows(0, TEXT_ROWS - 1);
	clear_rows(KEYBOARD_Y, LCD_HEIGHT - KEYBOARD_Y);
	for (key = 0; key < KEY_ROWS * 10; key++)
		draw_key(key, key == active_key);
	write_rows(0, LCD_HEIGHT);
	dirty_text_first = TEXT_ROWS;
	dirty_text_last = 0;
}

static void scroll_terminal(void)
{
	memmove(cells[0], cells[1], (TEXT_ROWS - 1) * TEXT_COLUMNS);
	memset(cells[TEXT_ROWS - 1], ' ', TEXT_COLUMNS);
	memmove(framebuffer, framebuffer + FONT_HEIGHT * LCD_STRIDE,
		(STATUS_Y - FONT_HEIGHT) * LCD_STRIDE);
	clear_rows(STATUS_Y - FONT_HEIGHT, FONT_HEIGHT);
	cursor_y = TEXT_ROWS - 1;
	mark_all_text();
	scrolls_pending++;
}

static void terminal_newline(void)
{
	cursor_x = 0;
	if (++cursor_y == TEXT_ROWS)
		scroll_terminal();
}

static void terminal_erase_line(unsigned int first, unsigned int last)
{
	if (first >= TEXT_COLUMNS)
		return;
	if (last >= TEXT_COLUMNS)
		last = TEXT_COLUMNS - 1;
	if (last < first)
		return;
	memset(&cells[cursor_y][first], ' ', last - first + 1);
	draw_text_rows(cursor_y, cursor_y);
	mark_text_row(cursor_y);
}

static void terminal_erase_display(unsigned int mode)
{
	unsigned int row;

	if (mode == 0) {
		terminal_erase_line(cursor_x, TEXT_COLUMNS - 1);
		for (row = cursor_y + 1; row < TEXT_ROWS; row++) {
			memset(cells[row], ' ', TEXT_COLUMNS);
			mark_text_row(row);
		}
	} else if (mode == 1) {
		for (row = 0; row < cursor_y; row++) {
			memset(cells[row], ' ', TEXT_COLUMNS);
			mark_text_row(row);
		}
		terminal_erase_line(0, cursor_x);
	} else if (mode == 2 || mode == 3) {
		memset(cells, ' ', sizeof(cells));
		clear_rows(0, STATUS_Y);
		mark_all_text();
	}
}

static void terminal_byte(unsigned char byte)
{
	if (escape_state == 1) {
		escape_state = byte == '[' ? 2 : 0;
		escape_parameter = 0;
		return;
	}
	if (escape_state == 2) {
		if (byte >= '0' && byte <= '9') {
			escape_parameter = escape_parameter * 10 + byte - '0';
			return;
		}
		if (byte == ';')
			return;
		if (byte == 'J') {
			terminal_erase_display(escape_parameter);
		} else if (byte == 'K') {
			if (escape_parameter == 0)
				terminal_erase_line(cursor_x, TEXT_COLUMNS - 1);
			else if (escape_parameter == 1)
				terminal_erase_line(0, cursor_x);
			else if (escape_parameter == 2)
				terminal_erase_line(0, TEXT_COLUMNS - 1);
		} else if (byte == 'H' || byte == 'f') {
			cursor_x = 0;
			cursor_y = 0;
		}
		escape_state = 0;
		return;
	}
	if (byte == 0x1b) {
		escape_state = 1;
		return;
	}
	if (byte == '\r') {
		cursor_x = 0;
		return;
	}
	if (byte == '\n') {
		terminal_newline();
		return;
	}
	if (byte == '\b' || byte == 0x7f) {
		if (cursor_x)
			cursor_x--;
		return;
	}
	if (byte == '\t') {
		do {
			terminal_byte(' ');
		} while (cursor_x & 3);
		return;
	}
	if (byte < 32)
		return;
	mark_text_row(cursor_y);
	cells[cursor_y][cursor_x] = byte;
	draw_character(cursor_x * FONT_WIDTH, cursor_y * FONT_HEIGHT, byte, 0);
	if (++cursor_x == TEXT_COLUMNS)
		terminal_newline();
}

static void consume_terminal_output(int master_fd)
{
	unsigned char output[OUTPUT_BATCH_BYTES];
	struct pollfd more = {
		.fd = master_fd,
		.events = POLLIN,
	};
	size_t total = 0;

	for (;;) {
		ssize_t count = read(master_fd, output, sizeof(output));
		ssize_t i;

		if (count <= 0)
			break;
		write(log_fd, output, count);
		for (i = 0; i < count; i++)
			terminal_byte(output[i]);
		total += count;
		if (total >= OUTPUT_BATCH_BYTES || !burst_painted ||
		    poll(&more, 1, OUTPUT_QUIET_MS) <= 0 ||
		    !(more.revents & POLLIN))
			break;
	}
	flush_text();
}

static int key_at(unsigned int x, unsigned int y)
{
	unsigned int row;

	if (x >= LCD_WIDTH || y < KEYBOARD_Y || y >= LCD_HEIGHT)
		return -1;
	row = (y - KEYBOARD_Y) * KEY_ROWS / (LCD_HEIGHT - KEYBOARD_Y);
	return row * 10 + x / KEY_WIDTH;
}

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

/* Keys the soft keyboard reports that carry no character of their own. */
static const unsigned short special_keys[] = {
	KEY_ENTER, KEY_BACKSPACE, KEY_TAB, KEY_ESC, KEY_LEFT, KEY_RIGHT,
	KEY_UP, KEY_DOWN, KEY_LEFTSHIFT, KEY_LEFTCTRL,
};

static int key_for_character(int character, int *shift)
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

static int character_for_key(unsigned int code, int shift)
{
	unsigned int i;

	for (i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++)
		if (keymap[i].code == code)
			return shift ? keymap[i].shifted : keymap[i].plain;
	return -1;
}

/*
 * What a key press means to the shell.  The front buttons have no terminal
 * meaning of their own, so they get the three things a terminal on a device
 * with no other keys most wants: an interrupt, completion, and the last
 * command back.
 */
static size_t key_bytes(unsigned int code, int shift, int control,
			unsigned char out[4])
{
	int character;

	switch (code) {
	case KEY_ENTER:
	case KEY_KPENTER:
		out[0] = '\r';
		return 1;
	case KEY_BACKSPACE:
		out[0] = 0x7f;
		return 1;
	case KEY_TAB:
	case KEY_SEARCH:
		out[0] = '\t';
		return 1;
	case KEY_ESC:
		out[0] = 0x1b;
		return 1;
	case KEY_F1:
		out[0] = 0x03;			/* random: interrupt */
		return 1;
	case KEY_LEFT:
	case KEY_RIGHT:
	case KEY_UP:
	case KEY_DOWN:
	case KEY_BACK:
		out[0] = 0x1b;
		out[1] = '[';
		out[2] = code == KEY_LEFT ? 'D' : code == KEY_RIGHT ? 'C' :
			 code == KEY_DOWN ? 'B' : 'A';
		return 3;
	default:
		character = character_for_key(code, shift);
		if (character < 0)
			return 0;
		if (control &&
		    (character == ' ' ||
		     (character >= '@' && character <= '_') ||
		     (character >= 'a' && character <= 'z')))
			character &= 0x1f;
		out[0] = (unsigned char)character;
		return 1;
	}
}

static int deliver_key(int master_fd, unsigned int code, int shift,
		       int control)
{
	unsigned char output[4];
	size_t length = key_bytes(code, shift, control, output);

	if (!length)
		return 0;
	burst_painted = 0;
	return write(master_fd, output, length) == (ssize_t)length;
}

static void add_event(struct input_event *events, unsigned int *count,
		      unsigned int type, unsigned int code, int value)
{
	memset(&events[*count], 0, sizeof(events[0]));
	events[*count].type = type;
	events[*count].code = code;
	events[*count].value = value;
	(*count)++;
}

/* Press and release one key on the soft keyboard, with its modifiers. */
static int emit_key(unsigned int code, int shift, int control)
{
	struct input_event events[10];
	unsigned int count = 0;
	ssize_t length;

	if (shift)
		add_event(events, &count, EV_KEY, KEY_LEFTSHIFT, 1);
	if (control)
		add_event(events, &count, EV_KEY, KEY_LEFTCTRL, 1);
	add_event(events, &count, EV_KEY, code, 1);
	add_event(events, &count, EV_SYN, SYN_REPORT, 0);
	add_event(events, &count, EV_KEY, code, 0);
	if (control)
		add_event(events, &count, EV_KEY, KEY_LEFTCTRL, 0);
	if (shift)
		add_event(events, &count, EV_KEY, KEY_LEFTSHIFT, 0);
	add_event(events, &count, EV_SYN, SYN_REPORT, 0);
	length = (ssize_t)(count * sizeof(events[0]));
	return write(uinput_fd, events, count * sizeof(events[0])) == length;
}

static int create_soft_keyboard(void)
{
	struct uinput_setup setup;
	unsigned int i;
	int fd;

	fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (fd < 0)
		return -1;
	if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0)
		goto fail;
	for (i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++)
		if (ioctl(fd, UI_SET_KEYBIT, keymap[i].code) < 0)
			goto fail;
	for (i = 0; i < sizeof(special_keys) / sizeof(special_keys[0]); i++)
		if (ioctl(fd, UI_SET_KEYBIT, special_keys[i]) < 0)
			goto fail;
	memset(&setup, 0, sizeof(setup));
	setup.id.bustype = BUS_VIRTUAL;
	strncpy(setup.name, SOFT_KEYBOARD_NAME, sizeof(setup.name) - 1);
	if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0)
		goto fail;
	return fd;
fail:
	close(fd);
	return -1;
}

/*
 * Sort the event nodes: the one with absolute axes is the touchscreen,
 * anything else with keys is a keyboard, including the soft one just made.
 */
static void open_input_devices(int *touch_fd)
{
	unsigned int n;

	*touch_fd = -1;
	keyboard_count = 0;
	for (n = 0; n < 16; n++) {
		unsigned long evbits = 0;
		char path[32];
		int fd;

		snprintf(path, sizeof(path), "/dev/input/event%u", n);
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), &evbits) < 0) {
			close(fd);
			continue;
		}
		if (evbits & (1ul << EV_ABS)) {
			if (*touch_fd < 0)
				*touch_fd = fd;
			else
				close(fd);
		} else if ((evbits & (1ul << EV_KEY)) &&
			   keyboard_count < MAX_KEYBOARDS) {
			keyboard_fds[keyboard_count++] = fd;
		} else {
			close(fd);
		}
	}
}

static const char *button_name(unsigned int code)
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

/* One event from a keyboard device.  Returns 1 if the shell got bytes. */
static int keyboard_event(int master_fd, const struct input_event *event)
{
	const char *button;
	char line[64];

	if (event->type != EV_KEY)
		return 0;
	switch (event->code) {
	case KEY_LEFTSHIFT:
	case KEY_RIGHTSHIFT:
		shift_down = event->value != 0;
		return 0;
	case KEY_LEFTCTRL:
	case KEY_RIGHTCTRL:
		control_down = event->value != 0;
		return 0;
	default:
		break;
	}
	if (!event->value)
		return 0;
	button = button_name(event->code);
	if (button && !button_reported) {
		snprintf(line, sizeof(line), "C33 input: front button %s\n",
			 button);
		log_text(line);
		button_reported = 1;
	}
	if (event->code == KEY_POWER) {
		/* The switch cannot cut power here; it is the sleep button. */
		if (suspend_seconds) {
			log_text("C33 input: power switch suspends until touch\n");
			set_display_blank(1);
			suspend_until_touch();
		} else {
			log_text("C33 input: power switch blanks the panel\n");
			set_display_blank(1);
		}
		return 0;
	}
	if (!deliver_key(master_fd, event->code, shift_down, control_down))
		return 0;
	if (!keyboard_reported) {
		log_text("C33 input: keyboards feed the PTY through evdev\n");
		keyboard_reported = 1;
	}
	return 1;
}

static int send_key(int master_fd, int key, int *redraw_keyboard)
{
	unsigned int code;
	int shift = 0;
	int control = 0;
	int character;

	*redraw_keyboard = 0;
	if (key == WR_KEY_SHIFT) {
		shift_active = !shift_active;
		*redraw_keyboard = 1;
		return 0;
	}
	if (key == WR_KEY_CONTROL) {
		control_active = !control_active;
		*redraw_keyboard = 1;
		return 0;
	}
	if (key == WR_KEY_SYMBOLS) {
		symbols_active = !symbols_active;
		shift_active = 0;
		*redraw_keyboard = 1;
		return 0;
	}
	if (key == WR_KEY_BACKSPACE) {
		code = KEY_BACKSPACE;
	} else if (key == WR_KEY_TAB) {
		code = KEY_TAB;
	} else if (key >= WR_KEY_SPACE_FIRST && key <= WR_KEY_SPACE_LAST) {
		code = KEY_SPACE;
	} else if (key == WR_KEY_LEFT || key == WR_KEY_RIGHT) {
		code = key == WR_KEY_LEFT ? KEY_LEFT : KEY_RIGHT;
	} else if (key >= WR_KEY_ENTER_FIRST && key <= WR_KEY_ENTER_LAST) {
		code = KEY_ENTER;
	} else {
		int found;

		character = key_character(key);
		if (character < 0)
			return 0;
		found = key_for_character(character, &shift);
		if (found < 0)
			return 0;
		code = (unsigned int)found;
		control = control_active;
	}
	if (shift_active || control_active) {
		shift_active = 0;
		control_active = 0;
		*redraw_keyboard = 1;
	}
	/* Without uinput the key goes straight to the shell, as it used to. */
	if (uinput_fd < 0)
		return deliver_key(master_fd, code, shift, control);
	burst_painted = 0;
	return emit_key(code, shift, control);
}

static int open_pty(int *slave_fd)
{
	char path[] = "/dev/pts/0000000000";
	char digits[10];
	unsigned int number;
	unsigned int count = 0;
	unsigned int i;
	struct winsize size = {
		.ws_row = TEXT_ROWS,
		.ws_col = TEXT_COLUMNS,
		.ws_xpixel = LCD_WIDTH,
		.ws_ypixel = STATUS_Y,
	};
	int master;
	int unlock = 0;

	master = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (master < 0)
		return -1;
	if (ioctl(master, TIOCSPTLCK, &unlock) < 0 ||
	    ioctl(master, TIOCGPTN, &number) < 0) {
		close(master);
		return -1;
	}
	do {
		digits[count++] = '0' + number % 10;
		number /= 10;
	} while (number && count < sizeof(digits));
	for (i = 0; i < count; i++)
		path[9 + i] = digits[count - i - 1];
	path[9 + count] = '\0';
	*slave_fd = open(path, O_RDWR | O_NOCTTY);
	if (*slave_fd < 0) {
		close(master);
		return -1;
	}
	ioctl(*slave_fd, TIOCSWINSZ, &size);
	return master;
}

static pid_t start_shell(int master, int slave)
{
	char *const argv[] = { "sh", "-i", NULL };
	char *const envp[] = {
		"HOME=/", "PATH=/bin:/sbin", "TERM=linux", NULL,
	};
	pid_t child = vfork();

	if (child == 0) {
		if (setsid() < 0 || ioctl(slave, TIOCSCTTY, 0) < 0 ||
		    dup2(slave, STDIN_FILENO) < 0 ||
		    dup2(slave, STDOUT_FILENO) < 0 ||
		    dup2(slave, STDERR_FILENO) < 0)
			_exit(126);
		close(master);
		if (slave > STDERR_FILENO)
			close(slave);
		execve("/bin/sh", argv, envp);
		_exit(127);
	}
	return child;
}

static long monotonic_seconds(void)
{
	struct timespec now;
	int ret = clock_gettime(CLOCK_MONOTONIC, &now);

	if (ret < 0)
		return 0;
	return now.tv_sec;
}

static void read_timeouts(void)
{
	char cmdline[512];
	const char *found;
	int fd = open("/proc/cmdline", O_RDONLY);
	ssize_t count;

	if (fd < 0)
		return;
	count = read(fd, cmdline, sizeof(cmdline) - 1);
	close(fd);
	if (count <= 0)
		return;
	cmdline[count] = '\0';
	found = strstr(cmdline, "wr.blank=");
	if (found)
		blank_seconds = atoi(found + strlen("wr.blank="));
	found = strstr(cmdline, "wr.suspend=");
	if (found)
		suspend_seconds = atoi(found + strlen("wr.suspend="));
	power_logging = strstr(cmdline, "wr.pmlog") != NULL;
	if (blank_seconds < 0)
		blank_seconds = 0;
	if (suspend_seconds < 0)
		suspend_seconds = 0;
}

/*
 * Suspend is opt-in from the same command line, because a machine that
 * suspends without a working wake source needs its batteries pulled. The
 * write returns when the kernel has resumed; the touch that caused the wake
 * arrives afterwards and unblanks the panel like any other.
 */
/*
 * A device with no serial cannot say why it failed to wake, so each attempt
 * leaves a line on the card: how long it slept, and how the touch interrupt
 * count moved across it. A file that ends in "suspending" is a machine that
 * never came back.
 */
static void set_display_blank(int blank);

static void power_note(const char *what, long seconds)
{
	char text[2048];
	char line[96];
	int fd;
	int length;
	int source;

	/* Writing a card is slow enough to be felt, so it is opt-in. */
	if (!power_logging)
		return;
	fd = open("/mnt/sd/linuxpm.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0)
		return;
	length = snprintf(line, sizeof(line), "%s at %ld s\n", what, seconds);
	if (length > 0)
		write(fd, line, length);
	/*
	 * Which interrupt moved is the only evidence there is for what woke
	 * the machine, so keep the whole table rather than one parsed number.
	 */
	source = open("/proc/interrupts", O_RDONLY);
	if (source >= 0) {
		ssize_t count = read(source, text, sizeof(text));

		if (count > 0)
			write(fd, text, count);
		close(source);
	}
	close(fd);
	sync();
}

static void suspend_until_touch(void)
{
	long before = monotonic_seconds();
	int fd = open("/sys/power/state", O_WRONLY);

	if (fd < 0) {
		log_text("C33 power: no suspend support\n");
		suspend_seconds = 0;
		return;
	}
	log_text("C33 power: suspending until touch\n");
	power_note("suspending", before);
	if (write(fd, "freeze\n", 7) < 0) {
		log_text("C33 power: suspend REFUSED\n");
		suspend_seconds = 0;
	} else {
		log_text("C33 power: resumed\n");
	}
	close(fd);
	/*
	 * The touch that woke the machine was spent on waking it: the kernel
	 * takes that interrupt as its wake event, so no key arrives to turn
	 * the panel back on. Light it before anything slower happens, or the
	 * wait for the screen is whatever else this function does.
	 */
	set_display_blank(0);
	power_note("resumed", monotonic_seconds() - before);
}

static void set_display_blank(int blank)
{
	if (blank == display_blanked)
		return;
	if (ioctl(fb_fd, FBIOBLANK,
		  blank ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK) < 0)
		return;
	display_blanked = blank;
	log_text(blank ? "C33 display: blanked while idle\n"
			: "C33 display: woken by touch\n");
}

int main(void)
{
	struct fb_var_screeninfo variable;
	struct fb_fix_screeninfo fixed;
	struct pollfd poll_fds[2 + MAX_KEYBOARDS];
	struct input_event events[8];
	unsigned int touch_x = 0;
	unsigned int touch_y = 0;
	unsigned int poll_count;
	unsigned int k;
	int touch_reported = 0;
	int slave_fd;
	int input_fd;
	int master_fd;
	pid_t shell;

	log_fd = open("/dev/ttyC0", O_WRONLY | O_NONBLOCK);
	if (log_fd < 0)
		log_fd = STDERR_FILENO;
	fb_fd = open("/dev/fb0", O_RDWR);
	uinput_fd = create_soft_keyboard();
	open_input_devices(&input_fd);
	if (fb_fd < 0 || input_fd < 0 ||
	    ioctl(fb_fd, FBIOGET_VSCREENINFO, &variable) < 0 ||
	    ioctl(fb_fd, FBIOGET_FSCREENINFO, &fixed) < 0 ||
	    variable.xres != LCD_WIDTH || variable.yres != LCD_HEIGHT ||
	    variable.bits_per_pixel != 1 || fixed.line_length != LCD_STRIDE) {
		log_text("C33 userspace console: device setup FAILED\n");
		return 1;
	}
	if (read(fb_fd, framebuffer, sizeof(framebuffer)) !=
	    sizeof(framebuffer))
		memset(framebuffer, 0, sizeof(framebuffer));
	memset(cells, ' ', sizeof(cells));
	for (const char *banner = "C33 USERSPACE CONSOLE\n"; *banner; banner++)
		terminal_byte(*banner);
	clear_rows(STATUS_Y, KEYBOARD_Y - STATUS_Y);
	for (unsigned int stage = 0; stage < 4; stage++)
		draw_checkpoint(stage);
	draw_checkpoint(4);

	master_fd = open_pty(&slave_fd);
	if (master_fd < 0) {
		log_text("C33 userspace console: PTY allocation FAILED\n");
		return 1;
	}
	draw_checkpoint(5);
	shell = start_shell(master_fd, slave_fd);
	close(slave_fd);
	if (shell < 0) {
		log_text("C33 userspace console: shell start FAILED\n");
		return 1;
	}
	draw_checkpoint(6);
	flush_display();
	log_text("C33 userspace console: fbdev + evdev + PTY shell ready\n");
	if (uinput_fd >= 0)
		log_text("C33 input: soft keyboard registered as a uinput device\n");
	else
		log_text("C33 input: no uinput, soft keys go straight to the PTY\n");

	poll_fds[0].fd = master_fd;
	poll_fds[0].events = POLLIN;
	poll_fds[1].fd = input_fd;
	poll_fds[1].events = POLLIN;
	for (k = 0; k < keyboard_count; k++) {
		poll_fds[2 + k].fd = keyboard_fds[k];
		poll_fds[2 + k].events = POLLIN;
	}
	poll_count = 2 + keyboard_count;
	read_timeouts();
	idle_since = monotonic_seconds();
	for (;;) {
		long idle = monotonic_seconds() - idle_since;
		int wait = -1;

		if (blank_seconds && !display_blanked && idle >= blank_seconds)
			set_display_blank(1);
		if (suspend_seconds && idle >= suspend_seconds) {
			set_display_blank(1);
			suspend_until_touch();
			idle_since = monotonic_seconds();
			idle = 0;
		}
		if (blank_seconds && !display_blanked)
			wait = (int)(blank_seconds - idle) * 1000;
		if (suspend_seconds) {
			int until = (int)(suspend_seconds - idle) * 1000;

			if (wait < 0 || until < wait)
				wait = until;
		}
		if (wait < 0)
			wait = -1;
		else if (wait < 1)
			wait = 1;
		if (poll(poll_fds, poll_count, wait) < 0)
			continue;
		for (k = 0; k < keyboard_count; k++) {
			ssize_t count;
			unsigned int i;
			int was_blanked = display_blanked;

			if (!(poll_fds[2 + k].revents & POLLIN))
				continue;
			count = read(keyboard_fds[k], events, sizeof(events));
			if (count <= 0)
				continue;
			/*
			 * Only a press counts as activity: the release of the
			 * key that blanked or suspended the machine arrives
			 * afterwards and must not undo it.
			 */
			for (i = 0; i < count / sizeof(events[0]); i++)
				if (events[i].type == EV_KEY && events[i].value)
					break;
			if (i == count / sizeof(events[0]))
				continue;
			idle_since = monotonic_seconds();
			set_display_blank(0);
			/* A key that wakes the panel is spent on waking it. */
			if (was_blanked)
				continue;
			for (i = 0; i < count / sizeof(events[0]); i++)
				keyboard_event(master_fd, &events[i]);
		}
		if (poll_fds[0].revents & POLLIN) {
			consume_terminal_output(master_fd);
			idle_since = monotonic_seconds();
			set_display_blank(0);
		}
		if (poll_fds[1].revents & POLLIN) {
			ssize_t count = read(input_fd, events, sizeof(events));
			unsigned int event_count;
			unsigned int i;
			int was_blanked = display_blanked;

			if (count <= 0)
				continue;
			idle_since = monotonic_seconds();
			set_display_blank(0);
			/* The touch that wakes the panel must not also type. */
			if (was_blanked)
				continue;
			event_count = count / sizeof(events[0]);
			if (!touch_reported) {
				log_text("C33 input: userspace console received evdev touch events\n");
				draw_checkpoint(7);
				touch_reported = 1;
			}
			for (i = 0; i < event_count; i++) {
				if (events[i].type == EV_ABS &&
				    events[i].code == ABS_X)
					touch_x = events[i].value;
				else if (events[i].type == EV_ABS &&
					 events[i].code == ABS_Y)
					touch_y = events[i].value;
				else if (events[i].type == EV_KEY &&
					 events[i].code == BTN_TOUCH &&
					 events[i].value) {
					int previous = active_key;


					active_key = key_at(touch_x, touch_y);
					draw_checkpoint(8);
					if (previous != active_key) {
						flush_key(previous);
						flush_key(active_key);
					}
				} else if (events[i].type == EV_KEY &&
					   events[i].code == BTN_TOUCH &&
					   !events[i].value) {
					int released = key_at(touch_x, touch_y);
					int previous = active_key;
					int redraw_keyboard = 0;

					draw_checkpoint(9);
					if (released >= 0 && released == active_key &&
					    send_key(master_fd, released,
						     &redraw_keyboard))
						draw_checkpoint(10);
					active_key = -1;
					if (redraw_keyboard)
						flush_keyboard();
					else
						flush_key(previous);
					write_rows(STATUS_Y, 6);
				}
			}
		}
		if (poll_fds[0].revents & (POLLERR | POLLHUP))
			break;
	}
	waitpid(shell, NULL, 0);
	return 1;
}
