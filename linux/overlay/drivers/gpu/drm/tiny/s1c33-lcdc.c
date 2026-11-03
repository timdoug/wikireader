// SPDX-License-Identifier: GPL-2.0-only
/*
 * DRM driver for the Epson S1C33E07 LCD controller, as the WikiReader drives
 * its 240x208 one-bit panel.
 *
 * The loader leaves the controller scanning with its timing set; Linux only
 * points it at a buffer (MADD, the main window's start address, which may be
 * internal video RAM or SDRAM, and its line offset in words) and switches it
 * on and off.  The panel's own format is DRM_FORMAT_D1: a bit a pixel, the
 * leftmost in a byte's high bit, 1 black.  A D1 framebuffer is scanned where
 * it is, so what a client writes through its mapping, with no way to say
 * what changed on a machine without an MMU, is on the panel as it is
 * written.  An XRGB8888 or RGB565 framebuffer, which is what modern
 * clients draw, is converted where damaged into the controller's own D1
 * image, which is then scanned.  The controller's internal video RAM is
 * the kernel's A0 RAM (asm/iram.h), so every image is in SDRAM.
 */
#include <linux/dma-mapping.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_panic.h>
#include <drm/drm_plane.h>
#include <drm/drm_probe_helper.h>

#define S1C33_LCD_WIDTH		240
#define S1C33_LCD_HEIGHT	208
/* The controller fetches whole words: 240 pixels take 8 of them. */
#define S1C33_LCD_STRIDE	32

/* Power save: 0 off, 2 doze, 3 driving the panel. */
#define S1C33_LCDC_PS		0x04
#define S1C33_LCDC_PS_MASK	0x3
#define S1C33_LCDC_PS_OFF	0x0
#define S1C33_LCDC_PS_NORMAL	0x3
/* The main window's start address and line offset in words. */
#define S1C33_LCDC_MADD		0x70
#define S1C33_LCDC_MLADD	0x74

struct s1c33_lcdc {
	struct drm_device drm;
	struct drm_plane plane;
	struct drm_crtc crtc;
	struct drm_encoder encoder;
	struct drm_connector connector;
	void __iomem *regs;
	struct gpio_desc *enable;
	/* The controller's own image: converted frames, the panic screen. */
	u8 *image;
	dma_addr_t image_dma;
};

static struct s1c33_lcdc *to_lcdc(struct drm_device *drm)
{
	return container_of(drm, struct s1c33_lcdc, drm);
}

static const struct drm_display_mode s1c33_lcdc_mode = {
	DRM_SIMPLE_MODE(S1C33_LCD_WIDTH, S1C33_LCD_HEIGHT, 0, 0),
};

static const u32 s1c33_lcdc_formats[] = {
	DRM_FORMAT_D1,
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_RGB565,
};

static void s1c33_lcdc_scan(struct s1c33_lcdc *lcdc, phys_addr_t addr,
			    unsigned int pitch)
{
	writel(pitch / 4, lcdc->regs + S1C33_LCDC_MLADD);
	writel(addr, lcdc->regs + S1C33_LCDC_MADD);
}

/*
 * The panel is lit by the loader, so this adopts its state: the enable line
 * goes up last and down first, since a panel without it fades instead of
 * clearing.
 */
static void s1c33_lcdc_power(struct s1c33_lcdc *lcdc, bool on)
{
	u32 ps = readl(lcdc->regs + S1C33_LCDC_PS) & ~S1C33_LCDC_PS_MASK;

	if (on) {
		writel(ps | S1C33_LCDC_PS_NORMAL, lcdc->regs + S1C33_LCDC_PS);
		gpiod_set_value_cansleep(lcdc->enable, 1);
	} else {
		gpiod_set_value_cansleep(lcdc->enable, 0);
		writel(ps | S1C33_LCDC_PS_OFF, lcdc->regs + S1C33_LCDC_PS);
	}
}

