// SPDX-License-Identifier: GPL-2.0-only
/*
 * drmbench: what a frame costs in each of the panel's DRM formats.
 *
 * Takes the display from fbdev emulation as a compositor would, and for D1
 * (the panel's own, scanned in place), RGB565 and XRGB8888 (converted to it
 * by the driver where damaged) times three frames: filling the screen,
 * scrolling a terminal by a text line and drawing the new one, and typing a
 * character.  Each is split into drawing (userspace, in the format) and
 * flushing (DIRTYFB, the driver's conversion).  Closing gives the display
 * back to fbdev emulation.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>

#define W	240
#define H	208

struct format {
	uint32_t fourcc;
	unsigned int bpp;
	const char *name;
};

static const struct format formats[] = {
	{ DRM_FORMAT_D1, 1, "D1" },
	{ DRM_FORMAT_RGB565, 16, "RGB565" },
	{ DRM_FORMAT_XRGB8888, 32, "XRGB8888" },
};

struct surface {
	const struct format *f;
	uint8_t *map;
	uint32_t pitch, handle, fb;
	size_t size;
};

static int fd;
static uint32_t crtc_id, connector_id;
static struct drm_mode_modeinfo mode;

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void die(const char *what)
{
	fprintf(stderr, "drmbench: %s: %s\n", what, strerror(errno));
	exit(1);
}

static void find_output(void)
{
	struct drm_mode_card_res res = { 0 };
	struct drm_mode_get_connector conn = { 0 };
	uint32_t crtcs[4], connectors[4];
	struct drm_mode_modeinfo modes[4];

	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
		die("resources");
	if (!res.count_crtcs || !res.count_connectors)
		die("no output");
	res.count_crtcs = res.count_connectors = 1;
	res.count_fbs = res.count_encoders = 0;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.connector_id_ptr = (uintptr_t)connectors;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
		die("resources");
	crtc_id = crtcs[0];
	connector_id = connectors[0];
	conn.connector_id = connector_id;
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0 || !conn.count_modes)
		die("connector");
	conn.count_modes = 1;
	conn.count_props = conn.count_encoders = 0;
	conn.modes_ptr = (uintptr_t)modes;
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
		die("connector");
	mode = modes[0];
}

static void make_surface(struct surface *s, const struct format *f)
{
	struct drm_mode_create_dumb create = { .width = W, .height = H, .bpp = f->bpp };
	struct drm_mode_map_dumb map = { 0 };
	struct drm_mode_fb_cmd2 fb = { .width = W, .height = H, .pixel_format = f->fourcc };
	struct drm_mode_crtc crtc = { 0 };

	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0)
		die("create dumb");
	map.handle = create.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0)
		die("map dumb");
	s->map = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
	if (s->map == MAP_FAILED)
		die("mmap");
	s->f = f;
	s->pitch = create.pitch;
	s->handle = create.handle;
	s->size = create.size;
	fb.handles[0] = create.handle;
	fb.pitches[0] = create.pitch;
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0)
		die("add framebuffer");
	s->fb = fb.fb_id;
	crtc.crtc_id = crtc_id;
	crtc.fb_id = s->fb;
	crtc.set_connectors_ptr = (uintptr_t)&connector_id;
	crtc.count_connectors = 1;
	crtc.mode = mode;
	crtc.mode_valid = 1;
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc) < 0)
		die("set crtc");
}

static void free_surface(struct surface *s)
{
	struct drm_mode_destroy_dumb destroy = { .handle = s->handle };

	ioctl(fd, DRM_IOCTL_MODE_RMFB, &s->fb);
	munmap(s->map, s->size);
	ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
}

/* A pixel, black or white, the way a renderer in this format writes one. */
static inline void put(struct surface *s, unsigned int x, unsigned int y, int black)
{
	uint8_t *row = s->map + y * s->pitch;

	switch (s->f->bpp) {
	case 1:
		if (black)
			row[x >> 3] |= 0x80 >> (x & 7);
		else
			row[x >> 3] &= ~(0x80 >> (x & 7));
		break;
	case 16:
		((uint16_t *)row)[x] = black ? 0 : 0xffff;
		break;
	default:
		((uint32_t *)row)[x] = black ? 0 : 0xffffff;
		break;
	}
}

/* A 6x8 cell with a checkered glyph, drawn pixel by pixel as fb/ would. */
static void glyph(struct surface *s, unsigned int x0, unsigned int y0, unsigned int n)
{
	unsigned int x, y;

	for (y = 0; y < 8; y++)
		for (x = 0; x < 6; x++)
			put(s, x0 + x, y0 + y, x < 5 && y < 7 && ((x + y + n) & 1));
}

static double flush_time;

static void flush(struct surface *s, unsigned int x1, unsigned int y1,
		  unsigned int x2, unsigned int y2)
{
	struct drm_clip_rect clip = { x1, y1, x2, y2 };
	struct drm_mode_fb_dirty_cmd dirty = {
		.fb_id = s->fb, .num_clips = 1, .clips_ptr = (uintptr_t)&clip,
	};
	double t = now();

	/* D1 is scanned in place and has nothing to flush. */
	if (s->f->bpp != 1 && ioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dirty) < 0)
		die("dirtyfb");
	flush_time += now() - t;
}

static void report(const char *test, const struct format *f, unsigned int n, double t)
{
	printf("DRMBENCH %-8s %-6s %7.2f ms a frame (draw %6.2f, flush %6.2f)\n",
	       f->name, test, t * 1000 / n, (t - flush_time) * 1000 / n,
	       flush_time * 1000 / n);
}

int main(void)
{
	unsigned int i, x, y, k;

	fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		die("/dev/dri/card0");
	if (ioctl(fd, DRM_IOCTL_SET_MASTER, 0) < 0)
		die("set master");
	find_output();
	for (k = 0; k < sizeof(formats) / sizeof(formats[0]); k++) {
		struct surface s;
		double t;

		make_surface(&s, &formats[k]);

		flush_time = 0;
		t = now();
		for (i = 0; i < 10; i++) {
			for (y = 0; y < H; y++)
				for (x = 0; x < W; x++)
					put(&s, x, y, (x ^ y ^ i) & 1);
			flush(&s, 0, 0, W, H);
		}
		report("fill", s.f, 10, now() - t);

		flush_time = 0;
		t = now();
		for (i = 0; i < 30; i++) {
			memmove(s.map, s.map + 8 * s.pitch, (H - 8) * s.pitch);
			for (x = 0; x < 40; x++)
				glyph(&s, x * 6, H - 8, x + i);
			flush(&s, 0, 0, W, H);
		}
		report("scroll", s.f, 30, now() - t);

		flush_time = 0;
		t = now();
		for (i = 0; i < 200; i++) {
			x = (i % 40) * 6;
			y = (i / 40 % 26) * 8;
			glyph(&s, x, y, i);
			flush(&s, x, y, x + 6, y + 8);
		}
		report("type", s.f, 200, now() - t);

		free_surface(&s);
	}
	ioctl(fd, DRM_IOCTL_DROP_MASTER, 0);
	close(fd);
	return 0;
}
