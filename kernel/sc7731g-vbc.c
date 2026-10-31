// SPDX-License-Identifier: GPL-2.0-only
/*
 * The SC7731G's VBC ("voice band codec", r2p0): the AP's digital audio
 * interface, whose DAC FIFOs feed the SC2713S codec (sc2713s-codec.c).
 * Playback only, mono, through mainline's sprd-pcm-dma (the "sprd,pcm-
 * platform" node) and sprd-dma (patch 0003's SC7731G variant).
 *
 * Register use is the vendor's sound/soc/sprd/dai/vbc/r2p0/ (vbc.c,
 * vbc-comm.c) and __sprd_audio_sc8830.h, Samsung SM-G531H GPL source;
 * docs/audio.md has the sequence.
 *
 * Mono because of sprd-pcm-dma: it feeds channel i from the i-th
 * channels'th of the buffer (dma_addr + i * totsize / channels), i.e. it
 * wants planar samples, while advertising SNDRV_PCM_INFO_INTERLEAVED. With
 * one channel the two agree. The VT10101 has one speaker anyway, and ALSA's
 * plug layer downmixes. DA0 is the left DAC, which the codec routes to the
 * speaker.
 *
 * The VBC and the codec's IRQ belong to the CP (the modem side) at reset:
 * probe takes them for the AP, as the vendor's vbc probe does
 * (arch_audio_vbc_switch). Nothing on this frame runs the CP's audio.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dmaengine.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <sound/dmaengine_pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "sprd-pcm-dma.h"

#define VBDA0		0x00	/* DA0 FIFO: the DMA's destination */
#define VBBUFFSIZE	0x10	/* 15:8 DA buffer size - 1 */
#define VBDABUFFDTA	0x18	/* bit 13 DA0 DMA, bit 15 VBENABLE */
#define  DA0_DMA_EN	BIT(13)
#define  VBENABLE	BIT(15)
#define VBIISSEL	0x3c	/* bit 14 DA LRCK */
#define DAPATCHCTL	0x40
#define DADGCTL		0x44	/* digital gain: left at 0, i.e. off */
#define VBCHNEN		0xc8	/* bit 0 DA0 */

/* the DA FIFOs are 160 16-bit frames, ping-pong; the vendor asks the DMA
 * for one buffer's worth per request (sprd-pcm.c) */
#define VBC_FIFO_FRAMES	160
#define VBC_FRAG_BYTES	(VBC_FIFO_FRAMES * 2)

/* AON APB (syscon) */
#define AON_APB_RST0	0x08	/* bit 20 VBC reset */
#define AON_VBC_CTRL	0x20	/* 19:4 who owns what (0 = AP) */
#define AON_WPROT_AP	0x3040	/* bit 6: the AP may not write the VBC */
#define AON_WPROT_CP(n)	(0x3044 + 4 * (n))	/* ... nor CP0/1/2, once set */

/* PMU APB (syscon) */
#define PMU_SLEEP_CTRL	0xcc	/* bit 8 AP_SLEEP_XTL_ON */
#define PMU_MEM_PD_CFG0	0x138	/* 5:2 the VBC's memories' power-down */

struct sc7731g_vbc {
	void __iomem *base;
	phys_addr_t phys;
	struct regmap *aon;
	struct regmap *pmu;
	struct sprd_pcm_dma_params dma;
};

static void vbc_bits(struct sc7731g_vbc *v, u32 reg, u32 mask, bool on)
{
	u32 x = readl(v->base + reg);

	writel(on ? x | mask : x & ~mask, v->base + reg);
}

static void sc7731g_vbc_take(struct sc7731g_vbc *v)
{
	int i;

	for (i = 0; i < 3; i++)
		regmap_update_bits(v->aon, AON_WPROT_CP(i), BIT(6), BIT(6));
	regmap_update_bits(v->aon, AON_VBC_CTRL, GENMASK(19, 4), 0);
	regmap_update_bits(v->aon, AON_WPROT_AP, BIT(6), 0);
	regmap_update_bits(v->pmu, PMU_SLEEP_CTRL, BIT(8), BIT(8));
	regmap_update_bits(v->pmu, PMU_MEM_PD_CFG0, GENMASK(5, 2), 0);
	regmap_update_bits(v->aon, AON_APB_RST0, BIT(20), BIT(20));
	udelay(10);
	regmap_update_bits(v->aon, AON_APB_RST0, BIT(20), 0);
}