/*
 * Dark where green is below half: on a black-and-white panel that is the
 * same answer as half luma for every grey, which is what clients draw, and
 * for the six pure colours, at a third of the instructions.  Eight pixels
 * make a byte, unrolled, since the kernel is built for size and would not
 * unroll it, and this loop is all a converted frame costs.
 */
#define S1C33_XRGB_DARK(p)	(~(p) >> 15 & 1)
#define S1C33_RGB565_DARK(p)	(~(p) >> 10 & 1)

#define S1C33_BYTE(dark, px)						\
	((dark((px)[0]) << 7) | (dark((px)[1]) << 6) |			\
	 (dark((px)[2]) << 5) | (dark((px)[3]) << 4) |			\
	 (dark((px)[4]) << 3) | (dark((px)[5]) << 2) |			\
	 (dark((px)[6]) << 1) | dark((px)[7]))

/* Convert the damaged rectangle of an XRGB8888 or RGB565 frame to D1. */
static void s1c33_lcdc_convert(struct s1c33_lcdc *lcdc,
			       struct drm_framebuffer *fb,
			       const struct drm_rect *clip)
{
	struct drm_gem_dma_object *gem = drm_fb_dma_get_gem_obj(fb, 0);
	const u8 *src = (const u8 *)gem->vaddr + fb->offsets[0];
	unsigned int pitch = fb->pitches[0];
	/* Whole bytes of the panel; its 240 pixels are 30 of them. */
	unsigned int b0 = clip->x1 / 8, b1 = DIV_ROUND_UP(clip->x2, 8);
	unsigned int y, b;

	for (y = clip->y1; y < clip->y2; y++) {
		u8 *out = lcdc->image + y * S1C33_LCD_STRIDE;

		if (fb->format->format == DRM_FORMAT_RGB565) {
			const u16 *px = (const u16 *)(src + y * pitch) + b0 * 8;

			for (b = b0; b < b1; b++, px += 8)
				out[b] = S1C33_BYTE(S1C33_RGB565_DARK, px);
		} else {
			const u32 *px = (const u32 *)(src + y * pitch) + b0 * 8;

			for (b = b0; b < b1; b++, px += 8)
				out[b] = S1C33_BYTE(S1C33_XRGB_DARK, px);
		}
	}
}

static int s1c33_lcdc_plane_atomic_check(struct drm_plane *plane,
					 struct drm_atomic_commit *state)
{
	struct s1c33_lcdc *lcdc = to_lcdc(plane->dev);
	struct drm_plane_state *plane_state =
		drm_atomic_get_new_plane_state(state, plane);
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, &lcdc->crtc);
	struct drm_framebuffer *fb = plane_state->fb;
	int ret;

	ret = drm_atomic_helper_check_plane_state(plane_state, crtc_state,
						  DRM_PLANE_NO_SCALING,
						  DRM_PLANE_NO_SCALING,
						  false, false);
	if (ret || !plane_state->visible || fb->format->format != DRM_FORMAT_D1)
		return ret;
	/* Scanned as it is: from a word, in whole words a line. */
	if (fb->pitches[0] % 4 || (plane_state->src_x >> 16) % 32 ||
	    (drm_fb_dma_get_gem_addr(fb, plane_state, 0) & 3))
		return -EINVAL;
	return 0;
}

/*
 * A D1 framebuffer is pointed at; any other has its damage converted into
 * the controller's own D1 image, which is pointed at.  This runs before the
 * CRTC is enabled, so the panel lights on the right picture.
 */
static void s1c33_lcdc_plane_atomic_update(struct drm_plane *plane,
					   struct drm_atomic_commit *state)
{
	struct s1c33_lcdc *lcdc = to_lcdc(plane->dev);
	struct drm_plane_state *old_state =
		drm_atomic_get_old_plane_state(state, plane);
	struct drm_plane_state *plane_state = plane->state;
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect clip;

