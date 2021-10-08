// SPDX-License-Identifier: GPL-2.0-only
/*
 * Simplest possible spinal frame-buffer driver, as a platform device
 *
 * Copyright (c) 2013, Stephen Warren
 *
 * Based on q40fb.c, which was:
 * Copyright (C) 2001 Richard Zidlicky <rz@linux-m68k.org>
 *
 * Also based on offb.c, which was:
 * Copyright (C) 1997 Geert Uytterhoeven
 * Copyright (C) 1996 Paul Mackerras
 */

#include <linux/errno.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <linux/module.h>
#include "spinalhdlfb.h"
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_clk.h>
#include <linux/of_platform.h>
#include <linux/parser.h>
#include <linux/regulator/consumer.h>
#include <linux/dmaengine.h>

static const struct fb_fix_screeninfo spinalfb_fix = {
	.id = "spinal",
	.type = FB_TYPE_PACKED_PIXELS,
	.visual = FB_VISUAL_TRUECOLOR,
	.accel = FB_ACCEL_NONE,
};

//static const struct fb_var_screeninfo spinalfb_var = {
//    .height     = -1,
//    .width      = -1,
//    .activate   = FB_ACTIVATE_NOW,
//    .vmode      = FB_VMODE_NONINTERLACED,
//};

#define PSEUDO_PALETTE_SIZE 16

struct spinalfb_par {
	void __iomem *base;
	struct clk *clkin;
	struct dma_chan *dma;
	u32 palette[PSEUDO_PALETTE_SIZE];
	u32 dma_running;
};

typedef struct {
	u32 hSyncStart, hSyncEnd;
	u32 hColorStart, hColorEnd;

	u32 vSyncStart, vSyncEnd;
	u32 vColorStart, vColorEnd;

	u32 polarities;
} spinalfb_timing;

static void spinalfb_start(void __iomem *base)
{
	writel(1, base);
}
static void spinalfb_stop(void __iomem *base)
{
	writel(0, base);
}

static void spinalfb_set_timing(void __iomem *base, spinalfb_timing t)
{
	writel(t.hSyncStart, base + 0x40);
	writel(t.hSyncEnd, base + 0x44);
	writel(t.hColorStart, base + 0x48);
	writel(t.hColorEnd, base + 0x4C);
	writel(t.vSyncStart, base + 0x50);
	writel(t.vSyncEnd, base + 0x54);
	writel(t.vColorStart, base + 0x58);
	writel(t.vColorEnd, base + 0x5C);
	writel(t.polarities, base + 0x60);
}

static int spinalfb_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
			      u_int transp, struct fb_info *info)
{
	u32 *pal = info->pseudo_palette;
	u32 cr = red >> (16 - info->var.red.length);
	u32 cg = green >> (16 - info->var.green.length);
	u32 cb = blue >> (16 - info->var.blue.length);
	u32 value;

	if (regno >= PSEUDO_PALETTE_SIZE)
		return -EINVAL;

	value = (cr << info->var.red.offset) | (cg << info->var.green.offset) |
		(cb << info->var.blue.offset);
	if (info->var.transp.length > 0) {
		u32 mask = (1 << info->var.transp.length) - 1;
		mask <<= info->var.transp.offset;
		value |= mask;
	}
	pal[regno] = value;

	return 0;
}

struct spinalfb_par;
static void spinalfb_clocks_destroy(struct spinalfb_par *par);
static void spinalfb_regulators_destroy(struct spinalfb_par *par);

static void spinalfb_destroy(struct fb_info *info)
{
	spinalfb_regulators_destroy(info->par);
	spinalfb_clocks_destroy(info->par);
	if (info->screen_base)
		iounmap(info->screen_base);
}

int spinalfb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	//    dev_info(info->dev, "##### fb_var_screeninfo2 #####\n");

	if (var->bits_per_pixel != 16)
		return -EINVAL;

	if (var->transp.length)
		return -EINVAL;

	//    dev_info(info->dev, "%d %d %d\n", var->xres, var->xres_virtual, var->pixclock);

	var->blue.offset = 0;
	var->green.offset = var->blue.length;
	var->red.offset = var->green.offset + var->green.length;

	return 0;
}

