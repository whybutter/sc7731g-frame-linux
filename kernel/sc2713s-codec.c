// SPDX-License-Identifier: GPL-2.0-only
/*
 * The SC2713S PMIC's audio codec (the vendor's "sprd-codec v3"), as the
 * VTimes VT10101 uses it: one mono playback path to its one speaker.
 *
 * The codec has two halves. Its digital side (the "DP" block, at
 * 0x40000000 in the AP's map) takes the samples from the VBC; its analog
 * side lives in the PMIC, behind the ADI bus, so this device is a child of
 * the PMIC's node (regulator-sc2713s.c populates it) and uses the PMIC's
 * regmap. Register use is the vendor's sound/soc/sprd/codec/sprd/v3/
 * sprd-codec.c and sprd-audio-power.c (Samsung SM-G531H GPL source);
 * docs/audio.md has the sequence and how it was found.
 *
 * The board's speaker is not on the codec's own speaker driver. The vendor
 * path (DAC -> AOL -> the internal PA -> SPK-L) gave silence on the VT10101,
 * not even a switch-on pop: SPK-L has nothing on it, and the right PA's
 * enables (0x640 bits 12/10) don't even stick on this PMIC. The speaker is
 * on SPK-R, driven by an external amplifier that GPIO 85 enables (active
 * high), and that amplifier is fed from the codec's HPR output: found by
 * hand, one output at a time, with someone listening (2026-10-28).
 *
 * So the path here is DAC(L) -> HPR mixer -> HPR driver -> "Speaker" (the
 * external amp's enable). Mono: the VBC side feeds one FIFO (DA0, the left
 * DAC), see sc7731g-vbc.c for why.
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/tlv.h>

/* the analog side, offsets in the PMIC's ADI regmap */
#define AUDIF_ENB	0x600	/* bit 0 DACL to the analog side */
#define PMUR2_PMUR1	0x640	/* bit 7 VB, bit 6 VBO */
#define PMUR4_PMUR3	0x644	/* BG 7, BG_IBIAS 5, VCM 4, VCM_BUF 3, ICM+ 2, VCMI 1:0 */
#define  VCM_V_SHIFT	11	/* 13:11; 2 = 3.2 V, the vendor's code for VBAT >= 3.8 V */
#define CCR		0x660	/* bit 3 DAC clock, bit 0 driver clock */
#define DACGR_DACR	0x674	/* bit 7 DACL_EN; DACL gain 14:12, 7 = 0 dB */
#define DAOCR3_DAOCR1	0x67c	/* bit 1 DACL -> HPR */
#define DCR2_DCR1	0x680	/* bit 6 HPR_EN */
#define DCGR2_DCGR1	0x690	/* HPR gain 3:0, -36 dB + 3 dB a step */
#define ANA_MODULE_EN	0x800	/* bit 4 ANA_AUD_EN */
#define ANA_CLK_EN	0x804	/* bit 2 6.5 MHz, bit 1 AUDIF */
#define ANA_RTC_CLK_EN	0x808	/* bit 4 */
#define ANA_RST		0x80c	/* bits 11:9 the audio's resets */
#define ANA_SLP_CTRL4	0x884
#define ANA_XTL_WAIT	0x88c	/* bit 8 XTL_EN */
#define ANA_AUDIO_CTRL	0x8b4	/* bit 1 6P5M_TX_INV */

/* the digital side, in the AP's map */
#define AUD_TOP_CTL	0x00	/* bit 0 DACL enable */
#define AUD_DAC_CTL	0x0c	/* rate 3:0 (48 kHz 1, 44.1 kHz 2), bit 15 mute */
#define  DAC_MUTE	BIT(15)
#define AUD_SDM_CTL0	0x10

/* AON APB (syscon) */
#define AON_APB_RST0	0x08	/* bits 19, 18: AUDIF and AUD resets */
#define AON_SINDRV	0x18	/* bit 0: the codec's 26 MHz */
#define AON_VBC_CTRL	0x20	/* bit 20: AUDIF clock auto-gating */

struct sc2713s_codec {
	struct device *dev;
	struct regmap *adi;
	struct regmap *aon;
	void __iomem *dp;
	struct clk *aud;
	struct gpio_desc *pa;
};

static void adi_bits(struct sc2713s_codec *c, u32 reg, u32 mask, bool on)
{
	regmap_update_bits(c->adi, reg, mask, on ? mask : 0);
}

static void dp_bits(struct sc2713s_codec *c, u32 reg, u32 mask, bool on)
{
	u32 v = readl(c->dp + reg);

	writel(on ? v | mask : v & ~mask, c->dp + reg);
}