	if (!fb || !lcdc->crtc.state->active)
		return;
	if (fb->format->format == DRM_FORMAT_D1) {
		s1c33_lcdc_scan(lcdc, drm_fb_dma_get_gem_addr(fb, plane_state, 0),
				fb->pitches[0]);
		return;
	}
	drm_atomic_helper_damage_iter_init(&iter, old_state, plane_state);
	drm_atomic_for_each_plane_damage(&iter, &clip)
		s1c33_lcdc_convert(lcdc, fb, &clip);
	s1c33_lcdc_scan(lcdc, lcdc->image_dma, S1C33_LCD_STRIDE);
}

/*
 * A panic screen is drawn in the controller's own D1 image, whatever
 * the plane was showing: a converted frame's buffer is not what the panel
 * scans, and a client's D1 one may be gone.  Then the panel scans the image,
 * lit, without sleeping.
 */
static int s1c33_lcdc_get_scanout_buffer(struct drm_plane *plane,
					 struct drm_scanout_buffer *sb)
{
	struct s1c33_lcdc *lcdc = to_lcdc(plane->dev);

	sb->format = drm_format_info(DRM_FORMAT_D1);
	sb->width = S1C33_LCD_WIDTH;
	sb->height = S1C33_LCD_HEIGHT;
	sb->pitch[0] = S1C33_LCD_STRIDE;
	iosys_map_set_vaddr(&sb->map[0], lcdc->image);
	return 0;
}

static void s1c33_lcdc_panic_flush(struct drm_plane *plane)
{
	struct s1c33_lcdc *lcdc = to_lcdc(plane->dev);
	u32 ps = readl(lcdc->regs + S1C33_LCDC_PS) & ~S1C33_LCDC_PS_MASK;

	s1c33_lcdc_scan(lcdc, lcdc->image_dma, S1C33_LCD_STRIDE);
	writel(ps | S1C33_LCDC_PS_NORMAL, lcdc->regs + S1C33_LCDC_PS);
	gpiod_set_value(lcdc->enable, 1);
}

static const struct drm_plane_helper_funcs s1c33_lcdc_plane_helper_funcs = {
	.prepare_fb = drm_gem_plane_helper_prepare_fb,
	.atomic_check = s1c33_lcdc_plane_atomic_check,
	.atomic_update = s1c33_lcdc_plane_atomic_update,
	.get_scanout_buffer = s1c33_lcdc_get_scanout_buffer,
	.panic_flush = s1c33_lcdc_panic_flush,
};

static const struct drm_plane_funcs s1c33_lcdc_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = drm_plane_cleanup,
	.reset = drm_atomic_helper_plane_reset,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static enum drm_mode_status
s1c33_lcdc_crtc_mode_valid(struct drm_crtc *crtc,
			   const struct drm_display_mode *mode)
{
	return drm_crtc_helper_mode_valid_fixed(crtc, mode, &s1c33_lcdc_mode);
}

static int s1c33_lcdc_crtc_atomic_check(struct drm_crtc *crtc,
					struct drm_atomic_commit *state)
{
	struct drm_crtc_state *crtc_state =
		drm_atomic_get_new_crtc_state(state, crtc);
	int ret;

	if (crtc_state->enable) {
		ret = drm_atomic_helper_check_crtc_primary_plane(crtc_state);
		if (ret)
			return ret;
	}
	return drm_atomic_add_affected_planes(state, crtc);
}

static void s1c33_lcdc_crtc_atomic_enable(struct drm_crtc *crtc,
					  struct drm_atomic_commit *state)
{
	s1c33_lcdc_power(to_lcdc(crtc->dev), true);
}

static void s1c33_lcdc_crtc_atomic_disable(struct drm_crtc *crtc,
					   struct drm_atomic_commit *state)
{
	s1c33_lcdc_power(to_lcdc(crtc->dev), false);
}

static const struct drm_crtc_helper_funcs s1c33_lcdc_crtc_helper_funcs = {
	.mode_valid = s1c33_lcdc_crtc_mode_valid,
	.atomic_check = s1c33_lcdc_crtc_atomic_check,
	.atomic_enable = s1c33_lcdc_crtc_atomic_enable,
	.atomic_disable = s1c33_lcdc_crtc_atomic_disable,
};