static void spinalfb_dma_complete(void *arg)
{
	//    struct snd_pcm_substream *substream = arg;
	//    struct spinal_lib_audio_out_device *priv = snd_pcm_substream_chip(substream);
	//    //printk("spinal_lib_audio_out_complete\n");
	//
	//    priv->position += snd_pcm_lib_period_bytes(substream);
	//    if (priv->position >= snd_pcm_lib_buffer_bytes(substream))
	//        priv->position = 0;
	//
	//    snd_pcm_period_elapsed(substream);
}

int spinalfb_dma_stop(struct fb_info *info)
{
	struct spinalfb_par *par = info->par;
	if (!par->dma_running)
		return 0;
	dmaengine_terminate_async(par->dma);
	dmaengine_synchronize(par->dma);
	par->dma_running = 0;
	spinalfb_stop(par->base);
	return 0;
}

int spinalfb_dma_start(struct fb_info *info)
{
	struct fb_var_screeninfo *var = &info->var;
	struct spinalfb_par *par = info->par;
	struct dma_async_tx_descriptor *desc;
	unsigned long flags = DMA_CTRL_ACK;
	dma_cookie_t cookie;
	u32 bytes;

	if (par->dma_running)
		spinalfb_dma_stop(info);

	bytes = var->xres_virtual * var->yres_virtual * var->bits_per_pixel / 8;

	desc = dmaengine_prep_dma_cyclic(par->dma, info->fix.smem_start, bytes,
					 bytes, DMA_MEM_TO_DEV, flags);

	if (!desc) {
		dev_info(info->dev, "Can't create DMA descriptor\n");
		return -ENOMEM;
	}

	desc->callback = spinalfb_dma_complete;
	desc->callback_param = info;
	cookie = desc->tx_submit(desc);
	if (dma_submit_error(cookie)) {
		dev_info(info->dev, "Can't submit DMA\n");
		return cookie;
	}

	dma_async_issue_pending(par->dma);

	par->dma_running = 1;
	spinalfb_start(par->base);

	return 0;
}

/* set the video mode according to info->var */
int spinalfb_set_par(struct fb_info *info)
{
	struct fb_var_screeninfo *var = &info->var;
	struct spinalfb_par *par = info->par;
	spinalfb_timing timing;
	int err;
	int xoff, yoff;

	dev_info(info->dev, "set resolution as %d x %d\n", var->xres,
		 var->yres);
	//    dev_info(info->dev, "%d %d %d\n", var->xres, var->xres_virtual, var->pixclock);
	//    dev_info(info->dev, "%d %d\n", var->hsync_len, var->vsync_len);
	//    dev_info(info->dev, "%d %d %d %d\n", var->left_margin, var->right_margin, var->upper_margin, var->lower_margin);
	//    dev_info(info->dev, "%d\n",(int) info->mode);
	//    if(info->mode){
	//        dev_info(info->dev, "%d %d %d %d\n", info->mode->pixclock, info->mode->right_margin, info->mode->upper_margin, info->mode->lower_margin);
	//    }

	if (var->pixclock) {
		if ((err = clk_set_rate(par->clkin, div_u64(1000000000000ull,
							    var->pixclock)))) {
			dev_err(info->dev, "Can't set clock\n");
			return err;
		}
	}

	info->fix.line_length =
		(info->var.xres_virtual * info->var.bits_per_pixel) / 8;

	timing.hSyncStart = var->hsync_len - 1;
	timing.hColorStart = timing.hSyncStart + var->left_margin;
	timing.hColorEnd = timing.hColorStart + var->xres;
	timing.hSyncEnd = timing.hColorEnd + var->right_margin;
	timing.vSyncStart = var->vsync_len - 1;
	timing.vColorStart = timing.vSyncStart + var->upper_margin;
	timing.vColorEnd = timing.vColorStart + var->yres;
	timing.vSyncEnd = timing.vColorEnd + var->lower_margin;
	timing.polarities = (var->sync & FB_SYNC_HOR_HIGH_ACT ? 1 : 0) |
			    (var->sync & FB_SYNC_VERT_HIGH_ACT ? 2 : 0);

	xoff = (var->xres - var->xres_virtual) / 2;
	yoff = (var->yres - var->yres_virtual) / 2;
	timing.hColorStart += xoff;
	timing.hColorEnd -= xoff;
	timing.vColorStart += yoff;
	timing.vColorEnd -= yoff;

	spinalfb_set_timing(par->base, timing);

	spinalfb_dma_stop(info);

	return spinalfb_dma_start(info);
}

