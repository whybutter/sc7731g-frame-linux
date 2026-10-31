// SPDX-License-Identifier: GPL-2.0-only
/*
 * DRM driver for the SC7731G display controller (DISPC, the vendor's
 * sprdfb), as a takeover of the display U-Boot leaves running.
 *
 * Why this exists before a real display driver: simpledrm can show pixels
 * but cannot import a buffer from another device, so lima (Mesa's Mali-400
 * driver, through kmsro) has nothing to render into. This driver is the
 * smallest KMS device that can: GEM DMA buffers, PRIME import, page flips on
 * the hardware's vsync. It does NOT power the panel or program the DSI host
 * yet -- the vendor U-Boot has already done both (it shows its splash), and
 * this driver only moves DISPC's OSD layer to our buffers. A DSI host + panel
 * driver for the 0x936500 panel is the next step; until then the mode is
 * whatever U-Boot set, read back from DISPC_SIZE_XY at probe.
 *
 * Register use is the vendor's drivers/video/sprdfb/sprdfb_dispc.c (Samsung
 * SM-G531H GPL kernel, 3.10.65), DPI (video mode) path:
 *
 *  - The panel runs from the OSD layer: OSD_BASE_ADDR, OSD_PITCH (in pixels),
 *    OSD_SIZE_XY, OSD_CTRL (bit 0 enable, bit 2 block alpha, format in 7:4:
 *    3 = 32 bpp, 5 = RGB565; bit 15 swaps R and B). The vendor's fbdev says
 *    format 3 with the swap is red at bit 16, i.e. DRM's XRGB8888. It isn't:
 *    with the swap on for XRGB8888 we saw the pomodoro app's red
 *    tomato come out blue (2026-10-28; cage scans out XR24). Without it,
 *    format 3 is XRGB8888, so the swap is for the BGR formats.
 *  - Registers are shadowed. In video mode, setting DPI_CTRL bit 5 asks for
 *    the shadow copy at the next vsync (dispc_run); INT bit 4 (update done)
 *    and bit 5 (DPI vsync) follow.
 *  - DISPC_CTRL bit 4 is "refresh running"; U-Boot leaves it set. Stopping
 *    it in video mode would starve the DSI host, so disable only hides the
 *    layer (the background color, black, shows).
 */

#include <linux/aperture.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <drm/clients/drm_client_setup.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_vblank.h>

#define DISPC_CTRL		0x00
#define  CTRL_REFRESH		BIT(4)
#define DISPC_SIZE_XY		0x04
#define DISPC_OSD_CTRL		0x40
#define  OSD_EN			BIT(0)
#define  OSD_BLOCK_ALPHA	BIT(2)
#define  OSD_FMT_32BPP		(3 << 4)
#define  OSD_RB_SWITCH		BIT(15)
#define DISPC_OSD_BASE_ADDR	0x44
#define DISPC_OSD_SIZE_XY	0x48
#define DISPC_OSD_PITCH		0x4c
#define DISPC_OSD_DISP_XY	0x50
#define DISPC_OSD_ALPHA		0x54
#define DISPC_INT_EN		0x70
#define DISPC_INT_CLR		0x74
#define DISPC_INT_STATUS	0x78
#define  INT_ERR		BIT(2)
#define  INT_UPDATE_DONE	BIT(4)
#define  INT_DPI_VSYNC		BIT(5)
#define DISPC_DPI_CTRL		0x80
#define  DPI_REG_UPDATE		BIT(5)
#define DISPC_DPI_H_TIMING	0x84
#define DISPC_DPI_V_TIMING	0x88

struct sc7731g_dispc {
	struct drm_device drm;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct drm_display_mode mode;
	void __iomem *base;
	int irq;
};

#define to_dispc(d) container_of(d, struct sc7731g_dispc, drm)

static const u32 sc7731g_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_ABGR8888,
};

static int sc7731g_conn_get_modes(struct drm_connector *connector)
{
	struct sc7731g_dispc *d = to_dispc(connector->dev);

	return drm_connector_helper_get_modes_fixed(connector, &d->mode);
}

