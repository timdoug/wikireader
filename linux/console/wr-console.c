// SPDX-License-Identifier: GPL-2.0-only
/* WikiReader userspace framebuffer terminal. */
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <linux/input.h>
#include <time.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "wr-keymap.h"
#include "wr-panel.h"

#define TEXT_COLUMNS	40
#define TEXT_ROWS	15
#define TEXT_HEIGHT	(TEXT_ROWS * FONT_HEIGHT)

#define OUTPUT_BATCH_BYTES	256
#define OUTPUT_QUIET_MS		8
#define SCROLL_FRAME_MS		16

/*
 * The terminal is the Linux console as TERM=linux describes it to curses
 * programs: cursor addressing and movement, insertion and deletion of
 * characters and lines, a scrolling region, reverse video, saving and
 * reporting the cursor.  Colours and the other renditions are accepted and
 * ignored, since the panel has one bit a pixel.
 */
#define ATTR_REVERSE	1
#define MAX_PARAMETERS	8

enum {
	ESCAPE_NONE,
	ESCAPE_START,
	ESCAPE_CSI,
	ESCAPE_SKIP,
	ESCAPE_OSC,
};

static unsigned char cells[TEXT_ROWS][TEXT_COLUMNS];
static unsigned char attrs[TEXT_ROWS][TEXT_COLUMNS];
static unsigned int cursor_x;
static unsigned int cursor_y;
static int wrap_pending;
static unsigned char rendition;
static unsigned int scroll_top;
static unsigned int scroll_bottom = TEXT_ROWS - 1;
static unsigned int saved_x;
static unsigned int saved_y;
static unsigned char saved_rendition;
static int cursor_visible = 1;
static int cursor_drawn;
static unsigned int drawn_x;
static unsigned int drawn_y;
static unsigned int escape_state;
static unsigned int parameters[MAX_PARAMETERS];
static unsigned int parameter_count;
static int private_mode;
static int terminal_fd = -1;	/* the PTY master, for replies */
static unsigned int dirty_text_first = TEXT_ROWS;
static unsigned int dirty_text_last;
static int fb_fd;
static int log_fd;
/*
 * The panel belongs to whichever program holds the foreground VT.  The
 * console holds VT 1 in process mode, so the kernel asks it before
 * switching away: while another VT has the panel (an X server, say) the
 * console draws only into its own copy, drops touches and keys, and stops
 * its blank and suspend timers; when the VT comes back it repaints.
 */
static int vt_fd = -1;
static int vt_pipe[2] = { -1, -1 };
static int vt_away;
/*
 * Key delivery.  No VT carries a terminal, so this program is the shell's
 * keyboard driver: everything that arrives on a keyboard-shaped evdev node
 * -- wr-keyboard's soft keyboard, the front buttons, anything added later,
 * which inotify on /dev/input reports -- is turned into bytes for the PTY.
 */
#define MAX_KEYBOARDS		8
static int keyboard_fds[MAX_KEYBOARDS];
static unsigned int keyboard_numbers[MAX_KEYBOARDS];
static unsigned int keyboard_count;
static int inotify_fd = -1;
static int shift_down;
static int control_down;
static int keyboard_reported;
static int button_reported;
static unsigned int scrolls_pending;
/*
 * Pacing keeps a burst of output readable, but it must never sit between a
 * key and the first thing that key produced: that delay is the latency the
 * user feels.  The first frame after input is painted immediately and only
 * the continuation of the same burst is paced.
 */
static int burst_painted;

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

/*
 * The kernel's uptime once the prompt is up, into the kernel log: the
 * launcher's bootlog.txt (with bootlog.on on the card) has Grifo's part of
 * a boot, from the tap to the kernel starting, and this the rest.
 */