///* perform fb specific ioctl (optional) */
//int spinalfb_ioctl(struct fb_info *info, unsigned int cmd, unsigned long arg){
//    dev_info(info->dev, "##### spinalfb_ioctl #####\n");
//    return 0;
//}
//
///* Handle 32bit compat ioctl (optional) */
//int spinalfb_compat_ioctl(struct fb_info *info, unsigned cmd, unsigned long arg){
//    dev_info(info->dev, "##### spinalfb_compat_ioctl #####\n");
//    return 0;
//}

//int spinalfb_pan_display(struct fb_var_screeninfo *var, struct fb_info *info){
//    return 0;
//}

static const struct fb_ops spinalfb_ops = {
	.owner = THIS_MODULE,
	.fb_destroy = spinalfb_destroy,
	.fb_setcolreg = spinalfb_setcolreg,
	.fb_fillrect = cfb_fillrect,
	.fb_copyarea = cfb_copyarea,
	.fb_imageblit = cfb_imageblit,
	.fb_check_var = spinalfb_check_var,
	.fb_set_par = spinalfb_set_par,
	//    .fb_pan_display = spinalfb_pan_display
	//    .fb_ioctl       = spinalfb_ioctl,
	//    .fb_compat_ioctl = spinalfb_compat_ioctl
};

static struct spinalfb_format spinalfb_formats[] = SPINALFB_FORMATS;

struct spinalfb_params {
	struct spinalfb_format *format;
};

static int spinalfb_parse_dt(struct platform_device *pdev,
			     struct spinalfb_params *params)
{
	struct device_node *np = pdev->dev.of_node;
	int ret;
	const char *format;
	int i;

	ret = of_property_read_string(np, "format", &format);
	if (ret) {
		dev_err(&pdev->dev, "Can't parse format property\n");
		return ret;
	}
	params->format = NULL;
	for (i = 0; i < ARRAY_SIZE(spinalfb_formats); i++) {
		if (strcmp(format, spinalfb_formats[i].name))
			continue;
		params->format = &spinalfb_formats[i];
		break;
	}
	if (!params->format) {
		dev_err(&pdev->dev, "Invalid format value\n");
		return -EINVAL;
	}

	return 0;
}

static int spinalfb_parse_pd(struct platform_device *pdev,
			     struct spinalfb_params *params)
{
	struct spinalfb_platform_data *pd = dev_get_platdata(&pdev->dev);
	int i;

	params->format = NULL;
	for (i = 0; i < ARRAY_SIZE(spinalfb_formats); i++) {
		if (strcmp(pd->format, spinalfb_formats[i].name))
			continue;

		params->format = &spinalfb_formats[i];
		break;
	}

	if (!params->format) {
		dev_err(&pdev->dev, "Invalid format value\n");
		return -EINVAL;
	}

	return 0;
}

static int spinalfb_clocks_get(struct spinalfb_par *par,
			       struct platform_device *pdev)
{
	par->clkin = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(par->clkin)) {
		dev_info(&pdev->dev, "Missing input clock\n");
		return -EINVAL;
	}

	return 0;
}
static void spinalfb_clocks_enable(struct spinalfb_par *par,
				   struct platform_device *pdev)
{
	int ret;
	ret = clk_prepare_enable(par->clkin);
	if (ret) {
		dev_err(&pdev->dev, "%s: failed to enable clock : %d\n",
			__func__, ret);
		clk_put(par->clkin);
		par->clkin = NULL;
	}
}
static void spinalfb_clocks_destroy(struct spinalfb_par *par)
{
}

#if defined CONFIG_OF && defined CONFIG_REGULATOR

#define SUPPLY_SUFFIX "-supply"

/*
 * Regulator handling code.
 *
 * Here we handle the num-supplies and vin*-supply properties of our
 * "spinal-framebuffer" dt node. This is necessary so that we can make sure
 * that any regulators needed by the display hardware that the bootloader
 * set up for us (and for which it provided a spinalfb dt node), stay up,
 * for the life of the spinalfb driver.
 *
 * When the driver unloads, we cleanly disable, and then release the
 * regulators.
 *
 * We only complain about errors here, no action is taken as the most likely
 * error can only happen due to a mismatch between the bootloader which set
 * up spinalfb, and the regulator definitions in the device tree. Chances are
 * that there are no adverse effects, and if there are, a clean teardown of
 * the fb probe will not help us much either. So just complain and carry on,
 * and hope that the user actually gets a working fb at the end of things.
 */
