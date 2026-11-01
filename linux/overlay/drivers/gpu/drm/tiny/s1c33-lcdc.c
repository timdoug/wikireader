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
 * it is, so fbdev emulation's buffer, which programs write through mmap
 * with no way to say what changed on a machine without an MMU, is always on
 * the panel.  An XRGB8888 or RGB565 framebuffer, which is what modern
 * clients draw, is converted where damaged into the controller's own D1
 * buffer in internal video RAM, which is then scanned.
 */
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/linux_logo.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_damage_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

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
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	void __iomem *regs;
	struct gpio_desc *enable;
	u8 __iomem *vram;		/* the converted image, in IVRAM */
	phys_addr_t vram_phys;
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

/* Dark below half brightness, by the usual luma weights. */
static inline bool s1c33_dark(unsigned int r, unsigned int g, unsigned int b)
{
	return r * 77 + g * 150 + b * 29 < 128 * 256;
}

/* Convert the damaged rectangle of an XRGB8888 or RGB565 frame to D1. */
static void s1c33_lcdc_convert(struct s1c33_lcdc *lcdc,
			       struct drm_framebuffer *fb,
			       const struct drm_rect *clip)
{
	struct drm_gem_dma_object *gem = drm_fb_dma_get_gem_obj(fb, 0);
	const u8 *src = (const u8 *)gem->vaddr + fb->offsets[0];
	bool rgb565 = fb->format->format == DRM_FORMAT_RGB565;
	unsigned int x0 = clip->x1 & ~7, x1 = min_t(int, ALIGN(clip->x2, 8), fb->width);
	unsigned int x, y;

	for (y = clip->y1; y < clip->y2; y++) {
		const u8 *line = src + y * fb->pitches[0];
		u8 __iomem *out = lcdc->vram + y * S1C33_LCD_STRIDE;

		for (x = x0; x < x1; x += 8) {
			unsigned int bit, n = min(8U, x1 - x);
			u8 byte = 0;

			for (bit = 0; bit < n; bit++) {
				unsigned int r, g, b;

				if (rgb565) {
					u16 p = ((const u16 *)line)[x + bit];

					r = (p >> 8) & 0xf8;
					g = (p >> 3) & 0xfc;
					b = (p << 3) & 0xf8;
				} else {
					u32 p = ((const u32 *)line)[x + bit];

					r = (p >> 16) & 0xff;
					g = (p >> 8) & 0xff;
					b = p & 0xff;
				}
				if (s1c33_dark(r, g, b))
					byte |= 0x80 >> bit;
			}
			writeb(byte, out + x / 8);
		}
	}
}

static int s1c33_lcdc_check(struct drm_simple_display_pipe *pipe,
			    struct drm_plane_state *plane_state,
			    struct drm_crtc_state *crtc_state)
{
	struct drm_framebuffer *fb = plane_state->fb;

	if (!fb || fb->format->format != DRM_FORMAT_D1)
		return 0;
	/* Scanned as it is: from a word, in whole words a line. */
	if (fb->pitches[0] % 4 || (plane_state->src_x >> 16) % 32 ||
	    (drm_fb_dma_get_gem_addr(fb, plane_state, 0) & 3))
		return -EINVAL;
	return 0;
}

static void s1c33_lcdc_update(struct drm_simple_display_pipe *pipe,
			      struct drm_plane_state *old_state)
{
	struct s1c33_lcdc *lcdc = to_lcdc(pipe->crtc.dev);
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_framebuffer *fb = state->fb;
	struct drm_atomic_helper_damage_iter iter;
	struct drm_rect clip;

	if (!fb || !pipe->crtc.state->active)
		return;
	if (fb->format->format == DRM_FORMAT_D1) {
		s1c33_lcdc_scan(lcdc, drm_fb_dma_get_gem_addr(fb, state, 0),
				fb->pitches[0]);
		return;
	}
	drm_atomic_helper_damage_iter_init(&iter, old_state, state);
	drm_atomic_for_each_plane_damage(&iter, &clip)
		s1c33_lcdc_convert(lcdc, fb, &clip);
	s1c33_lcdc_scan(lcdc, lcdc->vram_phys, S1C33_LCD_STRIDE);
}

static void s1c33_lcdc_enable(struct drm_simple_display_pipe *pipe,
			      struct drm_crtc_state *crtc_state,
			      struct drm_plane_state *plane_state)
{
	struct s1c33_lcdc *lcdc = to_lcdc(pipe->crtc.dev);