static void note_ready(void)
{
	char uptime[32], line[96];
	ssize_t length = -1;
	int fd = open("/proc/uptime", O_RDONLY);

	if (fd >= 0) {
		length = read(fd, uptime, sizeof(uptime) - 1);
		close(fd);
	}
	if (length <= 0)
		return;
	uptime[length] = '\0';
	uptime[strcspn(uptime, " ")] = '\0';
	snprintf(line, sizeof(line),
		 "C33 console: prompt up %s s after the kernel started\n", uptime);
	fd = open("/dev/kmsg", O_WRONLY);
	if (fd >= 0) {
		length = write(fd, line, strlen(line));
		close(fd);
	}
}

static void write_rows(unsigned int first, unsigned int count)
{
	size_t offset = first * LCD_STRIDE;
	size_t length = count * LCD_STRIDE;
	size_t done = 0;

	if (!count || first >= LCD_HEIGHT || vt_away)
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

static void draw_text_rows(unsigned int first, unsigned int last)
{
	unsigned int row;
	unsigned int column;

	clear_rows(first * FONT_HEIGHT, (last - first + 1) * FONT_HEIGHT);
	for (row = first; row <= last; row++)
		for (column = 0; column < TEXT_COLUMNS; column++)
			draw_character(column * FONT_WIDTH, row * FONT_HEIGHT,
				       cells[row][column],
				       (attrs[row][column] & ATTR_REVERSE) ^
				       (cursor_drawn && row == drawn_y &&
					column == drawn_x));
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

static void flush_display(void)
{
	draw_text_rows(0, TEXT_ROWS - 1);
	write_rows(0, TEXT_HEIGHT);
	dirty_text_first = TEXT_ROWS;
	dirty_text_last = 0;
}

static void draw_cell(unsigned int row, unsigned int column)
{
	int inverse = attrs[row][column] & ATTR_REVERSE;

	if (cursor_drawn && row == drawn_y && column == drawn_x)
		inverse = !inverse;
	draw_character(column * FONT_WIDTH, row * FONT_HEIGHT,
		       cells[row][column], inverse);
	mark_text_row(row);
}

static void redraw_row(unsigned int row)
{
	unsigned int column;

	for (column = 0; column < TEXT_COLUMNS; column++)
		draw_cell(row, column);
}

/* The cursor is its cell drawn inverted, taken down while output is
   processed so that scrolling never moves it with the text. */
static void hide_cursor(void)
{
	if (!cursor_drawn)
		return;
	cursor_drawn = 0;
	draw_cell(drawn_y, drawn_x);
}

static void show_cursor(void)
{
	if (!cursor_visible)
		return;
	drawn_x = cursor_x;
	drawn_y = cursor_y;
	cursor_drawn = 1;
	draw_cell(drawn_y, drawn_x);
}

static void blank_cells(unsigned int row, unsigned int first,
			unsigned int count)
{
	memset(&cells[row][first], ' ', count);
	memset(&attrs[row][first], 0, count);
}

static void scroll_region_up(unsigned int top, unsigned int bottom,
			     unsigned int count)
{
	unsigned int rows = bottom - top + 1;
	unsigned int row;

	if (count > rows)
		count = rows;
	memmove(cells[top], cells[top + count], (rows - count) * TEXT_COLUMNS);
	memmove(attrs[top], attrs[top + count], (rows - count) * TEXT_COLUMNS);
	for (row = bottom + 1 - count; row <= bottom; row++)
		blank_cells(row, 0, TEXT_COLUMNS);
	memmove(framebuffer + top * FONT_HEIGHT * LCD_STRIDE,
		framebuffer + (top + count) * FONT_HEIGHT * LCD_STRIDE,
		(rows - count) * FONT_HEIGHT * LCD_STRIDE);
	clear_rows((bottom + 1 - count) * FONT_HEIGHT, count * FONT_HEIGHT);
	for (row = top; row <= bottom; row++)
		mark_text_row(row);
	scrolls_pending++;
}

static void scroll_region_down(unsigned int top, unsigned int bottom,
			       unsigned int count)
{
	unsigned int rows = bottom - top + 1;
	unsigned int row;

	if (count > rows)
		count = rows;
	memmove(cells[top + count], cells[top], (rows - count) * TEXT_COLUMNS);
	memmove(attrs[top + count], attrs[top], (rows - count) * TEXT_COLUMNS);
	for (row = top; row < top + count; row++)
		blank_cells(row, 0, TEXT_COLUMNS);
	memmove(framebuffer + (top + count) * FONT_HEIGHT * LCD_STRIDE,
		framebuffer + top * FONT_HEIGHT * LCD_STRIDE,
		(rows - count) * FONT_HEIGHT * LCD_STRIDE);
	clear_rows(top * FONT_HEIGHT, count * FONT_HEIGHT);
	for (row = top; row <= bottom; row++)
		mark_text_row(row);
	scrolls_pending++;
}

/* Line feed: down a line, scrolling at the bottom of the region. */
static void terminal_index(void)
{
	if (cursor_y == scroll_bottom)
		scroll_region_up(scroll_top, scroll_bottom, 1);
	else if (cursor_y < TEXT_ROWS - 1)
		cursor_y++;
}

static void terminal_reverse_index(void)
{
	if (cursor_y == scroll_top)
		scroll_region_down(scroll_top, scroll_bottom, 1);
	else if (cursor_y)
		cursor_y--;
}

static void erase_in_line(unsigned int row, unsigned int first,
			  unsigned int last)
{
	if (last >= TEXT_COLUMNS)
		last = TEXT_COLUMNS - 1;
	if (first > last)
		return;
	blank_cells(row, first, last - first + 1);
	redraw_row(row);
}

static void erase_in_display(unsigned int mode)
{
	unsigned int row;

	if (mode == 0) {
		erase_in_line(cursor_y, cursor_x, TEXT_COLUMNS - 1);
		for (row = cursor_y + 1; row < TEXT_ROWS; row++)
			erase_in_line(row, 0, TEXT_COLUMNS - 1);
	} else if (mode == 1) {
		for (row = 0; row < cursor_y; row++)
			erase_in_line(row, 0, TEXT_COLUMNS - 1);
		erase_in_line(cursor_y, 0, cursor_x);
	} else if (mode == 2 || mode == 3) {
		for (row = 0; row < TEXT_ROWS; row++)
			blank_cells(row, 0, TEXT_COLUMNS);
		clear_rows(0, TEXT_HEIGHT);
		mark_all_text();
	}
}

/* Characters shift within the cursor's line; the rest of the line is kept. */
static void shift_line(int insert, unsigned int count)
{
	unsigned int x = cursor_x;
	unsigned int keep;

	if (count > TEXT_COLUMNS - x)
		count = TEXT_COLUMNS - x;
	keep = TEXT_COLUMNS - x - count;
	if (insert) {
		memmove(&cells[cursor_y][x + count], &cells[cursor_y][x], keep);
		memmove(&attrs[cursor_y][x + count], &attrs[cursor_y][x], keep);
		blank_cells(cursor_y, x, count);
	} else {
		memmove(&cells[cursor_y][x], &cells[cursor_y][x + count], keep);
		memmove(&attrs[cursor_y][x], &attrs[cursor_y][x + count], keep);
		blank_cells(cursor_y, TEXT_COLUMNS - count, count);
	}
	redraw_row(cursor_y);
}

static void terminal_reply(const char *text)
{
	if (terminal_fd >= 0)
		write(terminal_fd, text, strlen(text));
}

static void move_cursor(int row, int column)
{
	if (row < 0)
		row = 0;
	if (row >= TEXT_ROWS)
		row = TEXT_ROWS - 1;
	if (column < 0)
		column = 0;
	if (column >= TEXT_COLUMNS)
		column = TEXT_COLUMNS - 1;
	cursor_y = row;
	cursor_x = column;
	wrap_pending = 0;
}

/* The parameter, or @fallback when it is absent or zero. */
static unsigned int parameter(unsigned int index, unsigned int fallback)
{
	if (index >= parameter_count || !parameters[index])
		return fallback;
	return parameters[index];
}

static void control_sequence(unsigned char final)
{
	unsigned int n = parameter(0, 1);
	unsigned int i;
	char reply[24];

	if (private_mode) {
		/* DECTCEM; the other private modes do not apply here. */
		if ((final == 'h' || final == 'l') && parameter_count &&
		    parameters[0] == 25)
			cursor_visible = final == 'h';
		return;
	}
	switch (final) {
	case 'A':
		move_cursor(cursor_y - n, cursor_x);
		break;
	case 'B':
	case 'e':
		move_cursor(cursor_y + n, cursor_x);
		break;
	case 'C':
	case 'a':
		move_cursor(cursor_y, cursor_x + n);
		break;
	case 'D':
		move_cursor(cursor_y, cursor_x - n);
		break;
	case 'E':
		move_cursor(cursor_y + n, 0);
		break;
	case 'F':
		move_cursor(cursor_y - n, 0);
		break;
	case 'G':
	case '`':
		move_cursor(cursor_y, n - 1);
		break;
	case 'd':
		move_cursor(n - 1, cursor_x);
		break;
	case 'H':
	case 'f':
		move_cursor(parameter(0, 1) - 1, parameter(1, 1) - 1);
		break;
	case 'J':
		erase_in_display(parameter(0, 0));
		break;
	case 'K':
		if (parameter(0, 0) == 1)
			erase_in_line(cursor_y, 0, cursor_x);
		else if (parameter(0, 0) == 2)
			erase_in_line(cursor_y, 0, TEXT_COLUMNS - 1);
		else
			erase_in_line(cursor_y, cursor_x, TEXT_COLUMNS - 1);
		break;
	case 'X':
		erase_in_line(cursor_y, cursor_x, cursor_x + n - 1);
		break;
	case '@':
		shift_line(1, n);
		break;
	case 'P':
		shift_line(0, n);
		break;
	case 'L':
		if (cursor_y >= scroll_top && cursor_y <= scroll_bottom)
			scroll_region_down(cursor_y, scroll_bottom, n);
		break;
	case 'M':
		if (cursor_y >= scroll_top && cursor_y <= scroll_bottom)
			scroll_region_up(cursor_y, scroll_bottom, n);
		break;
	case 'S':
		scroll_region_up(scroll_top, scroll_bottom, n);
		break;
	case 'T':
		scroll_region_down(scroll_top, scroll_bottom, n);
		break;
	case 'r':
		i = parameter(1, TEXT_ROWS);
		if (i > TEXT_ROWS)
			i = TEXT_ROWS;
		if (parameter(0, 1) < i) {
			scroll_top = parameter(0, 1) - 1;
			scroll_bottom = i - 1;
		}
		move_cursor(0, 0);
		break;
	case 'm':
		/* Reverse video is the rendition one bit a pixel can show. */
		if (!parameter_count)
			rendition = 0;
		for (i = 0; i < parameter_count; i++) {
			if (parameters[i] == 0)
				rendition = 0;
			else if (parameters[i] == 7)
				rendition |= ATTR_REVERSE;
			else if (parameters[i] == 27)
				rendition &= ~ATTR_REVERSE;
		}
		break;
	case 'n':
		if (parameter(0, 0) == 5) {
			terminal_reply("\033[0n");
		} else if (parameter(0, 0) == 6) {
			snprintf(reply, sizeof(reply), "\033[%u;%uR",
				 cursor_y + 1, cursor_x + 1);
			terminal_reply(reply);
		}
		break;
	case 'c':
		terminal_reply("\033[?6c");
		break;
	case 's':
		saved_x = cursor_x;
		saved_y = cursor_y;
		saved_rendition = rendition;
		break;
	case 'u':
		move_cursor(saved_y, saved_x);
		rendition = saved_rendition;
		break;
	}
}

/*
 * The Linux console's CP437 line-drawing characters, which curses sends for
 * TERM=linux boxes, as the ASCII the font has.
 */
static unsigned char printable(unsigned char byte)
{
	if (byte < 0x80)
		return byte;
	switch (byte) {
	case 0xb3: case 0xba:
		return '|';
	case 0xc4: case 0xcd:
		return '-';
	case 0xb0: case 0xb1: case 0xb2: case 0xdb:
		return '#';
	case 0xf8:
		return 'o';
	case 0xfe: case 0xf9: case 0xfa:
		return '.';
	default:
		return (byte >= 0xb4 && byte <= 0xda) || byte == 0xc5 ?
			'+' : '?';
	}
}

static void terminal_put(unsigned char byte)
{
	/* A character in the last column wraps only when the next one comes,
	   so a full-screen program can fill that column without scrolling. */
	if (wrap_pending) {
		cursor_x = 0;
		terminal_index();
		wrap_pending = 0;
	}
	cells[cursor_y][cursor_x] = printable(byte);
	attrs[cursor_y][cursor_x] = rendition;
	draw_cell(cursor_y, cursor_x);
	if (cursor_x == TEXT_COLUMNS - 1)
		wrap_pending = 1;
	else
		cursor_x++;
}

static void terminal_reset(void)
{
	rendition = 0;
	scroll_top = 0;
	scroll_bottom = TEXT_ROWS - 1;
	cursor_visible = 1;
	erase_in_display(2);
	move_cursor(0, 0);
}

static void terminal_byte(unsigned char byte)
{
	switch (escape_state) {
	case ESCAPE_START:
		escape_state = ESCAPE_NONE;
		switch (byte) {
		case '[':
			escape_state = ESCAPE_CSI;
			parameter_count = 0;
			parameters[0] = 0;
			private_mode = 0;
			return;
		case ']':
			escape_state = ESCAPE_OSC;
			return;
		case '(': case ')': case '*': case '+': case '#': case '%':
			escape_state = ESCAPE_SKIP;
			return;
		case '7':
			saved_x = cursor_x;
			saved_y = cursor_y;
			saved_rendition = rendition;
			return;
		case '8':
			move_cursor(saved_y, saved_x);
			rendition = saved_rendition;
			return;
		case 'D':
			terminal_index();
			return;
		case 'E':
			cursor_x = 0;
			terminal_index();
			return;
		case 'M':
			wrap_pending = 0;
			terminal_reverse_index();
			return;
		case 'c':
			terminal_reset();
			return;
		}
		return;
	case ESCAPE_CSI:
		if (byte >= '0' && byte <= '9') {
			if (!parameter_count)
				parameter_count = 1;
			parameters[parameter_count - 1] =
				parameters[parameter_count - 1] * 10 + byte - '0';
			return;
		}
		if (byte == ';') {
			if (!parameter_count)
				parameter_count = 1;
			if (parameter_count < MAX_PARAMETERS)
				parameters[parameter_count++] = 0;
			return;
		}
		if (byte == '?' || byte == '>' || byte == '=') {
			private_mode = 1;
			return;
		}
		if (byte >= 0x20 && byte < 0x40)
			return;		/* intermediates */
		escape_state = ESCAPE_NONE;
		control_sequence(byte);
		return;
	case ESCAPE_SKIP:
		escape_state = ESCAPE_NONE;
		return;
	case ESCAPE_OSC:
		/* A title or palette: ignored up to BEL or ESC \. */
		if (byte == 0x07)
			escape_state = ESCAPE_NONE;
		else if (byte == 0x1b)
			escape_state = ESCAPE_SKIP;
		return;
	}

	switch (byte) {
	case 0x1b:
		escape_state = ESCAPE_START;
		return;
	case '\r':
		cursor_x = 0;
		wrap_pending = 0;
		return;
	case '\n':
	case '\v':
	case '\f':
		wrap_pending = 0;
		terminal_index();
		return;
	case '\b':
		if (wrap_pending)
			wrap_pending = 0;
		else if (cursor_x)
			cursor_x--;
		return;
	case '\t':
		move_cursor(cursor_y, (cursor_x / 8 + 1) * 8);
		return;
	}
	if (byte < 0x20 || byte == 0x7f)
		return;
	terminal_put(byte);
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
		hide_cursor();
		for (i = 0; i < count; i++)
			terminal_byte(output[i]);
		total += count;
		if (total >= OUTPUT_BATCH_BYTES || !burst_painted ||
		    poll(&more, 1, OUTPUT_QUIET_MS) <= 0 ||
		    !(more.revents & POLLIN))
			break;
	}
	show_cursor();
	flush_text();
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

/* Open event node N if it is a keyboard: keys and no absolute axes. */
static void add_keyboard(unsigned int n)
{
	unsigned long evbits = 0;
	char path[32];
	unsigned int k;
	int fd;

	for (k = 0; k < keyboard_count; k++)
		if (keyboard_numbers[k] == n)
			return;
	if (keyboard_count == MAX_KEYBOARDS)
		return;
	snprintf(path, sizeof(path), "/dev/input/event%u", n);
	fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return;
	if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), &evbits) < 0 ||
	    (evbits & (1ul << EV_ABS)) || !(evbits & (1ul << EV_KEY))) {
		close(fd);
		return;
	}
	keyboard_numbers[keyboard_count] = n;
	keyboard_fds[keyboard_count++] = fd;
}