static const struct drm_crtc_funcs s1c33_lcdc_crtc_funcs = {
	.reset = drm_atomic_helper_crtc_reset,
	.destroy = drm_crtc_cleanup,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_crtc_destroy_state,
};

static const struct drm_encoder_funcs s1c33_lcdc_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static int s1c33_lcdc_get_modes(struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &s1c33_lcdc_mode);
}

static const struct drm_connector_helper_funcs s1c33_lcdc_connector_hfuncs = {
	.get_modes = s1c33_lcdc_get_modes,
};

static const struct drm_connector_funcs s1c33_lcdc_connector_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/*
 * Only a converted framebuffer reports damage: a D1 one is scanned where it
 * is, so its clients have nothing to report, and fbdev emulation, if built,
 * maps it as it is rather than keeping a shadow by deferred I/O, which
 * needs page faults.
 */
static struct drm_framebuffer *
s1c33_lcdc_fb_create(struct drm_device *drm, struct drm_file *file,
		     const struct drm_format_info *info,
		     const struct drm_mode_fb_cmd2 *cmd)
{
	if (cmd->pixel_format == DRM_FORMAT_D1)
		return drm_gem_fb_create(drm, file, info, cmd);
	return drm_gem_fb_create_with_dirty(drm, file, info, cmd);
}

static const struct drm_mode_config_funcs s1c33_lcdc_mode_config_funcs = {
	.fb_create = s1c33_lcdc_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/* Lines in whole words, which the controller fetches. */
static int s1c33_lcdc_dumb_create(struct drm_file *file, struct drm_device *drm,
				  struct drm_mode_create_dumb *args)
{
	args->pitch = ALIGN(DIV_ROUND_UP(args->width * args->bpp, 8), 4);
	args->size = 0;
	return drm_gem_dma_dumb_create_internal(file, drm, args);
}

DEFINE_DRM_GEM_DMA_FOPS(s1c33_lcdc_fops);

static const struct drm_driver s1c33_lcdc_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	.name = "s1c33-lcdc",
	.desc = "Epson S1C33 LCD controller",
	.major = 1,
	.minor = 0,
	.fops = &s1c33_lcdc_fops,
	DRM_GEM_DMA_DRIVER_OPS_WITH_DUMB_CREATE(s1c33_lcdc_dumb_create),
	DRM_FBDEV_DMA_DRIVER_OPS,
};

/*
 * The panel shows what the loader left, from wherever the kernel moved it
 * (head.S): copy that into the controller's own image and scan it from
 * there, so the picture stays until a client draws, and the memory it came
 * from can go.  Grifo's lines are the panel's 32 bytes, whatever MLADD
 * reads: it sets that only for a sub-window.
 */
static void s1c33_lcdc_adopt(struct s1c33_lcdc *lcdc)
{
	const size_t size = S1C33_LCD_STRIDE * S1C33_LCD_HEIGHT;
	phys_addr_t at = readl(lcdc->regs + S1C33_LCDC_MADD);

	if (pfn_valid(PHYS_PFN(at)) && pfn_valid(PHYS_PFN(at + size - 1)))
		memcpy(lcdc->image, phys_to_virt(at), size);
	s1c33_lcdc_scan(lcdc, lcdc->image_dma, S1C33_LCD_STRIDE);
}

