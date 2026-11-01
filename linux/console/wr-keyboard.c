// SPDX-License-Identifier: GPL-2.0-only
/*
 * WikiReader touchscreen keyboard and panel power.
 *
 * The keyboard is the bottom of the panel, four rows below the console's
 * text.  A tapped key is reported on a uinput device, "WikiReader soft
 * keyboard", with KEY_LEFTSHIFT and KEY_LEFTCTRL around it as a physical
 * keyboard would send them, so it reaches whatever reads keyboards: the
 * console's shell, and anything else.
 *
 * The panel's power is here too, since the touchscreen is both what keeps
 * the machine busy and what wakes it.  After wr.blank= seconds (from the
 * kernel command line, which the launcher takes from the card) with no touch
 * and no button the panel goes dark; after wr.suspend= the machine sleeps
 * until touched; the power switch, which cannot cut power from Linux, does
 * whichever is set.  Idle means no input, as for the kernel's own console
 * blanking.  While the panel is dark the buttons are grabbed, so that the
 * press that wakes it is spent on waking it and types nothing.
 *
 * When another VT has the panel (an X server) this draws nothing, ignores
 * touches and stops its timers; when VT 1 comes back it repaints.
 */
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

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

#define SOFT_KEYBOARD_NAME	"WikiReader soft keyboard"
#define MAX_BUTTONS		4
#define BLANK_SECONDS_DEFAULT	120

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

/* Keys the soft keyboard reports that carry no character of their own. */
static const unsigned short special_keys[] = {
	KEY_ENTER, KEY_BACKSPACE, KEY_TAB, KEY_ESC, KEY_LEFT, KEY_RIGHT,
	KEY_UP, KEY_DOWN, KEY_LEFTSHIFT, KEY_LEFTCTRL,
};

static int fb_fd = -1;
static int log_fd;
static int uinput_fd = -1;
static int touch_fd = -1;
static int button_fds[MAX_BUTTONS];
static unsigned int button_count;
static int active_fd = -1;	/* /sys/class/tty/tty0/active */
static int away;		/* another VT has the panel */
static int active_key = -1;
static int shift_active;
static int control_active;
static int symbols_active;
static int blank_seconds = BLANK_SECONDS_DEFAULT;
static int suspend_seconds;
static int power_logging;
static int display_blanked;
static int buttons_grabbed;
static int buttons_held;
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

static long monotonic_seconds(void)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
		return 0;
	return now.tv_sec;
}

static void write_rows(unsigned int first, unsigned int count)
{
	size_t offset = first * LCD_STRIDE;
	size_t length = count * LCD_STRIDE;
	size_t done = 0;

	if (!count || first >= LCD_HEIGHT || away)
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

static int key_at(unsigned int x, unsigned int y)
{
	unsigned int row;

	if (x >= LCD_WIDTH || y < KEYBOARD_Y || y >= LCD_HEIGHT)
		return -1;
	row = (y - KEYBOARD_Y) * KEY_ROWS / (LCD_HEIGHT - KEYBOARD_Y);
	return row * 10 + x / KEY_WIDTH;
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
static void emit_key(unsigned int code, int shift, int control)
{
	struct input_event events[10];
	unsigned int count = 0;

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
	if (write(uinput_fd, events, count * sizeof(events[0])) !=
	    (ssize_t)(count * sizeof(events[0])))
		log_text("C33 input: soft keyboard write FAILED\n");
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

/* A released key: change the keyboard's state or report the key. */
static int tap_key(int key)
{
	unsigned int code;
	int shift = 0;
	int control = 0;
	int redraw = 0;

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
		int character = key_character(key);
		int found = character < 0 ? -1 :
			key_for_character(character, &shift);

		if (found < 0)
			return 0;
		code = (unsigned int)found;
		control = control_active;
	}
	if (shift_active || control_active) {
		shift_active = 0;
		control_active = 0;
		redraw = 1;
	}
	emit_key(code, shift, control);
	return redraw;
}

/*
 * Sort the event nodes: the one with absolute axes is the touchscreen, and
 * anything else with keys, other than this program's own keyboard, is
 * buttons, which keep the machine awake and may be the power switch.
 */
static void open_input_devices(void)
{
	unsigned int n;

	for (n = 0; n < 16; n++) {
		unsigned long evbits = 0;
		char path[32];
		char name[64] = "";
		int fd;

		snprintf(path, sizeof(path), "/dev/input/event%u", n);
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
		if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), &evbits) < 0 ||
		    !strcmp(name, SOFT_KEYBOARD_NAME)) {
			close(fd);
		} else if (evbits & (1ul << EV_ABS)) {
			if (touch_fd < 0)
				touch_fd = fd;
			else
				close(fd);
		} else if ((evbits & (1ul << EV_KEY)) &&
			   button_count < MAX_BUTTONS) {
			button_fds[button_count++] = fd;
		} else {
			close(fd);
		}
	}
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
 * The port data, direction and function bytes for P0 and P6: what the power
 * switch and the buttons are reading, straight from the registers, for a
 * device that cannot say so any other way.
 */