static const struct drm_connector_helper_funcs sc7731g_conn_helpers = {
	.get_modes = sc7731g_conn_get_modes,
};

static const struct drm_connector_funcs sc7731g_conn_funcs = {
	.reset = drm_atomic_helper_connector_reset,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = drm_connector_cleanup,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static void sc7731g_show(struct sc7731g_dispc *d, struct drm_plane_state *ps)
{
	struct drm_framebuffer *fb = ps->fb;
	u32 ctrl = OSD_EN | OSD_BLOCK_ALPHA | OSD_FMT_32BPP;

	if (fb->format->format == DRM_FORMAT_XBGR8888 ||
	    fb->format->format == DRM_FORMAT_ABGR8888)
		ctrl |= OSD_RB_SWITCH;

	writel(drm_fb_dma_get_gem_addr(fb, ps, 0), d->base + DISPC_OSD_BASE_ADDR);
	writel(fb->pitches[0] / 4, d->base + DISPC_OSD_PITCH);
	writel((fb->width & 0xfff) | ((fb->height & 0xfff) << 16),
	       d->base + DISPC_OSD_SIZE_XY);
	writel(0, d->base + DISPC_OSD_DISP_XY);
	writel(0xff, d->base + DISPC_OSD_ALPHA);
	writel(ctrl, d->base + DISPC_OSD_CTRL);
	writel(readl(d->base + DISPC_DPI_CTRL) | DPI_REG_UPDATE,
	       d->base + DISPC_DPI_CTRL);
}

static void sc7731g_send_event(struct drm_simple_display_pipe *pipe)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_pending_vblank_event *event = crtc->state->event;

	if (!event)
		return;
	crtc->state->event = NULL;
	spin_lock_irq(&crtc->dev->event_lock);
	if (drm_crtc_vblank_get(crtc) == 0)
		drm_crtc_arm_vblank_event(crtc, event);
	else
		drm_crtc_send_vblank_event(crtc, event);
	spin_unlock_irq(&crtc->dev->event_lock);
}

static void sc7731g_pipe_enable(struct drm_simple_display_pipe *pipe,
				struct drm_crtc_state *cs,
				struct drm_plane_state *ps)
{
	struct sc7731g_dispc *d = to_dispc(pipe->crtc.dev);

	if (!(readl(d->base + DISPC_CTRL) & CTRL_REFRESH))
		drm_warn(&d->drm, "DISPC is not refreshing: no panel until there is a DSI driver\n");
	sc7731g_show(d, ps);
	drm_crtc_vblank_on(&pipe->crtc);
	sc7731g_send_event(pipe);
}

static void sc7731g_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct sc7731g_dispc *d = to_dispc(pipe->crtc.dev);

	drm_crtc_vblank_off(&pipe->crtc);
	writel(readl(d->base + DISPC_OSD_CTRL) & ~OSD_EN, d->base + DISPC_OSD_CTRL);
	writel(readl(d->base + DISPC_DPI_CTRL) | DPI_REG_UPDATE,
	       d->base + DISPC_DPI_CTRL);
}

static void sc7731g_pipe_update(struct drm_simple_display_pipe *pipe,
				struct drm_plane_state *old)
{
	struct sc7731g_dispc *d = to_dispc(pipe->crtc.dev);
	struct drm_plane_state *ps = pipe->plane.state;

	if (ps->fb && pipe->crtc.state->active)
		sc7731g_show(d, ps);
	sc7731g_send_event(pipe);
}

static int sc7731g_enable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct sc7731g_dispc *d = to_dispc(pipe->crtc.dev);

	writel(INT_DPI_VSYNC, d->base + DISPC_INT_CLR);
	writel(readl(d->base + DISPC_INT_EN) | INT_DPI_VSYNC, d->base + DISPC_INT_EN);
	return 0;
}