/* VB, BG, BG_IBIAS, VCOM, VCOM_BUF, VBO, in the vendor's order */
static int sc2713s_power_event(struct snd_soc_dapm_widget *w,
			       struct snd_kcontrol *k, int event)
{
	struct sc2713s_codec *c =
		snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		adi_bits(c, PMUR2_PMUR1, BIT(7), true);
		adi_bits(c, PMUR4_PMUR3, BIT(7), true);
		adi_bits(c, PMUR4_PMUR3, BIT(5), true);
		regmap_update_bits(c->adi, PMUR4_PMUR3, 7 << VCM_V_SHIFT,
				   2 << VCM_V_SHIFT);
		adi_bits(c, PMUR4_PMUR3, BIT(4), true);
		adi_bits(c, PMUR4_PMUR3, BIT(3), true);
		adi_bits(c, PMUR2_PMUR1, BIT(6), true);
		adi_bits(c, PMUR4_PMUR3, BIT(1) | BIT(0), true);
		/* the vendor defines a 5 ms LDO wait it never uses; VCOM
		 * settling is what pops, so wait anyway */
		usleep_range(5000, 6000);
	} else {
		adi_bits(c, PMUR4_PMUR3, BIT(1) | BIT(0), false);
		adi_bits(c, PMUR2_PMUR1, BIT(6), false);
		adi_bits(c, PMUR4_PMUR3, BIT(3) | BIT(4) | BIT(5) | BIT(7), false);
		adi_bits(c, PMUR2_PMUR1, BIT(7), false);
	}
	return 0;
}

static int sc2713s_dac_event(struct snd_soc_dapm_widget *w,
			     struct snd_kcontrol *k, int event)
{
	struct sc2713s_codec *c =
		snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));
	bool on = SND_SOC_DAPM_EVENT_ON(event);

	if (on) {
		adi_bits(c, CCR, BIT(3) | BIT(0), true);
		dp_bits(c, AUD_TOP_CTL, BIT(0), true);
		adi_bits(c, AUDIF_ENB, BIT(0), true);
		regmap_update_bits(c->adi, DACGR_DACR, BIT(7) | (7 << 12),
				   BIT(7) | (7 << 12));
		dp_bits(c, AUD_DAC_CTL, DAC_MUTE, false);
	} else {
		dp_bits(c, AUD_DAC_CTL, DAC_MUTE, true);
		adi_bits(c, DACGR_DACR, BIT(7), false);
		adi_bits(c, AUDIF_ENB, BIT(0), false);
		dp_bits(c, AUD_TOP_CTL, BIT(0), false);
		adi_bits(c, CCR, BIT(3) | BIT(0), false);
	}
	return 0;
}

static int sc2713s_hpr_event(struct snd_soc_dapm_widget *w,
			     struct snd_kcontrol *k, int event)
{
	struct sc2713s_codec *c =
		snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));
	bool on = SND_SOC_DAPM_EVENT_ON(event);

	adi_bits(c, DAOCR3_DAOCR1, BIT(1), on);
	adi_bits(c, DCR2_DCR1, BIT(6), on);
	return 0;
}

/* the external amplifier: last on, first off */
static int sc2713s_spk_event(struct snd_soc_dapm_widget *w,
			     struct snd_kcontrol *k, int event)
{
	struct sc2713s_codec *c =
		snd_soc_component_get_drvdata(snd_soc_dapm_to_component(w->dapm));

	gpiod_set_value_cansleep(c->pa, SND_SOC_DAPM_EVENT_ON(event));
	return 0;
}

