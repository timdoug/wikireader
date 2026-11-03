// SPDX-License-Identifier: GPL-2.0-only
/*
 * The panel through DRM, as wr-console and wr-selftest show it: the first
 * connected connector and the first CRTC, a dumb buffer of one bit a pixel
 * added as DRM_FORMAT_D1 (the leftmost pixel in a byte's high bit, dark
 * where set, as wr-panel.h draws), mapped, and shown or blanked with
 * SETCRTC.  The panel's controller scans the buffer in place, so a write to
 * the mapping is on the screen.  Plain kernel ioctls: no libdrm.
 */
#ifndef WR_DRM_H
#define WR_DRM_H

#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

struct wr_drm {
	int fd;
	uint32_t crtc;
	uint32_t connector;
	uint32_t fb;
	uint32_t handle;
	uint32_t pitch;
	uint32_t width;
	uint32_t height;
	uint64_t size;
	uint8_t *map;
	struct drm_mode_modeinfo mode;
};

static inline int wr_drm_ioctl(int fd, unsigned long request, void *arg)
{
	int ret;

	do
		ret = ioctl(fd, request, arg);
	while (ret < 0 && (errno == EINTR || errno == EAGAIN));
	return ret;
}

/* The first connected connector with a mode, and the first CRTC. */
static inline int wr_drm_find(struct wr_drm *d)
{
	struct drm_mode_card_res res;
	uint32_t crtcs[4], connectors[4];
	unsigned int i;

	memset(&res, 0, sizeof(res));
	if (wr_drm_ioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0 ||
	    !res.count_crtcs || !res.count_connectors ||
	    res.count_crtcs > 4 || res.count_connectors > 4)
		return -1;
	res.count_fbs = res.count_encoders = 0;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.connector_id_ptr = (uintptr_t)connectors;
	if (wr_drm_ioctl(d->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
		return -1;
	d->crtc = crtcs[0];
	for (i = 0; i < res.count_connectors; i++) {
		struct drm_mode_get_connector conn;
		struct drm_mode_modeinfo modes[4];

		memset(&conn, 0, sizeof(conn));
		conn.connector_id = connectors[i];
		if (wr_drm_ioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0 ||
		    conn.connection != 1 || !conn.count_modes ||
		    conn.count_modes > 4)
			continue;
		conn.count_props = conn.count_encoders = 0;
		conn.modes_ptr = (uintptr_t)modes;
		if (wr_drm_ioctl(d->fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0)
			continue;
		d->connector = connectors[i];
		d->mode = modes[0];
		d->width = modes[0].hdisplay;
		d->height = modes[0].vdisplay;
		return 0;
	}
	return -1;
}

/* A D1 dumb buffer the size of the mode, added and mapped. */
static inline int wr_drm_buffer(struct wr_drm *d)
{
	struct drm_mode_create_dumb create = {
		.width = d->width, .height = d->height, .bpp = 1,
	};
	struct drm_mode_fb_cmd2 fb;
	struct drm_mode_map_dumb map;
	void *p;

	if (wr_drm_ioctl(d->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0)
		return -1;
	d->handle = create.handle;
	d->pitch = create.pitch;
	d->size = create.size;
	memset(&fb, 0, sizeof(fb));
	fb.width = d->width;
	fb.height = d->height;
	fb.pixel_format = DRM_FORMAT_D1;
	fb.handles[0] = d->handle;
	fb.pitches[0] = d->pitch;
	if (wr_drm_ioctl(d->fd, DRM_IOCTL_MODE_ADDFB2, &fb) < 0)
		return -1;
	d->fb = fb.fb_id;
	memset(&map, 0, sizeof(map));
	map.handle = d->handle;
	if (wr_drm_ioctl(d->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0)
		return -1;
	p = mmap(NULL, d->size, PROT_READ | PROT_WRITE, MAP_SHARED, d->fd,
		 map.offset);
	if (p == MAP_FAILED)
		return -1;
	d->map = p;
	return 0;
}

/* The buffer on the panel, or the CRTC off: the panel's only blanking. */
static inline int wr_drm_show(struct wr_drm *d, int on)
{
	struct drm_mode_crtc crtc;

	memset(&crtc, 0, sizeof(crtc));
	crtc.crtc_id = d->crtc;
	if (on) {
		crtc.fb_id = d->fb;
		crtc.set_connectors_ptr = (uintptr_t)&d->connector;
		crtc.count_connectors = 1;
		crtc.mode = d->mode;
		crtc.mode_valid = 1;
	}
	return wr_drm_ioctl(d->fd, DRM_IOCTL_MODE_SETCRTC, &crtc);
}

/* Open the card, find the panel and make its buffer; the first opener is
   DRM master, as the console is at boot. */
static inline int wr_drm_open(struct wr_drm *d, const char *path)
{
	memset(d, 0, sizeof(*d));
	d->fd = open(path, O_RDWR | O_CLOEXEC);
	if (d->fd < 0)
		return -1;
	if (wr_drm_find(d) < 0 || wr_drm_buffer(d) < 0) {
		close(d->fd);
		d->fd = -1;
		return -1;
	}
	return 0;
}

static inline int wr_drm_master(struct wr_drm *d, int take)
{
	return wr_drm_ioctl(d->fd, take ? DRM_IOCTL_SET_MASTER :
			    DRM_IOCTL_DROP_MASTER, NULL);
}

#endif