static int spinalfb_regulators_get(struct spinalfb_par *par,
				   struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct property *prop;
	struct regulator *regulator;
	const char *p;
	int count = 0, i = 0;

	if (dev_get_platdata(&pdev->dev) || !np)
		return 0;

	/* Count the number of regulator supplies */
	for_each_property_of_node (np, prop) {
		p = strstr(prop->name, SUPPLY_SUFFIX);
		if (p && p != prop->name)
			count++;
	}

	if (!count)
		return 0;

	par->regulators = devm_kcalloc(&pdev->dev, count,
				       sizeof(struct regulator *), GFP_KERNEL);
	if (!par->regulators)
		return -ENOMEM;

	/* Get all the regulators */
	for_each_property_of_node (np, prop) {
		char name[32]; /* 32 is max size of property name */

		p = strstr(prop->name, SUPPLY_SUFFIX);
		if (!p || p == prop->name)
			continue;

		strlcpy(name, prop->name,
			strlen(prop->name) - strlen(SUPPLY_SUFFIX) + 1);
		regulator = devm_regulator_get_optional(&pdev->dev, name);
		if (IS_ERR(regulator)) {
			if (PTR_ERR(regulator) == -EPROBE_DEFER)
				return -EPROBE_DEFER;
			dev_err(&pdev->dev, "regulator %s not found: %ld\n",
				name, PTR_ERR(regulator));
			continue;
		}
		par->regulators[i++] = regulator;
	}
	par->regulator_count = i;

	return 0;
}

static void spinalfb_regulators_enable(struct spinalfb_par *par,
				       struct platform_device *pdev)
{
	int i, ret;

	/* Enable all the regulators */
	for (i = 0; i < par->regulator_count; i++) {
		ret = regulator_enable(par->regulators[i]);
		if (ret) {
			dev_err(&pdev->dev,
				"failed to enable regulator %d: %d\n", i, ret);
			devm_regulator_put(par->regulators[i]);
			par->regulators[i] = NULL;
		}
	}
	par->regulators_enabled = true;
}

static void spinalfb_regulators_destroy(struct spinalfb_par *par)
{
	int i;

	if (!par->regulators || !par->regulators_enabled)
		return;

	for (i = 0; i < par->regulator_count; i++)
		if (par->regulators[i])
			regulator_disable(par->regulators[i]);
}
#else
static int spinalfb_regulators_get(struct spinalfb_par *par,
				   struct platform_device *pdev)
{
	return 0;
}
static void spinalfb_regulators_enable(struct spinalfb_par *par,
				       struct platform_device *pdev)
{
}
static void spinalfb_regulators_destroy(struct spinalfb_par *par)
{
}
#endif