static int sc7731g_vbc_startup(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *dai)
{
	/* whole FIFO buffers per period, so no fragment straddles one */
	return snd_pcm_hw_constraint_step(substream->runtime, 0,
					  SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
					  VBC_FRAG_BYTES);
}

static int sc7731g_vbc_trigger(struct snd_pcm_substream *substream, int cmd,
			       struct snd_soc_dai *dai)
{
	struct sc7731g_vbc *v = snd_soc_dai_get_drvdata(dai);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		writel((readl(v->base + VBBUFFSIZE) & ~GENMASK(15, 8)) |
		       ((VBC_FIFO_FRAMES - 1) << 8), v->base + VBBUFFSIZE);
		vbc_bits(v, VBIISSEL, BIT(14), true);
		writel(0, v->base + DADGCTL);
		writel(0, v->base + DAPATCHCTL);
		vbc_bits(v, VBCHNEN, BIT(0), true);
		vbc_bits(v, VBDABUFFDTA, DA0_DMA_EN, true);
		vbc_bits(v, VBDABUFFDTA, VBENABLE, true);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		vbc_bits(v, VBDABUFFDTA, VBENABLE | DA0_DMA_EN, false);
		vbc_bits(v, VBCHNEN, BIT(0), false);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int sc7731g_vbc_dai_probe(struct snd_soc_dai *dai)
{
	struct sc7731g_vbc *v = snd_soc_dai_get_drvdata(dai);

	snd_soc_dai_init_dma_data(dai, &v->dma, NULL);
	return 0;
}

static const struct snd_soc_dai_ops sc7731g_vbc_ops = {
	.probe = sc7731g_vbc_dai_probe,
	.startup = sc7731g_vbc_startup,
	.trigger = sc7731g_vbc_trigger,
};

static struct snd_soc_dai_driver sc7731g_vbc_dai = {
	.name = "sc7731g-vbc",
	.playback = {
		.stream_name = "VBC Playback",
		.channels_min = 1,
		.channels_max = 1,
		.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.ops = &sc7731g_vbc_ops,
};

static const struct snd_soc_component_driver sc7731g_vbc_component = {
	.name = "sc7731g-vbc",
	.legacy_dai_naming = 1,
};

static int sc7731g_vbc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sc7731g_vbc *v;
	struct resource *res;
	struct clk *clk;

	v = devm_kzalloc(dev, sizeof(*v), GFP_KERNEL);
	if (!v)
		return -ENOMEM;
	v->base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(v->base))
		return PTR_ERR(v->base);
	v->phys = res->start;
	v->aon = syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,syscon-aon");
	if (IS_ERR(v->aon))
		return dev_err_probe(dev, PTR_ERR(v->aon), "sprd,syscon-aon\n");
	v->pmu = syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,syscon-pmu");
	if (IS_ERR(v->pmu))
		return dev_err_probe(dev, PTR_ERR(v->pmu), "sprd,syscon-pmu\n");
	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "clock\n");

	sc7731g_vbc_take(v);

	v->dma.dev_phys[0] = v->phys + VBDA0;
	v->dma.datawidth[0] = DMA_SLAVE_BUSWIDTH_2_BYTES;
	v->dma.fragment_len[0] = VBC_FRAG_BYTES;
	v->dma.chan_name[0] = "da0";

	platform_set_drvdata(pdev, v);
	dev_set_drvdata(dev, v);
	return devm_snd_soc_register_component(dev, &sc7731g_vbc_component,
					       &sc7731g_vbc_dai, 1);
}

static const struct of_device_id sc7731g_vbc_of_match[] = {
	{ .compatible = "sprd,sc7731g-vbc" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc7731g_vbc_of_match);

static struct platform_driver sc7731g_vbc_driver = {
	.driver = {
		.name = "sc7731g-vbc",
		.of_match_table = sc7731g_vbc_of_match,
	},
	.probe = sc7731g_vbc_probe,
};
module_platform_driver(sc7731g_vbc_driver);

MODULE_DESCRIPTION("Spreadtrum SC7731G VBC (r2p0) playback DAI");
MODULE_LICENSE("GPL");