static void port_state(char *out, size_t size)
{
	static const unsigned long addresses[] = {
		0x300380, 0x300381, 0x3003a0, 0x30038c, 0x30038d, 0x3003ac,
	};
	unsigned char values[6];
	unsigned int i;
	int fd = open("/dev/mem", O_RDONLY);

	if (fd < 0) {
		snprintf(out, size, "ports unreadable");
		return;
	}
	for (i = 0; i < 6; i++)
		if (lseek(fd, (off_t)addresses[i], SEEK_SET) < 0 ||
		    read(fd, &values[i], 1) != 1)
			values[i] = 0xee;
	close(fd);
	snprintf(out, size,
		 "P0D=%02x IOC0=%02x P0CFP03=%02x P6D=%02x IOC6=%02x P6CFP03=%02x",
		 values[0], values[1], values[2], values[3], values[4], values[5]);
}

/*
 * A device with no serial cannot say why it failed to wake, so with wr.pmlog
 * each event leaves a line on the card, and each suspend how long it slept
 * and how the interrupt counts moved across it.  A file that ends in
 * "suspending" is a machine that never came back.
 */
static void power_line(const char *what)
{
	char ports[96];
	char line[160];
	int length;
	int fd;

	if (!power_logging)
		return;
	fd = open("/mnt/sd/linuxpm.txt", O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0)
		return;
	port_state(ports, sizeof(ports));
	length = snprintf(line, sizeof(line), "%s: %s\n", what, ports);
	if (length > 0)
		write(fd, line, length);
	close(fd);
	sync();
}

static void power_note(const char *what, long seconds)
{
	char text[2048];
	char line[96];
	char ports[96];
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
	port_state(ports, sizeof(ports));
	length = snprintf(line, sizeof(line), "%s\n", ports);
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

static void grab_buttons(int grab)
{
	unsigned int i;

	if (grab == buttons_grabbed)
		return;
	for (i = 0; i < button_count; i++)
		ioctl(button_fds[i], EVIOCGRAB, grab);
	buttons_grabbed = grab;
}

/*
 * A dark panel takes the buttons for itself.  They are let go once the
 * panel is lit and no button is down, so that the release of the button
 * that woke it goes nowhere either.
 */
static void set_display_blank(int blank)
{
	if (blank != display_blanked &&
	    ioctl(fb_fd, FBIOBLANK,
		  blank ? FB_BLANK_POWERDOWN : FB_BLANK_UNBLANK) == 0) {
		display_blanked = blank;
		log_text(blank ? "C33 display: blanked while idle\n"
				: "C33 display: woken by touch\n");
	}
	if (display_blanked)
		grab_buttons(1);
	else if (!buttons_held)
		grab_buttons(0);
}

/*
 * Suspend is opt-in from the command line, because a machine that suspends
 * without a working wake source needs its batteries pulled.  The write
 * returns when the kernel has resumed.
 */
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
	 * takes that interrupt as its wake event, so no touch arrives to turn
	 * the panel back on.  Light it before anything slower happens, or the
	 * wait for the screen is whatever else this function does.
	 */
	set_display_blank(0);
	power_note("resumed", monotonic_seconds() - before);
}

/* Whether VT 1, the console's, still has the panel. */
static void check_active_vt(void)
{
	char name[16];
	ssize_t count;
	int was_away = away;

	if (active_fd < 0 || lseek(active_fd, 0, SEEK_SET) < 0)
		return;
	count = read(active_fd, name, sizeof(name) - 1);
	if (count <= 0)
		return;
	name[count] = '\0';
	away = strcmp(name, "tty1\n") != 0;
	if (away && !was_away) {
		active_key = -1;
	} else if (!away && was_away) {
		/* Whoever had the panel may have left it blanked. */
		display_blanked = 1;
		set_display_blank(0);
		idle_since = monotonic_seconds();
		flush_keyboard();
	}
}

static void button_event(const struct input_event *event)
{
	const char *button;
	char line[64];
	int was_blanked = display_blanked;

	if (event->type != EV_KEY)
		return;
	if (event->value == 0 && buttons_held > 0)
		buttons_held--;
	else if (event->value == 1)
		buttons_held++;
	/*
	 * Only a press counts as activity: the release of the key that
	 * blanked or suspended the machine arrives afterwards and must not
	 * undo it.
	 */
	if (event->value != 1) {
		set_display_blank(display_blanked);
		return;
	}
	button = button_name(event->code);
	if (button) {
		snprintf(line, sizeof(line), "button %s pressed", button);
		power_line(line);
	}
	idle_since = monotonic_seconds();
	set_display_blank(0);
	if (was_blanked || event->code != KEY_POWER)
		return;
	if (suspend_seconds) {
		log_text("C33 input: power switch suspends until touch\n");
		set_display_blank(1);
		suspend_until_touch();
		idle_since = monotonic_seconds();
	} else {
		log_text("C33 input: power switch blanks the panel\n");
		set_display_blank(1);
	}
}

