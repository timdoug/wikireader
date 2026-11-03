// SPDX-License-Identifier: GPL-2.0-only
/*
 * The panel through DRM, as wr-console and wr-selftest show it: the first
 * connected connector, the first CRTC and its primary plane, and a dumb
 * buffer of one bit a pixel added as DRM_FORMAT_D1 (the leftmost pixel in a
 * byte's high bit, dark where set, as wr-panel.h draws), mapped.  The
 * buffer is shown, and the panel blanked, by atomic commits: the mode, the
 * connector's CRTC, the plane's framebuffer and the CRTC's ACTIVE, which off
 * is the panel's only blanking.  The panel's controller scans the buffer in
 * place, so a write to the mapping is on the screen.  Plain kernel ioctls:
 * no libdrm.
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

/* The plane "type" property's "Primary", as the kernel numbers it. */
#define WR_PLANE_TYPE_PRIMARY	1

/* The properties a commit sets, in the order wr_drm_show() lists them. */
enum {
	WR_CRTC_MODE_ID, WR_CRTC_ACTIVE,
	WR_CONNECTOR_CRTC_ID,
	WR_PLANE_FB_ID, WR_PLANE_CRTC_ID,
	WR_PLANE_SRC_X, WR_PLANE_SRC_Y, WR_PLANE_SRC_W, WR_PLANE_SRC_H,
	WR_PLANE_CRTC_X, WR_PLANE_CRTC_Y, WR_PLANE_CRTC_W, WR_PLANE_CRTC_H,
	WR_PROPS
};

struct wr_drm {
	int fd;
	uint32_t crtc;
	uint32_t connector;
	uint32_t plane;
	uint32_t mode_blob;
	uint32_t prop[WR_PROPS];
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

/*
 * An object's property named NAME: its id, and its value through VALUE if
 * that is not NULL.  Zero if it has none.
 */
static inline uint32_t wr_drm_property(int fd, uint32_t object, uint32_t type,
				       const char *name, uint64_t *value)
{
	struct drm_mode_obj_get_properties list = {
		.obj_id = object, .obj_type = type,
	};
	uint32_t ids[32];
	uint64_t values[32];
	unsigned int i;

	if (wr_drm_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &list) < 0 ||
	    list.count_props > 32)
		return 0;
	list.props_ptr = (uintptr_t)ids;
	list.prop_values_ptr = (uintptr_t)values;
	if (wr_drm_ioctl(fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &list) < 0)
		return 0;
	for (i = 0; i < list.count_props; i++) {
		struct drm_mode_get_property property = { .prop_id = ids[i] };

		if (wr_drm_ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &property) < 0 ||
		    strcmp(property.name, name))
			continue;
		if (value)
			*value = values[i];
		return ids[i];
	}
	return 0;
}

/* The first connected connector with a mode, the first CRTC and its
   primary plane. */
static inline int wr_drm_find(struct wr_drm *d)
{
	struct drm_mode_card_res res;
	struct drm_mode_get_plane_res planes;
	uint32_t crtcs[4], connectors[4], plane_ids[4];
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
	for (i = 0; i < res.count_connectors && !d->connector; i++) {
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
	}
	if (!d->connector)
		return -1;

	memset(&planes, 0, sizeof(planes));
	if (wr_drm_ioctl(d->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &planes) < 0 ||
	    !planes.count_planes || planes.count_planes > 4)
		return -1;
	planes.plane_id_ptr = (uintptr_t)plane_ids;
	if (wr_drm_ioctl(d->fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &planes) < 0)
		return -1;
	for (i = 0; i < planes.count_planes && !d->plane; i++) {
		struct drm_mode_get_plane plane = { .plane_id = plane_ids[i] };
		uint64_t type = 0;

		if (wr_drm_ioctl(d->fd, DRM_IOCTL_MODE_GETPLANE, &plane) == 0 &&
		    (plane.possible_crtcs & 1) &&
		    wr_drm_property(d->fd, plane_ids[i], DRM_MODE_OBJECT_PLANE,
				    "type", &type) &&
		    type == WR_PLANE_TYPE_PRIMARY)
			d->plane = plane_ids[i];
	}
	return d->plane ? 0 : -1;
}

