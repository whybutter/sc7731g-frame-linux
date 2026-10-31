// SPDX-License-Identifier: GPL-2.0-only
/*
 * PWM block of the Spreadtrum SC7731G (0x40260000, four channels 0x20
 * apart). Channel 3 drives the VT10101's backlight controller.
 *
 * Mainline's pwm-sprd is the SC9860's, with separate MOD/DUTY/ENABLE
 * registers. Here one CNT register holds both (duty << 8 | mod) and the
 * enable is bit 8 of PRESCALE, as the vendor's drivers/video/backlight/
 * sprd_backlight.c (Samsung SM-G531H GPL kernel; the frame's kernel prints
 * the same strings) programs it:
 *
 *   PRESCALE = prescale (7:0) | enable (8)
 *   CNT      = duty (15:8) | mod (7:0)
 *   PAT_LOW / PAT_HIG = 0xffff (the "pattern" registers, all ones = plain PWM)
 *
 * Period = (prescale + 1) * (mod + 1) / clk, high time = duty / (mod + 1) of
 * it. The vendor ran mod 255, prescale 0 off 26 MHz (~101.6 kHz); so do we,
 * with prescale picked for the period asked.
 */

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

#define PWM_PRESCALE		0x00
#define  PWM_ENABLE		BIT(8)
#define PWM_CNT			0x04
#define PWM_PAT_LOW		0x0c
#define PWM_PAT_HIG		0x10
#define PWM_CH(n)		((n) * 0x20)
#define PWM_MOD			0xff
#define PWM_NUM			4

struct sc7731g_pwm {
	void __iomem *base;
	struct clk *clk[PWM_NUM];
};

static inline struct sc7731g_pwm *to_sc(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static int sc7731g_pwm_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			     const struct pwm_state *state)
{
	struct sc7731g_pwm *p = to_sc(chip);
	void __iomem *ch = p->base + PWM_CH(pwm->hwpwm);
	struct clk *clk = p->clk[pwm->hwpwm];
	bool was_on = pwm->state.enabled;
	u64 rate, cycles, prescale, duty;
	int ret;

	if (state->polarity != PWM_POLARITY_NORMAL)
		return -EINVAL;

	if (!state->enabled) {
		if (was_on) {
			writel(0, ch + PWM_PRESCALE);
			clk_disable_unprepare(clk);
		}
		return 0;
	}

	if (!was_on) {
		ret = clk_prepare_enable(clk);
		if (ret)
			return ret;
	}
	rate = clk_get_rate(clk);
	cycles = mul_u64_u64_div_u64(state->period, rate, NSEC_PER_SEC);
	prescale = div_u64(cycles, PWM_MOD + 1);
	prescale = clamp_val(prescale, 1, 256) - 1;
	duty = state->period ? mul_u64_u64_div_u64(state->duty_cycle, PWM_MOD + 1,
						   state->period) : 0;
	duty = min_t(u64, duty, PWM_MOD + 1);

	/* full on: the vendor's mod 255 tops out at 255/256; duty == mod + 1
	 * doesn't fit 8 bits, so 255 it is */
	writel(min_t(u64, duty, PWM_MOD) << 8 | PWM_MOD, ch + PWM_CNT);
	writel(0xffff, ch + PWM_PAT_LOW);
	writel(0xffff, ch + PWM_PAT_HIG);
	writel(prescale | PWM_ENABLE, ch + PWM_PRESCALE);
	return 0;
}

static int sc7731g_pwm_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
				 struct pwm_state *state)
{
	struct sc7731g_pwm *p = to_sc(chip);
	void __iomem *ch = p->base + PWM_CH(pwm->hwpwm);
	u32 pre = readl(ch + PWM_PRESCALE), cnt = readl(ch + PWM_CNT);
	u64 rate = clk_get_rate(p->clk[pwm->hwpwm]) ?: 26000000;
	u32 mod = cnt & 0xff;

	state->enabled = pre & PWM_ENABLE;
	state->polarity = PWM_POLARITY_NORMAL;
	state->period = div64_u64((u64)((pre & 0xff) + 1) * (mod + 1) * NSEC_PER_SEC, rate);
	state->duty_cycle = mod ? div_u64(state->period * ((cnt >> 8) & 0xff), mod + 1) : 0;
	/* a channel U-Boot left running keeps its clock: count it as ours */
	if (state->enabled)
		clk_prepare_enable(p->clk[pwm->hwpwm]);
	return 0;
}

static const struct pwm_ops sc7731g_pwm_ops = {
	.apply = sc7731g_pwm_apply,
	.get_state = sc7731g_pwm_get_state,
};

static int sc7731g_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pwm_chip *chip;
	struct sc7731g_pwm *p;
	int i;

	chip = devm_pwmchip_alloc(dev, PWM_NUM, sizeof(*p));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	p = to_sc(chip);
	p->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(p->base))
		return PTR_ERR(p->base);
	for (i = 0; i < PWM_NUM; i++) {
		char name[8];

		snprintf(name, sizeof(name), "pwm%d", i);
		p->clk[i] = devm_clk_get_optional(dev, name);
		if (IS_ERR(p->clk[i]))
			return dev_err_probe(dev, PTR_ERR(p->clk[i]), "%s clock\n", name);
	}
	chip->ops = &sc7731g_pwm_ops;
	return devm_pwmchip_add(dev, chip);
}

static const struct of_device_id sc7731g_pwm_of_match[] = {
	{ .compatible = "sprd,sc7731g-pwm" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc7731g_pwm_of_match);

static struct platform_driver sc7731g_pwm_driver = {
	.driver = {
		.name = "pwm-sprd-sc7731g",
		.of_match_table = sc7731g_pwm_of_match,
	},
	.probe = sc7731g_pwm_probe,
};
module_platform_driver(sc7731g_pwm_driver);

MODULE_DESCRIPTION("Spreadtrum SC7731G PWM");
MODULE_LICENSE("GPL");