static const struct snd_soc_dapm_widget sc2713s_widgets[] = {
	SND_SOC_DAPM_SUPPLY("Analog Power", SND_SOC_NOPM, 0, 0,
			    sc2713s_power_event,
			    SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_DAC_E("DAC", "Playback", SND_SOC_NOPM, 0, 0,
			   sc2713s_dac_event,
			   SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_PGA_E("HPR", SND_SOC_NOPM, 0, 0, NULL, 0,
			   sc2713s_hpr_event,
			   SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_SPK("Speaker", sc2713s_spk_event),
};

static const struct snd_soc_dapm_route sc2713s_routes[] = {
	{ "DAC", NULL, "Analog Power" },
	{ "HPR", NULL, "DAC" },
	{ "Speaker", NULL, "HPR" },
};

static const DECLARE_TLV_DB_SCALE(hp_tlv, -3600, 300, 1);

static const struct snd_kcontrol_new sc2713s_controls[] = {
	SOC_SINGLE_TLV("Speaker Playback Volume", DCGR2_DCGR1, 0, 15, 0, hp_tlv),
};

static int sc2713s_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params,
			     struct snd_soc_dai *dai)
{
	struct sc2713s_codec *c = snd_soc_component_get_drvdata(dai->component);
	u32 code, v;

	switch (params_rate(params)) {
	case 48000:
		code = 1;
		break;
	case 44100:
		code = 2;
		break;
	default:
		return -EINVAL;
	}
	v = readl(c->dp + AUD_DAC_CTL);
	writel((v & ~0xf) | code, c->dp + AUD_DAC_CTL);
	return 0;
}

static const struct snd_soc_dai_ops sc2713s_dai_ops = {
	.hw_params = sc2713s_hw_params,
};

static struct snd_soc_dai_driver sc2713s_dai = {
	.name = "sc2713s-dac",
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.ops = &sc2713s_dai_ops,
};

/*
 * Once, at probe: the codec's clocks, resets and the analog block's
 * enables, which the vendor also sets once and leaves (codec.c's
 * sprd_codec_power_up / __sprd_audio_sc8830.h). Only the audio power,
 * DAC, HPR and the amplifier follow playback, through DAPM.
 */
static void sc2713s_init(struct sc2713s_codec *c)
{
	regmap_update_bits(c->aon, AON_VBC_CTRL, BIT(20), BIT(20));
	regmap_update_bits(c->aon, AON_SINDRV, BIT(0), BIT(0));
	regmap_update_bits(c->aon, AON_APB_RST0, BIT(18) | BIT(19), BIT(18) | BIT(19));
	udelay(10);
	regmap_update_bits(c->aon, AON_APB_RST0, BIT(18) | BIT(19), 0);

	adi_bits(c, ANA_MODULE_EN, BIT(4), true);
	adi_bits(c, ANA_RST, GENMASK(11, 9), true);
	udelay(10);
	adi_bits(c, ANA_RST, GENMASK(11, 9), false);
	adi_bits(c, PMUR4_PMUR3, BIT(2), true);
	adi_bits(c, ANA_CLK_EN, BIT(2) | BIT(1), true);
	adi_bits(c, ANA_AUDIO_CTRL, BIT(1), true);
	adi_bits(c, ANA_RTC_CLK_EN, BIT(4), true);
	adi_bits(c, ANA_XTL_WAIT, BIT(8), true);
	regmap_write(c->adi, ANA_SLP_CTRL4, 0);

	writel(0x400, c->dp + AUD_SDM_CTL0);
	dp_bits(c, AUD_DAC_CTL, DAC_MUTE, true);
	/* -6 dB on the speaker until userspace says otherwise: the external
	 * amp's gain is unknown, and the bench test at -12 dB was loud enough */
	regmap_update_bits(c->adi, DCGR2_DCGR1, 0xf, 10);
}

static int sc2713s_component_probe(struct snd_soc_component *component)
{
	struct sc2713s_codec *c = snd_soc_component_get_drvdata(component);

	snd_soc_component_init_regmap(component, c->adi);
	sc2713s_init(c);
	return 0;
}

static const struct snd_soc_component_driver sc2713s_component = {
	.probe = sc2713s_component_probe,
	.controls = sc2713s_controls,
	.num_controls = ARRAY_SIZE(sc2713s_controls),
	.dapm_widgets = sc2713s_widgets,
	.num_dapm_widgets = ARRAY_SIZE(sc2713s_widgets),
	.dapm_routes = sc2713s_routes,
	.num_dapm_routes = ARRAY_SIZE(sc2713s_routes),
	.endianness = 1,
};

static int sc2713s_codec_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sc2713s_codec *c;
	u32 dp[2];
	int ret;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	c->dev = dev;

	c->adi = dev_get_regmap(dev->parent, NULL);
	if (!c->adi)
		return dev_err_probe(dev, -ENODEV, "no PMIC regmap\n");
	c->aon = syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,syscon-aon");
	if (IS_ERR(c->aon))
		return dev_err_probe(dev, PTR_ERR(c->aon), "sprd,syscon-aon\n");
	/* the DP block is in the AP's map, not the PMIC's: a property, as
	 * the vendor DT has it (sprd,reg_dp), since this node's reg is the
	 * PMIC bus's */
	ret = of_property_read_u32_array(dev->of_node, "sprd,reg-dp", dp, 2);
	if (ret)
		return dev_err_probe(dev, ret, "sprd,reg-dp\n");
	c->dp = devm_ioremap(dev, dp[0], dp[1]);
	if (!c->dp)
		return -ENOMEM;
	c->aud = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(c->aud))
		return dev_err_probe(dev, PTR_ERR(c->aud), "clock\n");
	c->pa = devm_gpiod_get(dev, "pa", GPIOD_OUT_LOW);
	if (IS_ERR(c->pa))
		return dev_err_probe(dev, PTR_ERR(c->pa), "pa-gpios\n");

	platform_set_drvdata(pdev, c);
	return devm_snd_soc_register_component(dev, &sc2713s_component,
					       &sc2713s_dai, 1);
}

static const struct of_device_id sc2713s_codec_of_match[] = {
	{ .compatible = "sprd,sc2713s-codec" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc2713s_codec_of_match);

static struct platform_driver sc2713s_codec_driver = {
	.driver = {
		.name = "sc2713s-codec",
		.of_match_table = sc2713s_codec_of_match,
	},
	.probe = sc2713s_codec_probe,
};
module_platform_driver(sc2713s_codec_driver);

MODULE_DESCRIPTION("Spreadtrum SC2713S audio codec (VT10101 speaker path)");
MODULE_LICENSE("GPL");
