// SPDX-License-Identifier: GPL-2.0-only
/* WikiReader userspace framebuffer terminal and touchscreen keyboard. */
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <stdint.h>
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
#define KEY_ROWS	3

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
};

static const char *const key_labels[KEY_ROWS][10] = {
	{ "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P" },
	{ "A", "S", "D", "F", "G", "H", "J", "K", "L", "BS" },
	{ "Z", "X", "C", "V", "SP", "SP", "B", "N", "M", "EN" },
};

static const unsigned char key_values[KEY_ROWS][11] = {
	"qwertyuiop",
	"asdfghjkl\177",
	"zxcv  bnm\n",
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
static int fb_fd;
static int log_fd;

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

	if (character >= 'a' && character <= 'z')
		character -= 'a' - 'A';
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
	unsigned int gx;
	unsigned int gy;

	for (gy = 0; gy < FONT_HEIGHT; gy++)
		for (gx = 0; gx < FONT_WIDTH; gx++) {
			int foreground = gy < 7 && gx < 5 &&
				(rows[gy] & (1U << (4 - gx)));

			set_pixel(x + gx, y + gy,
				  inverse ? !foreground : foreground);
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
	const char *label = key_labels[row][column];
	unsigned int label_length = strlen(label);
	unsigned int label_x = x0 + (KEY_WIDTH - label_length * FONT_WIDTH) / 2;
	unsigned int label_y = y0 + (y1 - y0 + 1 - FONT_HEIGHT) / 2;
	unsigned int x;
	unsigned int y;

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
	draw_text_rows(first, last);
	write_rows(first * FONT_HEIGHT, (last - first + 1) * FONT_HEIGHT);
	dirty_text_first = TEXT_ROWS;
	dirty_text_last = 0;
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
	cursor_y = TEXT_ROWS - 1;
	mark_all_text();
}

static void terminal_newline(void)
{
	cursor_x = 0;
	if (++cursor_y == TEXT_ROWS)
		scroll_terminal();
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
			memset(cells, ' ', sizeof(cells));
			cursor_x = 0;
			cursor_y = 0;
			mark_all_text();
		} else if (byte == 'K') {
			memset(&cells[cursor_y][cursor_x], ' ',
			       TEXT_COLUMNS - cursor_x);
			mark_text_row(cursor_y);
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
	if (++cursor_x == TEXT_COLUMNS)
		terminal_newline();
}

static int key_at(unsigned int x, unsigned int y)
{
	unsigned int row;

	if (x >= LCD_WIDTH || y < KEYBOARD_Y || y >= LCD_HEIGHT)
		return -1;
	row = (y - KEYBOARD_Y) * KEY_ROWS / (LCD_HEIGHT - KEYBOARD_Y);
	return row * 10 + x / KEY_WIDTH;
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

int main(void)
{
	struct fb_var_screeninfo variable;
	struct fb_fix_screeninfo fixed;
	struct pollfd poll_fds[2];
	struct input_event events[8];
	unsigned int touch_x = 0;
	unsigned int touch_y = 0;
	int touch_reported = 0;
	int slave_fd;
	int input_fd;
	int master_fd;
	pid_t shell;

	log_fd = open("/dev/ttyC0", O_WRONLY | O_NONBLOCK);
	if (log_fd < 0)
		log_fd = STDERR_FILENO;
	fb_fd = open("/dev/fb0", O_RDWR);
	input_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
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

	poll_fds[0].fd = master_fd;
	poll_fds[0].events = POLLIN;
	poll_fds[1].fd = input_fd;
	poll_fds[1].events = POLLIN;
	for (;;) {
		if (poll(poll_fds, 2, -1) < 0)
			continue;
		if (poll_fds[0].revents & POLLIN) {
			unsigned char output[128];
			ssize_t count = read(master_fd, output, sizeof(output));
			ssize_t i;

			if (count > 0) {
				write(log_fd, output, count);
				for (i = 0; i < count; i++)
					terminal_byte(output[i]);
				flush_text();
			}
		}
		if (poll_fds[1].revents & POLLIN) {
			ssize_t count = read(input_fd, events, sizeof(events));
			unsigned int event_count;
			unsigned int i;

			if (count <= 0)
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

					draw_checkpoint(9);
					if (released >= 0 && released == active_key &&
					    write(master_fd,
						  &key_values[released / 10]
							     [released % 10], 1) == 1)
						draw_checkpoint(10);
					active_key = -1;
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