static void sc7731g_disable_vblank(struct drm_simple_display_pipe *pipe)
{
	struct sc7731g_dispc *d = to_dispc(pipe->crtc.dev);

	writel(readl(d->base + DISPC_INT_EN) & ~INT_DPI_VSYNC, d->base + DISPC_INT_EN);
}

static const struct drm_simple_display_pipe_funcs sc7731g_pipe_funcs = {
	.enable = sc7731g_pipe_enable,
	.disable = sc7731g_pipe_disable,
	.update = sc7731g_pipe_update,
	.enable_vblank = sc7731g_enable_vblank,
	.disable_vblank = sc7731g_disable_vblank,
};

static irqreturn_t sc7731g_irq(int irq, void *arg)
{
	struct sc7731g_dispc *d = arg;
	u32 st = readl(d->base + DISPC_INT_STATUS);

	if (!st)
		return IRQ_NONE;
	writel(st, d->base + DISPC_INT_CLR);
	if (st & INT_DPI_VSYNC)
		drm_crtc_handle_vblank(&d->pipe.crtc);
	if (st & INT_ERR) {
		/* underflow; the vendor logs it once and masks it too */
		drm_warn_once(&d->drm, "DISPC underflow\n");
		writel(readl(d->base + DISPC_INT_EN) & ~INT_ERR, d->base + DISPC_INT_EN);
	}
	return IRQ_HANDLED;
}

/*
 * The mode U-Boot set. DISPC_SIZE_XY is the active area. In video mode the
 * DPI timing registers hold sync/porches as the vendor's dispc_dpi_init
 * packs them: H = hsync | hbp << 8 | hfp << 20, V the same. The pixel clock
 * is from `clocks` when there is one (no clock driver yet: the dts gives the
 * vendor's 384 MHz / (dpi_clk_div 7 + 1)), else 60 Hz is assumed.
 */
static void sc7731g_read_mode(struct sc7731g_dispc *d, struct device *dev)
{
	struct drm_display_mode *m = &d->mode;
	u32 size = readl(d->base + DISPC_SIZE_XY);
	u32 h = readl(d->base + DISPC_DPI_H_TIMING);
	u32 v = readl(d->base + DISPC_DPI_V_TIMING);
	u32 hsync = h & 0xff, hbp = (h >> 8) & 0xfff, hfp = (h >> 20) & 0xfff;
	u32 vsync = v & 0xff, vbp = (v >> 8) & 0xfff, vfp = (v >> 20) & 0xfff;
	struct clk *clk = devm_clk_get_optional(dev, NULL);
	unsigned long rate = IS_ERR_OR_NULL(clk) ? 0 : clk_get_rate(clk);
	u32 w = size & 0xfff, ht = size >> 16 & 0xfff;
	u32 width_mm = 0, height_mm = 0;

	m->hdisplay = w;
	m->hsync_start = w + hfp;
	m->hsync_end = m->hsync_start + hsync;
	m->htotal = m->hsync_end + hbp;
	m->vdisplay = ht;
	m->vsync_start = ht + vfp;
	m->vsync_end = m->vsync_start + vsync;
	m->vtotal = m->vsync_end + vbp;
	m->clock = rate ? rate / 1000 : m->htotal * m->vtotal * 60 / 1000;
	m->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	of_property_read_u32(dev->of_node, "width-mm", &width_mm);
	of_property_read_u32(dev->of_node, "height-mm", &height_mm);
	m->width_mm = width_mm;
	m->height_mm = height_mm;
	drm_mode_set_name(m);

	dev_info(dev, "mode from U-Boot: %ux%u, H %u/%u/%u V %u/%u/%u, %u kHz\n",
		 w, ht, hfp, hsync, hbp, vfp, vsync, vbp, m->clock);
}

DEFINE_DRM_GEM_DMA_FOPS(sc7731g_fops);

static const struct drm_driver sc7731g_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.fops = &sc7731g_fops,
	DRM_GEM_DMA_DRIVER_OPS,
	DRM_FBDEV_DMA_DRIVER_OPS,
	.name = "sc7731g-dispc",
	.desc = "Spreadtrum SC7731G DISPC",
	.major = 1,
	.minor = 0,
};