static void scan_keyboards(void)
{
	unsigned int n;

	for (n = 0; n < 32; n++)
		add_keyboard(n);
}

/* A node came or went in /dev/input: wr-keyboard's may come after this. */
static void input_nodes_changed(void)
{
	char buffer[512];

	while (read(inotify_fd, buffer, sizeof(buffer)) > 0)
		;
	scan_keyboards();
}

static void drop_keyboard(unsigned int k)
{
	close(keyboard_fds[k]);
	keyboard_count--;
	keyboard_fds[k] = keyboard_fds[keyboard_count];
	keyboard_numbers[k] = keyboard_numbers[keyboard_count];
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
	if (!deliver_key(master_fd, event->code, shift_down, control_down))
		return 0;
	if (!keyboard_reported) {
		log_text("C33 input: keyboards feed the PTY through evdev\n");
		keyboard_reported = 1;
	}
	return 1;
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
		.ws_ypixel = TEXT_HEIGHT,
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
	char *const argv[] = { "sh", "-l", "-i", NULL };
	/* Programs in the SD card's bin folder run by name, so a short command
	   can be added without rebuilding the image. */
	char *const envp[] = {
		"HOME=/root", "PATH=/bin:/sbin:/usr/bin:/usr/sbin:/mnt/sd/bin",
		"TERM=linux", NULL,
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

static void vt_signalled(int sig)
{
	char byte = sig == SIGUSR1 ? 'r' : 'a';

	write(vt_pipe[1], &byte, 1);
}

static void take_vt(void)
{
	struct vt_mode mode = {
		.mode = VT_PROCESS, .relsig = SIGUSR1, .acqsig = SIGUSR2,
	};
	struct sigaction action = { .sa_handler = vt_signalled };

	if (pipe(vt_pipe) < 0)
		return;
	fcntl(vt_pipe[0], F_SETFL, O_NONBLOCK);
	fcntl(vt_pipe[1], F_SETFL, O_NONBLOCK);
	vt_fd = open("/dev/tty1", O_RDWR | O_NOCTTY);
	if (vt_fd < 0)
		return;
	sigaction(SIGUSR1, &action, NULL);
	sigaction(SIGUSR2, &action, NULL);
	/* The console reads the keyboards itself; the VT only owns the panel. */
	ioctl(vt_fd, KDSETMODE, KD_GRAPHICS);
	ioctl(vt_fd, KDSKBMODE, K_OFF);
	if (ioctl(vt_fd, VT_SETMODE, &mode) < 0) {
		close(vt_fd);
		vt_fd = -1;
		return;
	}
	log_text("C33 console: holding VT 1\n");
}

static void vt_request(void)
{
	char byte;

	while (read(vt_pipe[0], &byte, 1) == 1) {
		if (byte == 'r' && !vt_away) {
			vt_away = 1;
			ioctl(vt_fd, VT_RELDISP, 1);
			log_text("C33 console: panel handed to another VT\n");
		} else if (byte == 'a' && vt_away) {
			ioctl(vt_fd, VT_RELDISP, VT_ACKACQ);
			vt_away = 0;
			write_rows(0, TEXT_HEIGHT);
			log_text("C33 console: panel back\n");
		}
	}
}

int main(void)
{
	struct fb_var_screeninfo variable;
	struct fb_fix_screeninfo fixed;
	struct pollfd poll_fds[3 + MAX_KEYBOARDS];
	struct input_event events[8];
	int slave_fd;
	int master_fd;
	pid_t shell;

	index_glyphs();

	log_fd = open("/dev/ttyC0", O_WRONLY | O_NONBLOCK);
	if (log_fd < 0)
		log_fd = STDERR_FILENO;
	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0 ||
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
	for (const char *banner = "C33 USERSPACE CONSOLE\r\n"; *banner; banner++)
		terminal_byte(*banner);
	inotify_fd = inotify_init1(IN_NONBLOCK);
	if (inotify_fd >= 0)
		inotify_add_watch(inotify_fd, "/dev/input", IN_CREATE);
	scan_keyboards();

	master_fd = open_pty(&slave_fd);
	if (master_fd < 0) {
		log_text("C33 userspace console: PTY allocation FAILED\n");
		return 1;
	}
	terminal_fd = master_fd;
	shell = start_shell(master_fd, slave_fd);
	close(slave_fd);
	if (shell < 0) {
		log_text("C33 userspace console: shell start FAILED\n");
		return 1;
	}
	flush_display();
	log_text("C33 userspace console: fbdev + evdev + PTY shell ready\n");
	note_ready();
	take_vt();

	for (;;) {
		unsigned int poll_count = 1;
		unsigned int vt_index = 0;
		unsigned int inotify_index = 0;
		unsigned int first_keyboard;
		unsigned int k;

		poll_fds[0].fd = master_fd;
		poll_fds[0].events = POLLIN;
		if (vt_fd >= 0) {
			vt_index = poll_count;
			poll_fds[poll_count].fd = vt_pipe[0];
			poll_fds[poll_count++].events = POLLIN;
		}
		if (inotify_fd >= 0) {
			inotify_index = poll_count;
			poll_fds[poll_count].fd = inotify_fd;
			poll_fds[poll_count++].events = POLLIN;
		}
		first_keyboard = poll_count;
		for (k = 0; k < keyboard_count; k++) {
			poll_fds[poll_count].fd = keyboard_fds[k];
			poll_fds[poll_count++].events = POLLIN;
		}
		if (poll(poll_fds, poll_count, -1) < 0)
			continue;
		if (vt_index && (poll_fds[vt_index].revents & POLLIN))
			vt_request();
		/* Backwards, since dropping one moves the last into its place. */
		for (k = keyboard_count; k-- > 0;) {
			short revents = poll_fds[first_keyboard + k].revents;
			ssize_t count;
			unsigned int i;

			if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
				drop_keyboard(k);
				continue;
			}
			if (!(revents & POLLIN))
				continue;
			count = read(keyboard_fds[k], events, sizeof(events));
			if (count <= 0 || vt_away)
				continue;
			for (i = 0; i < count / sizeof(events[0]); i++)
				keyboard_event(master_fd, &events[i]);
		}
		/* After the drops: a new node may reuse a dropped one's number. */
		if (inotify_index && (poll_fds[inotify_index].revents & POLLIN))
			input_nodes_changed();
		if (poll_fds[0].revents & POLLIN)
			consume_terminal_output(master_fd);
		if (poll_fds[0].revents & (POLLERR | POLLHUP))
			break;
	}
	waitpid(shell, NULL, 0);
	return 1;
}
