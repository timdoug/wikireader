// SPDX-License-Identifier: GPL-2.0-only
/*
 * wlkeyboard: the soft keyboard on Wayland.
 *
 * The console's keyboard (wr-keys.h), drawn the same, on a layer shell
 * surface across the bottom of the panel whose exclusive zone keeps the
 * application above it.  A tapped key is sent with the virtual keyboard
 * protocol, so it goes to whichever client has the keyboard focus; the
 * surface itself takes no focus.  The keymap given with it is the
 * system's default, the US one wr-keymap.h's codes are.
 *
 * It draws one bit a pixel, as the console does, and widens what changed
 * into a shared-memory buffer, RGB565 where the compositor takes it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wr-keys.h"

#define SURFACE_HEIGHT	(LCD_HEIGHT - KEYBOARD_Y)

struct buffer {
	struct wl_buffer *buffer;
	uint8_t *pixels;
	int busy;
	/* Panel rows this buffer has yet to be given. */
	unsigned int stale_first, stale_last;
};

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwp_virtual_keyboard_manager_v1 *keyboard_manager;
static struct zwp_virtual_keyboard_v1 *keyboard;
static struct wl_surface *surface;
static struct zwlr_layer_surface_v1 *layer_surface;
static struct wl_touch *touch;
static struct wl_pointer *pointer;

static struct buffer buffers[2];
static int rgb565;
static unsigned int bytes_per_pixel = 4;
static int configured;
static int running = 1;
/* Panel rows drawn but not yet shown. */
static unsigned int dirty_first = LCD_HEIGHT, dirty_last;
static uint32_t shift_mask, control_mask;
static int pointer_x, pointer_y, touch_x, touch_y;
static int32_t touch_id = -1;

static void die(const char *what)
{
	fprintf(stderr, "wlkeyboard: %s\n", what);
	exit(1);
}

static uint32_t now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void mark_rows(unsigned int first, unsigned int last)
{
	if (first < dirty_first)
		dirty_first = first;
	if (last > dirty_last)
		dirty_last = last;
}

/* A key's rows, as draw_key() lays them out. */
static void mark_key(int key)
{
	unsigned int row;

	if (key < 0 || key >= KEY_ROWS * 10)
		return;
	row = key / 10;
	mark_rows(KEYBOARD_Y + row * SURFACE_HEIGHT / KEY_ROWS,
		  KEYBOARD_Y + (row + 1) * SURFACE_HEIGHT / KEY_ROWS - 1);
}

static void redraw_key(int key)
{
	if (key < 0 || key >= KEY_ROWS * 10)
		return;
	draw_key(key, key == active_key);
	mark_key(key);
}

static void redraw_keyboard(void)
{
	int key;

	clear_rows(KEYBOARD_Y, SURFACE_HEIGHT);
	for (key = 0; key < KEY_ROWS * 10; key++)
		draw_key(key, key == active_key);
	mark_rows(KEYBOARD_Y, LCD_HEIGHT - 1);
}

/* Widen panel rows first..last into a buffer, black or white. */
static void widen(struct buffer *b, unsigned int first, unsigned int last)
{
	unsigned int y, x;

	for (y = first; y <= last; y++) {
		const uint8_t *in = framebuffer + y * LCD_STRIDE;
		uint8_t *row = b->pixels + (y - KEYBOARD_Y) * LCD_WIDTH * bytes_per_pixel;

		if (rgb565) {
			uint16_t *out = (uint16_t *)row;

			for (x = 0; x < LCD_WIDTH; x++)
				out[x] = in[x >> 3] & (0x80 >> (x & 7)) ? 0 : 0xffff;
		} else {
			uint32_t *out = (uint32_t *)row;

			for (x = 0; x < LCD_WIDTH; x++)
				out[x] = in[x >> 3] & (0x80 >> (x & 7)) ? 0 : 0xffffff;
		}
	}
}

/*
 * Show what was drawn, in whichever buffer the compositor is not reading;
 * with both held, when one comes back.  Each buffer catches up on the rows
 * drawn since it was last shown.
 */
static void present(void)
{
	struct buffer *b;
	unsigned int i;

	if (!configured || dirty_first > dirty_last)
		return;
	for (i = 0; i < 2; i++) {
		if (dirty_first < buffers[i].stale_first)
			buffers[i].stale_first = dirty_first;
		if (dirty_last > buffers[i].stale_last)
			buffers[i].stale_last = dirty_last;
	}
	for (i = 0; i < 2 && buffers[i].busy; i++)
		;
	if (i == 2)
		return;
	b = &buffers[i];
	widen(b, b->stale_first, b->stale_last);
	wl_surface_attach(surface, b->buffer, 0, 0);
	wl_surface_damage_buffer(surface, 0, dirty_first - KEYBOARD_Y, LCD_WIDTH,
				 dirty_last - dirty_first + 1);
	wl_surface_commit(surface);
	b->busy = 1;
	b->stale_first = LCD_HEIGHT;
	b->stale_last = 0;
	dirty_first = LCD_HEIGHT;
	dirty_last = 0;
}

