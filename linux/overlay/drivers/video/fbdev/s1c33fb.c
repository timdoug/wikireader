// SPDX-License-Identifier: GPL-2.0-only
/* Framebuffer view of the WikiReader LCD memory configured by the loader. */
#include <linux/fb.h>
#include <linux/io.h>
#include <linux/linux_logo.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define S1C33_FB_WIDTH  240
#define S1C33_FB_HEIGHT 208
#define S1C33_FB_STRIDE 32

static const struct fb_fix_screeninfo s1c33fb_fix = {
	.id = "s1c33-lcd",
	.type = FB_TYPE_PACKED_PIXELS,
	.visual = FB_VISUAL_MONO01,
	.line_length = S1C33_FB_STRIDE,
	.accel = FB_ACCEL_NONE,
};

static const struct fb_var_screeninfo s1c33fb_var = {
	.xres = S1C33_FB_WIDTH,
	.yres = S1C33_FB_HEIGHT,
	.xres_virtual = S1C33_FB_WIDTH,
	.yres_virtual = S1C33_FB_HEIGHT,
	.bits_per_pixel = 1,
	.activate = FB_ACTIVATE_NOW,
	.height = -1,
	.width = -1,
	.vmode = FB_VMODE_NONINTERLACED,
};

static const struct fb_ops s1c33fb_ops = {
	.owner = THIS_MODULE,
	FB_DEFAULT_IOMEM_OPS,
};

static bool s1c33fb_show_boot_logo(struct fb_info *info)
{
	const struct linux_logo *logo = fb_find_logo(1);
	unsigned int source_stride;
	unsigned int destination_x;
	unsigned int destination_y = 16;
	unsigned int x;
	unsigned int y;

	if (!logo || logo->type != LINUX_LOGO_MONO ||
	    logo->width > S1C33_FB_WIDTH ||
	    destination_y + logo->height > S1C33_FB_HEIGHT)
		return false;
	/* The panel wiring puts the low-address edge at physical screen right. */
	destination_x = 0;
	if (destination_x & 7)
		return false;
	source_stride = DIV_ROUND_UP(logo->width, 8);
	for (y = 0; y < logo->height; y++)
		for (x = 0; x < source_stride; x++)
			writeb(~logo->data[y * source_stride + x],
			       info->screen_base +
			       (destination_y + y) * S1C33_FB_STRIDE +
			       destination_x / 8 + x);
	return true;
}

static int s1c33fb_probe(struct platform_device *pdev)
{
	struct resource *resource;
	struct fb_info *info;
	void __iomem *screen;
	int ret;

	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource)
		return -EINVAL;
	screen = devm_ioremap_resource(&pdev->dev, resource);
	if (IS_ERR(screen))
		return dev_err_probe(&pdev->dev, PTR_ERR(screen),
				     "cannot map framebuffer\n");

	info = framebuffer_alloc(0, &pdev->dev);
	if (!info)
		return -ENOMEM;
	info->fbops = &s1c33fb_ops;
	info->fix = s1c33fb_fix;
	info->fix.smem_start = resource->start;
	info->fix.smem_len = resource_size(resource);
	info->var = s1c33fb_var;
	info->screen_base = screen;
	info->screen_size = resource_size(resource);

	ret = register_framebuffer(info);
	if (ret) {
		framebuffer_release(info);
		return dev_err_probe(&pdev->dev, ret,
				     "cannot register framebuffer\n");
	}
	platform_set_drvdata(pdev, info);
	if (s1c33fb_show_boot_logo(info))
		dev_info(&pdev->dev,
			 "registered /dev/fb%d, 240x208 mono; Tux logo shown\n",
			 info->node);
	else
		dev_info(&pdev->dev, "registered /dev/fb%d, 240x208 mono\n",
			 info->node);
	return 0;
}

static void s1c33fb_remove(struct platform_device *pdev)
{
	struct fb_info *info = platform_get_drvdata(pdev);

	unregister_framebuffer(info);
	framebuffer_release(info);
}

static struct platform_driver s1c33fb_driver = {
	.driver.name = "s1c33-fb",
	.probe = s1c33fb_probe,
	.remove = s1c33fb_remove,
};
module_platform_driver(s1c33fb_driver);

MODULE_DESCRIPTION("Epson S1C33 WikiReader framebuffer");
MODULE_LICENSE("GPL");