static int s1c33_lcdc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s1c33_lcdc *lcdc;
	struct drm_device *drm;
	int ret;

	lcdc = devm_drm_dev_alloc(dev, &s1c33_lcdc_driver, struct s1c33_lcdc,
				  drm);
	if (IS_ERR(lcdc))
		return PTR_ERR(lcdc);
	drm = &lcdc->drm;

	lcdc->image = dmam_alloc_coherent(dev,
					  S1C33_LCD_STRIDE * S1C33_LCD_HEIGHT,
					  &lcdc->image_dma, GFP_KERNEL);
	if (!lcdc->image)
		return -ENOMEM;
	lcdc->regs = devm_platform_ioremap_resource_byname(pdev, "lcdc");
	if (IS_ERR(lcdc->regs))
		return PTR_ERR(lcdc->regs);
	s1c33_lcdc_adopt(lcdc);
	/* The panel is already lit: adopt its state rather than glitching it. */
	lcdc->enable = devm_gpiod_get_optional(dev, "enable", GPIOD_ASIS);
	if (IS_ERR(lcdc->enable))
		return dev_err_probe(dev, PTR_ERR(lcdc->enable),
				     "cannot claim the display enable\n");

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.min_width = S1C33_LCD_WIDTH;
	drm->mode_config.max_width = S1C33_LCD_WIDTH;
	drm->mode_config.min_height = S1C33_LCD_HEIGHT;
	drm->mode_config.max_height = S1C33_LCD_HEIGHT;
	drm->mode_config.preferred_depth = 1;
	drm->mode_config.funcs = &s1c33_lcdc_mode_config_funcs;

	/*
	 * One plane on one CRTC, through an encoder that is only wiring, to
	 * the panel.
	 */
	drm_plane_helper_add(&lcdc->plane, &s1c33_lcdc_plane_helper_funcs);
	ret = drm_universal_plane_init(drm, &lcdc->plane, 0,
				       &s1c33_lcdc_plane_funcs,
				       s1c33_lcdc_formats,
				       ARRAY_SIZE(s1c33_lcdc_formats), NULL,
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;
	drm_plane_enable_fb_damage_clips(&lcdc->plane);
	drm_crtc_helper_add(&lcdc->crtc, &s1c33_lcdc_crtc_helper_funcs);
	ret = drm_crtc_init_with_planes(drm, &lcdc->crtc, &lcdc->plane, NULL,
					&s1c33_lcdc_crtc_funcs, NULL);
	if (ret)
		return ret;
	lcdc->encoder.possible_crtcs = drm_crtc_mask(&lcdc->crtc);
	ret = drm_encoder_init(drm, &lcdc->encoder, &s1c33_lcdc_encoder_funcs,
			       DRM_MODE_ENCODER_NONE, NULL);
	if (ret)
		return ret;
	drm_connector_helper_add(&lcdc->connector, &s1c33_lcdc_connector_hfuncs);
	ret = drm_connector_init(drm, &lcdc->connector,
				 &s1c33_lcdc_connector_funcs,
				 DRM_MODE_CONNECTOR_DPI);
	if (ret)
		return ret;
	ret = drm_connector_attach_encoder(&lcdc->connector, &lcdc->encoder);
	if (ret)
		return ret;
	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, drm);
	drm_client_setup_with_fourcc(drm, DRM_FORMAT_D1);
	dev_info(dev, "240x208 one-bit panel, D1 scanned in place, XRGB8888 and RGB565 converted\n");
	return 0;
}

static void s1c33_lcdc_remove(struct platform_device *pdev)
{
	drm_dev_unplug(platform_get_drvdata(pdev));
}

static void s1c33_lcdc_shutdown(struct platform_device *pdev)
{
	drm_atomic_helper_shutdown(platform_get_drvdata(pdev));
}

static const struct of_device_id s1c33_lcdc_of_match[] = {
	{ .compatible = "epson,s1c33-lcdc" },
	{ }
};
MODULE_DEVICE_TABLE(of, s1c33_lcdc_of_match);

static struct platform_driver s1c33_lcdc_platform_driver = {
	.driver = {
		.name = "s1c33-lcdc",
		.of_match_table = s1c33_lcdc_of_match,
	},
	.probe = s1c33_lcdc_probe,
	.remove = s1c33_lcdc_remove,
	.shutdown = s1c33_lcdc_shutdown,
};
module_platform_driver(s1c33_lcdc_platform_driver);

MODULE_DESCRIPTION("Epson S1C33 LCD controller DRM driver");
MODULE_LICENSE("GPL");