/* The property ids a commit sets, and the mode as a blob. */
static inline int wr_drm_properties(struct wr_drm *d)
{
	static const struct {
		uint32_t type;
		const char *name;
	} names[WR_PROPS] = {
		{ DRM_MODE_OBJECT_CRTC, "MODE_ID" },
		{ DRM_MODE_OBJECT_CRTC, "ACTIVE" },
		{ DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID" },
		{ DRM_MODE_OBJECT_PLANE, "FB_ID" },
		{ DRM_MODE_OBJECT_PLANE, "CRTC_ID" },
		{ DRM_MODE_OBJECT_PLANE, "SRC_X" },
		{ DRM_MODE_OBJECT_PLANE, "SRC_Y" },
		{ DRM_MODE_OBJECT_PLANE, "SRC_W" },
		{ DRM_MODE_OBJECT_PLANE, "SRC_H" },
		{ DRM_MODE_OBJECT_PLANE, "CRTC_X" },
		{ DRM_MODE_OBJECT_PLANE, "CRTC_Y" },
		{ DRM_MODE_OBJECT_PLANE, "CRTC_W" },
		{ DRM_MODE_OBJECT_PLANE, "CRTC_H" },
	};
	struct drm_mode_create_blob blob = {
		.data = (uintptr_t)&d->mode, .length = sizeof(d->mode),
	};
	unsigned int i;

	for (i = 0; i < WR_PROPS; i++) {
		uint32_t object = names[i].type == DRM_MODE_OBJECT_CRTC ? d->crtc :
			names[i].type == DRM_MODE_OBJECT_CONNECTOR ? d->connector :
			d->plane;

		d->prop[i] = wr_drm_property(d->fd, object, names[i].type,
					     names[i].name, NULL);
		if (!d->prop[i])
			return -1;
	}
	if (wr_drm_ioctl(d->fd, DRM_IOCTL_MODE_CREATEPROPBLOB, &blob) < 0)
		return -1;
	d->mode_blob = blob.blob_id;
	return 0;
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

/*
 * The buffer on the panel, the panel lit or dark: one atomic commit of the
 * whole pipe, whatever another program left in it.
 */
static inline int wr_drm_show(struct wr_drm *d, int on)
{
	uint32_t objects[3] = { d->crtc, d->connector, d->plane };
	uint32_t counts[3] = { 2, 1, WR_PROPS - 3 };
	uint64_t values[WR_PROPS] = {
		[WR_CRTC_MODE_ID] = d->mode_blob,
		[WR_CRTC_ACTIVE] = !!on,
		[WR_CONNECTOR_CRTC_ID] = d->crtc,
		[WR_PLANE_FB_ID] = d->fb,
		[WR_PLANE_CRTC_ID] = d->crtc,
		[WR_PLANE_SRC_W] = (uint64_t)d->width << 16,
		[WR_PLANE_SRC_H] = (uint64_t)d->height << 16,
		[WR_PLANE_CRTC_W] = d->width,
		[WR_PLANE_CRTC_H] = d->height,
	};
	struct drm_mode_atomic commit = {
		.flags = DRM_MODE_ATOMIC_ALLOW_MODESET,
		.count_objs = 3,
		.objs_ptr = (uintptr_t)objects,
		.count_props_ptr = (uintptr_t)counts,
		.props_ptr = (uintptr_t)d->prop,
		.prop_values_ptr = (uintptr_t)values,
	};

	return wr_drm_ioctl(d->fd, DRM_IOCTL_MODE_ATOMIC, &commit);
}

/* Open the card, find the panel and make its buffer; the first opener is
   DRM master, as the console is at boot. */
static inline int wr_drm_open(struct wr_drm *d, const char *path)
{
	struct drm_set_client_cap universal = {
		.capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES, .value = 1,
	};
	struct drm_set_client_cap atomic = {
		.capability = DRM_CLIENT_CAP_ATOMIC, .value = 1,
	};

	memset(d, 0, sizeof(*d));
	d->fd = open(path, O_RDWR | O_CLOEXEC);
	if (d->fd < 0)
		return -1;
	if (wr_drm_ioctl(d->fd, DRM_IOCTL_SET_CLIENT_CAP, &universal) < 0 ||
	    wr_drm_ioctl(d->fd, DRM_IOCTL_SET_CLIENT_CAP, &atomic) < 0 ||
	    wr_drm_find(d) < 0 || wr_drm_properties(d) < 0 ||
	    wr_drm_buffer(d) < 0) {
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