static int spinalfb_probe(struct platform_device *pdev)
{
	int ret;
	struct spinalfb_params params;
	struct fb_info *info;
	struct spinalfb_par *par;
	struct resource *mem, *reg;
	const char *mode_option;

	if (fb_get_options("spinalfb", (char **)&mode_option))
		return -ENODEV;

	ret = -ENODEV;
	if (dev_get_platdata(&pdev->dev))
		ret = spinalfb_parse_pd(pdev, &params);
	else if (pdev->dev.of_node)
		ret = spinalfb_parse_dt(pdev, &params);

	if (ret)
		return ret;

	mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem) {
		dev_err(&pdev->dev, "No memory resource\n");
		return -EINVAL;
	}

	reg = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!reg) {
		dev_err(&pdev->dev, "No vga reg resource\n");
		return -ENXIO;
	}

	info = framebuffer_alloc(sizeof(struct spinalfb_par), &pdev->dev);
	if (!info)
		return -ENOMEM;
	platform_set_drvdata(pdev, info);

	par = info->par;
	par->dma_running = 0;

	par->base = devm_ioremap_resource(&pdev->dev, reg);
	if (IS_ERR(par->base)) {
		dev_err(&pdev->dev, "Can't ioremap reg\n");
		goto error_fb_release;
	}

	par->dma = dma_request_chan(&pdev->dev, "stream");
	if (IS_ERR_OR_NULL(par->dma)) {
		dev_err(&pdev->dev, "missing DMA ?\n");
		goto error_fb_release;
	}

	if (!mode_option) {
		ret = of_property_read_string(pdev->dev.of_node, "mode",
					      &mode_option);
		if (ret) {
			mode_option = "640x480@60m";
		}
	}

	info->fbops = &spinalfb_ops;
	info->flags = FBINFO_DEFAULT | FBINFO_MISC_FIRMWARE;

	info->fix = spinalfb_fix;
	info->fix.smem_start = mem->start;
	info->fix.smem_len = resource_size(mem);

	if (!fb_find_mode(&info->var, info, mode_option, NULL, 0, NULL, 16)) {
		dev_err(&pdev->dev, "No valid video modes found\n");
		return -EINVAL;
	}

	info->apertures = alloc_apertures(1);
	if (!info->apertures) {
		ret = -ENOMEM;
		goto error_fb_release;
	}
	info->apertures->ranges[0].base = info->fix.smem_start;
	info->apertures->ranges[0].size = info->fix.smem_len;

	info->screen_base =
		ioremap_wc(info->fix.smem_start, info->fix.smem_len);
	if (!info->screen_base) {
		ret = -ENOMEM;
		goto error_fb_release;
	}
	info->pseudo_palette = par->palette;

	ret = spinalfb_clocks_get(par, pdev);
	if (ret < 0)
		goto error_unmap;

	ret = spinalfb_regulators_get(par, pdev);
	if (ret < 0)
		goto error_clocks;

	spinalfb_clocks_enable(par, pdev);
	spinalfb_regulators_enable(par, pdev);

	dev_info(&pdev->dev,
		 "framebuffer at 0x%lx, 0x%x bytes, mapped to 0x%p\n",
		 info->fix.smem_start, info->fix.smem_len, info->screen_base);
	dev_info(&pdev->dev, "format=%s, mode=%dx%dx%d, linelength=%d\n",
		 params.format->name, info->var.xres, info->var.yres,
		 info->var.bits_per_pixel, info->fix.line_length);

	ret = register_framebuffer(info);
	if (ret < 0) {
		dev_err(&pdev->dev, "Unable to register spinalfb: %d\n", ret);
		goto error_regulators;
	}

	dev_info(&pdev->dev, "fb%d: spinalfb registered!\n", info->node);

	info->fbops->fb_set_par(info);
	return 0;

error_regulators:
	spinalfb_regulators_destroy(par);
error_clocks:
	spinalfb_clocks_destroy(par);
error_unmap:
	iounmap(info->screen_base);
error_fb_release:
	framebuffer_release(info);
	return ret;
}

static int spinalfb_remove(struct platform_device *pdev)
{
	struct fb_info *info = platform_get_drvdata(pdev);

	unregister_framebuffer(info);
	framebuffer_release(info);

	return 0;
}

static const struct of_device_id spinalfb_of_match[] = {
	{
		.compatible = "spinal-framebuffer",
	},
	{
		.compatible = "spinalhdl,framebuffer-1.0",
	},
	{},
};
MODULE_DEVICE_TABLE(of, spinalfb_of_match);

static struct platform_driver spinalfb_driver = {
    .driver = {
        .name = "spinal-framebuffer",
        .of_match_table = spinalfb_of_match,
    },
    .probe = spinalfb_probe,
    .remove = spinalfb_remove,
};

//module_platform_driver(spinalfb_driver);

static int __init spinalfb_init(void)
{
	int ret;
	struct device_node *np;

	ret = platform_driver_register(&spinalfb_driver);
	if (ret)
		return ret;

	if (IS_ENABLED(CONFIG_OF_ADDRESS) && of_chosen) {
		for_each_child_of_node (of_chosen, np) {
			if (of_device_is_compatible(np, "spinal-framebuffer") ||
			    of_device_is_compatible(
				    np, "spinalhdl,framebuffer-1.0"))
				of_platform_device_create(np, NULL, NULL);
		}
	}

	return 0;
}

fs_initcall(spinalfb_init);

MODULE_AUTHOR("Stephen Warren <swarren@wwwdotorg.org>");
MODULE_DESCRIPTION("Simple framebuffer driver");
MODULE_LICENSE("GPL v2");