	s1c33_lcdc_power(lcdc, true);
	s1c33_lcdc_update(pipe, plane_state);
}

static void s1c33_lcdc_disable(struct drm_simple_display_pipe *pipe)
{
	s1c33_lcdc_power(to_lcdc(pipe->crtc.dev), false);
}

static const struct drm_simple_display_pipe_funcs s1c33_lcdc_pipe_funcs = {
	.check = s1c33_lcdc_check,
	.enable = s1c33_lcdc_enable,
	.disable = s1c33_lcdc_disable,
	.update = s1c33_lcdc_update,
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
 * Only a converted framebuffer reports damage.  fbdev emulation gives one
 * that does a shadow kept by deferred I/O, which needs page faults; a D1
 * one it maps as it is, which needs none.
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
 * The kernel's monochrome Tux on fbdev emulation's image, 16 rows down at the
 * low-address edge, which the panel's wiring puts at its physical right.
 */
static bool s1c33_lcdc_show_logo(struct drm_device *drm)
{
	const struct linux_logo *logo = fb_find_logo(1);
	struct fb_info *info = drm->fb_helper ? drm->fb_helper->info : NULL;
	unsigned int stride, x, y;
	u8 *screen;

	if (!logo || logo->type != LINUX_LOGO_MONO || !info ||
	    !info->screen_buffer || logo->width > S1C33_LCD_WIDTH ||
	    16 + logo->height > S1C33_LCD_HEIGHT)
		return false;
	screen = info->screen_buffer;
	stride = DIV_ROUND_UP(logo->width, 8);
	for (y = 0; y < logo->height; y++)
		for (x = 0; x < stride; x++)
			screen[(16 + y) * info->fix.line_length + x] =
				~logo->data[y * stride + x];
	return true;
}

static int s1c33_lcdc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s1c33_lcdc *lcdc;
	struct resource *vram;
	struct drm_device *drm;
	int ret;

	lcdc = devm_drm_dev_alloc(dev, &s1c33_lcdc_driver, struct s1c33_lcdc,
				  drm);
	if (IS_ERR(lcdc))
		return PTR_ERR(lcdc);
	drm = &lcdc->drm;

	vram = platform_get_resource_byname(pdev, IORESOURCE_MEM, "vram");
	if (!vram || resource_size(vram) < S1C33_LCD_STRIDE * S1C33_LCD_HEIGHT)
		return dev_err_probe(dev, -EINVAL, "no room for the panel's image\n");
	lcdc->vram = devm_ioremap_resource(dev, vram);
	if (IS_ERR(lcdc->vram))
		return PTR_ERR(lcdc->vram);
	lcdc->vram_phys = vram->start;
	lcdc->regs = devm_platform_ioremap_resource_byname(pdev, "lcdc");
	if (IS_ERR(lcdc->regs))
		return PTR_ERR(lcdc->regs);
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

	drm_connector_helper_add(&lcdc->connector, &s1c33_lcdc_connector_hfuncs);
	ret = drm_connector_init(drm, &lcdc->connector,
				 &s1c33_lcdc_connector_funcs,
				 DRM_MODE_CONNECTOR_DPI);
	if (ret)
		return ret;
	ret = drm_simple_display_pipe_init(drm, &lcdc->pipe,
					   &s1c33_lcdc_pipe_funcs,
					   s1c33_lcdc_formats,
					   ARRAY_SIZE(s1c33_lcdc_formats),
					   NULL, &lcdc->connector);
	if (ret)
		return ret;
	drm_plane_enable_fb_damage_clips(&lcdc->pipe.plane);
	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, drm);
	drm_client_setup_with_fourcc(drm, DRM_FORMAT_D1);
	/*
	 * The panel is the boot display, so show fbdev emulation's image now:
	 * without fbcon to set its mode, nothing else would until a program
	 * did, and those that write /dev/fb0 directly never do.
	 */
	if (drm->fb_helper)
		drm_fb_helper_restore_fbdev_mode_unlocked(drm->fb_helper, false);
	dev_info(dev, "240x208 one-bit panel, D1 scanned in place, XRGB8888 and RGB565 converted%s\n",
		 s1c33_lcdc_show_logo(drm) ? "; Tux logo shown" : "");
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