static void touch_events(void)
{
	static unsigned int touch_x, touch_y;
	static int touch_reported;
	struct input_event events[8];
	ssize_t count = read(touch_fd, events, sizeof(events));
	unsigned int i;
	int was_blanked = display_blanked;

	if (count <= 0 || away)
		return;
	idle_since = monotonic_seconds();
	set_display_blank(0);
	/* The touch that wakes the panel must not also type. */
	if (was_blanked)
		return;
	if (!touch_reported) {
		log_text("C33 input: soft keyboard received evdev touch events\n");
		touch_reported = 1;
	}
	for (i = 0; i < count / sizeof(events[0]); i++) {
		if (events[i].type == EV_ABS && events[i].code == ABS_X) {
			touch_x = events[i].value;
		} else if (events[i].type == EV_ABS && events[i].code == ABS_Y) {
			touch_y = events[i].value;
		} else if (events[i].type == EV_KEY &&
			   events[i].code == BTN_TOUCH && events[i].value) {
			int previous = active_key;

			active_key = key_at(touch_x, touch_y);
			if (previous != active_key) {
				flush_key(previous);
				flush_key(active_key);
			}
		} else if (events[i].type == EV_KEY &&
			   events[i].code == BTN_TOUCH) {
			int released = key_at(touch_x, touch_y);
			int previous = active_key;
			int redraw = 0;

			if (released >= 0 && released == active_key)
				redraw = tap_key(released);
			active_key = -1;
			if (redraw)
				flush_keyboard();
			else
				flush_key(previous);
		}
	}
}

int main(void)
{
	struct fb_var_screeninfo variable;
	struct fb_fix_screeninfo fixed;
	struct pollfd poll_fds[2 + MAX_BUTTONS];
	unsigned int poll_count = 0;
	unsigned int k;

	log_fd = open("/dev/ttyC0", O_WRONLY | O_NONBLOCK);
	if (log_fd < 0)
		log_fd = STDERR_FILENO;
	fb_fd = open("/dev/fb0", O_RDWR);
	uinput_fd = create_soft_keyboard();
	open_input_devices();
	if (fb_fd < 0 || touch_fd < 0 || uinput_fd < 0 ||
	    ioctl(fb_fd, FBIOGET_VSCREENINFO, &variable) < 0 ||
	    ioctl(fb_fd, FBIOGET_FSCREENINFO, &fixed) < 0 ||
	    variable.xres != LCD_WIDTH || variable.yres != LCD_HEIGHT ||
	    variable.bits_per_pixel != 1 || fixed.line_length != LCD_STRIDE) {
		log_text("C33 keyboard: device setup FAILED\n");
		/* Respawning would only fail again, as fast as init can. */
		for (;;)
			pause();
	}
	log_text("C33 input: soft keyboard registered as a uinput device\n");
	active_fd = open("/sys/class/tty/tty0/active", O_RDONLY);
	check_active_vt();
	read_timeouts();
	idle_since = monotonic_seconds();
	flush_keyboard();

	poll_fds[poll_count].fd = touch_fd;
	poll_fds[poll_count++].events = POLLIN;
	for (k = 0; k < button_count; k++) {
		poll_fds[poll_count].fd = button_fds[k];
		poll_fds[poll_count++].events = POLLIN;
	}
	if (active_fd >= 0) {
		poll_fds[poll_count].fd = active_fd;
		poll_fds[poll_count++].events = POLLPRI | POLLERR;
	}
	for (;;) {
		long idle = monotonic_seconds() - idle_since;
		int wait = -1;

		if (away)
			idle = 0;
		if (blank_seconds && !display_blanked && idle >= blank_seconds)
			set_display_blank(1);
		if (suspend_seconds && idle >= suspend_seconds) {
			power_line("idle timeout");
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
		if (wait < 0 || away)
			wait = -1;
		else if (wait < 1)
			wait = 1;
		if (poll(poll_fds, poll_count, wait) < 0)
			continue;
		if (active_fd >= 0 &&
		    (poll_fds[poll_count - 1].revents & (POLLPRI | POLLERR)))
			check_active_vt();
		if (poll_fds[0].revents & POLLIN)
			touch_events();
		for (k = 0; k < button_count; k++) {
			struct input_event events[8];
			ssize_t count;
			unsigned int i;

			if (!(poll_fds[1 + k].revents & POLLIN))
				continue;
			count = read(button_fds[k], events, sizeof(events));
			if (count <= 0 || away)
				continue;
			for (i = 0; i < count / sizeof(events[0]); i++)
				button_event(&events[i]);
		}
	}
}