static void buffer_release(void *data, struct wl_buffer *wl_buffer)
{
	struct buffer *b = data;

	b->busy = 0;
	present();
}

static const struct wl_buffer_listener buffer_listener = {
	.release = buffer_release,
};

static void make_buffers(void)
{
	size_t size = LCD_WIDTH * bytes_per_pixel * SURFACE_HEIGHT;
	struct wl_shm_pool *pool;
	uint8_t *pixels;
	unsigned int i;
	int fd;

	fd = memfd_create("wlkeyboard", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, 2 * size) < 0)
		die("cannot make shared memory");
	pixels = mmap(NULL, 2 * size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED)
		die("cannot map shared memory");
	pool = wl_shm_create_pool(shm, fd, 2 * size);
	for (i = 0; i < 2; i++) {
		buffers[i].pixels = pixels + i * size;
		buffers[i].buffer = wl_shm_pool_create_buffer(pool, i * size,
			LCD_WIDTH, SURFACE_HEIGHT, LCD_WIDTH * bytes_per_pixel,
			rgb565 ? WL_SHM_FORMAT_RGB565 : WL_SHM_FORMAT_XRGB8888);
		wl_buffer_add_listener(buffers[i].buffer, &buffer_listener, &buffers[i]);
		buffers[i].stale_first = KEYBOARD_Y;
		buffers[i].stale_last = LCD_HEIGHT - 1;
	}
	wl_shm_pool_destroy(pool);
	close(fd);
}

/* The keymap that key codes are read in: the system's default. */
static void send_keymap(void)
{
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap;
	char *text;
	size_t size;
	int fd;

	if (!context)
		die("no xkb context");
	keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!keymap)
		die("no keymap");
	shift_mask = 1u << xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
	control_mask = 1u << xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CTRL);
	text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	if (!text)
		die("cannot write the keymap");
	size = strlen(text) + 1;
	fd = memfd_create("wlkeyboard-keymap", MFD_CLOEXEC);
	if (fd < 0 || write(fd, text, size) != (ssize_t)size)
		die("cannot pass the keymap");
	zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size);
	close(fd);
	free(text);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
}

/*
 * A key, pressed and released.  The compositor keeps no modifier state for
 * a virtual keyboard's keys, so the modifiers are sent as state around it.
 */
static void send_key(int code, int shift, int control)
{
	uint32_t mods = (shift ? shift_mask : 0) | (control ? control_mask : 0);
	uint32_t time = now_ms();

	if (mods)
		zwp_virtual_keyboard_v1_modifiers(keyboard, mods, 0, 0, 0);
	zwp_virtual_keyboard_v1_key(keyboard, time, code, WL_KEYBOARD_KEY_STATE_PRESSED);
	zwp_virtual_keyboard_v1_key(keyboard, time, code, WL_KEYBOARD_KEY_STATE_RELEASED);
	if (mods)
		zwp_virtual_keyboard_v1_modifiers(keyboard, 0, 0, 0, 0);
}

/* A finger or the button, at surface coordinates. */
static void press(int x, int y)
{
	int previous = active_key;

	active_key = key_at(x, y + KEYBOARD_Y);
	if (previous != active_key) {
		redraw_key(previous);
		redraw_key(active_key);
	}
	present();
}

static void release(int x, int y)
{
	int released = key_at(x, y + KEYBOARD_Y);
	int previous = active_key;
	int redraw = 0;

	if (released >= 0 && released == active_key) {
		int code, shift, control;

		redraw = tap_key(released, &code, &shift, &control);
		if (code >= 0)
			send_key(code, shift, control);
	}
	active_key = -1;
	if (redraw)
		redraw_keyboard();
	else
		redraw_key(previous);
	present();
}

static void touch_down(void *data, struct wl_touch *t, uint32_t serial, uint32_t time,
		       struct wl_surface *s, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
	if (touch_id >= 0)
		return;
	touch_id = id;
	touch_x = wl_fixed_to_int(x);
	touch_y = wl_fixed_to_int(y);
	press(touch_x, touch_y);
}

static void touch_up(void *data, struct wl_touch *t, uint32_t serial, uint32_t time,
		     int32_t id)
{
	if (id != touch_id)
		return;
	touch_id = -1;
	release(touch_x, touch_y);
}