/*
 * Plain drm_gem_fb_create, no dirty hook: in video mode the DISPC re-reads
 * the buffer every frame, so there is nothing to flush. The first version
 * used drm_gem_fb_create_with_dirty, and with a dirty hook drm_fbdev_dma
 * switches to a vmalloc shadow that a damage worker blits into the scanout
 * buffer -- and on the frame (2026-10-27) that blit happened once and never
 * again: writes to /dev/fb0 stayed in the shadow, the scanout buffer kept
 * whatever Android had left in that RAM, and we saw a frozen Android
 * screen. Why the worker stopped wasn't found (no oops, no stuck thread,
 * oops_in_progress 0, no kprobes in this build); without the hook fbdev
 * and fbcon draw straight into the scanned buffer and the question is moot.
 */
static const struct drm_mode_config_funcs sc7731g_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int sc7731g_dispc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sc7731g_dispc *d;
	struct drm_device *drm;
	int ret;

	d = devm_drm_dev_alloc(dev, &sc7731g_driver, struct sc7731g_dispc, drm);
	if (IS_ERR(d))
		return PTR_ERR(d);
	drm = &d->drm;
	platform_set_drvdata(pdev, drm);

	d->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(d->base))
		return PTR_ERR(d->base);
	d->irq = platform_get_irq(pdev, 0);
	if (d->irq < 0)
		return d->irq;

	sc7731g_read_mode(d, dev);
	if (!d->mode.hdisplay || !d->mode.vdisplay)
		return dev_err_probe(dev, -ENODEV, "DISPC has no size: was the panel lit by U-Boot?\n");

	/* simpledrm on U-Boot's splash goes; from here the buffers are ours */
	ret = aperture_remove_all_conflicting_devices(sc7731g_driver.name);
	if (ret)
		return ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;
	drm->mode_config.min_width = d->mode.hdisplay;
	drm->mode_config.max_width = d->mode.hdisplay;
	drm->mode_config.min_height = d->mode.vdisplay;
	drm->mode_config.max_height = d->mode.vdisplay;
	drm->mode_config.funcs = &sc7731g_mode_config_funcs;
	drm->mode_config.preferred_depth = 24;

	drm_connector_helper_add(&d->connector, &sc7731g_conn_helpers);
	ret = drm_connector_init(drm, &d->connector, &sc7731g_conn_funcs,
				 DRM_MODE_CONNECTOR_DSI);
	if (ret)
		return ret;

	ret = drm_simple_display_pipe_init(drm, &d->pipe, &sc7731g_pipe_funcs,
					   sc7731g_formats, ARRAY_SIZE(sc7731g_formats),
					   NULL, &d->connector);
	if (ret)
		return ret;

	ret = drm_vblank_init(drm, 1);
	if (ret)
		return ret;
	writel(0, d->base + DISPC_INT_EN);
	writel(0x3f, d->base + DISPC_INT_CLR);
	ret = devm_request_irq(dev, d->irq, sc7731g_irq, 0, dev_name(dev), d);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);
	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;
	drm_client_setup(drm, NULL);
	return 0;
}

static void sc7731g_dispc_remove(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
}

static void sc7731g_dispc_shutdown(struct platform_device *pdev)
{
	drm_atomic_helper_shutdown(platform_get_drvdata(pdev));
}

static const struct of_device_id sc7731g_dispc_of_match[] = {
	{ .compatible = "sprd,sc7731g-dispc" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc7731g_dispc_of_match);

static struct platform_driver sc7731g_dispc_driver = {
	.driver = {
		.name = "sc7731g-dispc",
		.of_match_table = sc7731g_dispc_of_match,
	},
	.probe = sc7731g_dispc_probe,
	.remove = sc7731g_dispc_remove,
	.shutdown = sc7731g_dispc_shutdown,
};
module_platform_driver(sc7731g_dispc_driver);

MODULE_DESCRIPTION("Spreadtrum SC7731G display controller (U-Boot takeover)");
MODULE_LICENSE("GPL");