static void touch_motion(void *data, struct wl_touch *t, uint32_t time, int32_t id,
			 wl_fixed_t x, wl_fixed_t y)
{
	if (id != touch_id)
		return;
	touch_x = wl_fixed_to_int(x);
	touch_y = wl_fixed_to_int(y);
	press(touch_x, touch_y);
}

static void touch_frame(void *data, struct wl_touch *t)
{
}

static void touch_cancel(void *data, struct wl_touch *t)
{
	int previous = active_key;

	touch_id = -1;
	active_key = -1;
	redraw_key(previous);
	present();
}

static void touch_shape(void *data, struct wl_touch *t, int32_t id, wl_fixed_t major,
			wl_fixed_t minor)
{
}

static void touch_orientation(void *data, struct wl_touch *t, int32_t id,
			      wl_fixed_t orientation)
{
}

static const struct wl_touch_listener touch_listener = {
	.down = touch_down,
	.up = touch_up,
	.motion = touch_motion,
	.frame = touch_frame,
	.cancel = touch_cancel,
	.shape = touch_shape,
	.orientation = touch_orientation,
};

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial,
			  struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	pointer_x = wl_fixed_to_int(x);
	pointer_y = wl_fixed_to_int(y);
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial,
			  struct wl_surface *s)
{
	touch_cancel(data, NULL);
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time,
			   wl_fixed_t x, wl_fixed_t y)
{
	pointer_x = wl_fixed_to_int(x);
	pointer_y = wl_fixed_to_int(y);
	if (active_key >= 0)
		press(pointer_x, pointer_y);
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial,
			   uint32_t time, uint32_t button, uint32_t state)
{
	if (button != BTN_LEFT)
		return;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED)
		press(pointer_x, pointer_y);
	else
		release(pointer_x, pointer_y);
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time,
			 uint32_t axis, wl_fixed_t value)
{
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
};

static void seat_capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
	if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !touch) {
		touch = wl_seat_get_touch(s);
		wl_touch_add_listener(touch, &touch_listener, NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	}
}

static void seat_name(void *data, struct wl_seat *s, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void shm_format(void *data, struct wl_shm *s, uint32_t format)
{
	if (format == WL_SHM_FORMAT_RGB565) {
		rgb565 = 1;
		bytes_per_pixel = 2;
	}
}

static const struct wl_shm_listener shm_listener = {
	.format = shm_format,
};

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *l, uint32_t serial,
			    uint32_t width, uint32_t height)
{
	zwlr_layer_surface_v1_ack_configure(l, serial);
	if (configured)
		return;
	configured = 1;
	make_buffers();
	redraw_keyboard();
	present();
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *l)
{
	running = 0;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
			    const char *interface, uint32_t version)
{
	if (!strcmp(interface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	} else if (!strcmp(interface, wl_shm_interface.name)) {
		shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
		wl_shm_add_listener(shm, &shm_listener, NULL);
	} else if (!strcmp(interface, wl_seat_interface.name) && !seat) {
		/* Version 2: the events the listeners here cover. */
		seat = wl_registry_bind(registry, name, &wl_seat_interface, 2);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
		layer_shell = wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1);
	} else if (!strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name)) {
		keyboard_manager = wl_registry_bind(registry, name,
			&zwp_virtual_keyboard_manager_v1_interface, 1);
	}
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_global_remove,
};

int main(void)
{
	struct wl_display *display = wl_display_connect(NULL);
	struct wl_registry *registry;

	if (!display)
		die("cannot connect to the compositor");
	registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	/* The globals, then the shm formats and seat capabilities. */
	wl_display_roundtrip(display);
	wl_display_roundtrip(display);
	if (!compositor || !shm || !seat || !layer_shell || !keyboard_manager)
		die("the compositor lacks wl_shm, a seat, the layer shell or virtual keyboards");

	keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(keyboard_manager, seat);
	send_keymap();

	surface = wl_compositor_create_surface(compositor);
	layer_surface = zwlr_layer_shell_v1_get_layer_surface(layer_shell, surface, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP, "osk");
	zwlr_layer_surface_v1_add_listener(layer_surface, &layer_listener, NULL);
	zwlr_layer_surface_v1_set_size(layer_surface, LCD_WIDTH, SURFACE_HEIGHT);
	zwlr_layer_surface_v1_set_anchor(layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, SURFACE_HEIGHT);
	zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
	wl_surface_commit(surface);

	while (running && wl_display_dispatch(display) >= 0)
		;
	wl_display_disconnect(display);
	return 0;
}
