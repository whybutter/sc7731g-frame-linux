// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * SC7731G (SC8830 family, "scx30g") clocks, described one node per clock in
 * the device tree the way the vendor did it.
 *
 * Mainline's Spreadtrum clock drivers (drivers/clk/sprd) are for the SC9860
 * and later, with the tree compiled into the driver. For this chip the only
 * description there is is the vendor's: its DT has every clock as a node
 * (fixed, fixed-factor, PLL, gate, mux, divider, composite) with the raw
 * register address and mask in `reg`, read by drivers/clk/clk-sc.c of the
 * Samsung SM-G531H GPL kernel. kernel/sc7731g-clocks.dtsi is that tree, cut
 * from the VT10101's own dt.img (with our compatibles), and this is clk-sc.c
 * redone for the mainline clk API, keeping its binding:
 *
 *   gate:       reg = <enable-reg mask [prepare-reg mask]>
 *   muxed:      reg = <select-reg mask [enable-reg mask [prepare-reg mask]]>
 *   divider:    reg = <divider-reg mask [enable-reg mask]>
 *   composite:  reg = <select-reg mask divider-reg mask [enable-reg mask]>
 *   adj. PLL:   reg = <cfg1-reg mask prepare-reg mask>
 *
 * A prepare register with bit 0 set in its address means "setting the mask
 * disables" (the vendor's CLK_GATE_SET_TO_DISABLE trick; e.g. clk_gpu_axi's
 * prepare is the GPU power domain's force-shutdown bit).
 *
 * What is different from the vendor's:
 *
 *  - Registers are read-modify-written under one lock. The vendor wrote the
 *    mask to the register's set/clear aliases (+0x1000/+0x2000) under a
 *    hardware spinlock shared with the modem; nothing else runs on this SoC
 *    when we do, and RMW is right for any register, alias or not.
 *  - PLLs are read-only (rate from CFG1: 26 MHz * (Nint + Kint/2^20), the
 *    vendor's scx30g formula). Nothing here should retune the DDR or CPU PLL.
 *  - No MM power domain (camera/codec clocks are left out of the dtsi), and
 *    no clocks whose registers sit inside the GPU (0x6xxxxxxx): reading those
 *    with the GPU domain off can hang the bus, and CCF reads every mux at
 *    registration. They come back with the GPU driver.
 *  - Every gate and mux is CLK_IGNORE_UNUSED, as in the vendor's: U-Boot set
 *    up things (the display, the eMMC) whose drivers may not claim their
 *    clocks yet.
 */

#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

static DEFINE_SPINLOCK(sc7731g_clk_lock);

struct sc7731g_reg {
	void __iomem *reg;
	u32 msk;
	bool set_disables;
};

struct sc7731g_clk {
	struct clk_hw hw;
	struct sc7731g_reg enb, pre, sel, div;
};

#define to_sc(_hw) container_of(_hw, struct sc7731g_clk, hw)

/* the vendor's reg pairs are raw physical addresses, one page each */
static int __init sc7731g_read_prop(struct device_node *np, const char *prop,
				    int idx, struct sc7731g_reg *r)
{
	u32 cell[2];
	phys_addr_t pa;

	if (of_property_read_u32_index(np, prop, 2 * idx, &cell[0]) ||
	    of_property_read_u32_index(np, prop, 2 * idx + 1, &cell[1]))
		return -ENOENT;
	pa = cell[0];
	r->set_disables = pa & 1;
	pa &= ~3;
	r->reg = ioremap(pa, 4);
	r->msk = cell[1];
	return r->reg ? 0 : -ENOMEM;
}

static int __init sc7731g_read_reg(struct device_node *np, int idx,
				   struct sc7731g_reg *r)
{
	return sc7731g_read_prop(np, "reg", idx, r);
}

static void sc7731g_update(struct sc7731g_reg *r, u32 mask, u32 val)
{
	unsigned long flags;

	spin_lock_irqsave(&sc7731g_clk_lock, flags);
	writel((readl(r->reg) & ~mask) | (val & mask), r->reg);
	spin_unlock_irqrestore(&sc7731g_clk_lock, flags);
}

static void sc7731g_switch(struct sc7731g_reg *r, bool on)
{
	if (r->reg)
		sc7731g_update(r, r->msk, (on ^ r->set_disables) ? r->msk : 0);
}

static bool sc7731g_is_on(struct sc7731g_reg *r)
{
	if (!r->reg)
		return true;
	return !!(readl(r->reg) & r->msk) ^ r->set_disables;
}

static int sc7731g_prepare(struct clk_hw *hw)
{
	sc7731g_switch(&to_sc(hw)->pre, true);
	return 0;
}

static void sc7731g_unprepare(struct clk_hw *hw)
{
	sc7731g_switch(&to_sc(hw)->pre, false);
}

static int sc7731g_is_prepared(struct clk_hw *hw)
{
	return sc7731g_is_on(&to_sc(hw)->pre);
}

static int sc7731g_enable(struct clk_hw *hw)
{
	sc7731g_switch(&to_sc(hw)->enb, true);
	return 0;
}

static void sc7731g_disable(struct clk_hw *hw)
{
	sc7731g_switch(&to_sc(hw)->enb, false);
}

static int sc7731g_is_enabled(struct clk_hw *hw)
{
	return sc7731g_is_on(&to_sc(hw)->enb);
}

static u8 sc7731g_get_parent(struct clk_hw *hw)
{
	struct sc7731g_reg *r = &to_sc(hw)->sel;

	return (readl(r->reg) & r->msk) >> __ffs(r->msk);
}

static int sc7731g_set_parent(struct clk_hw *hw, u8 index)
{
	struct sc7731g_reg *r = &to_sc(hw)->sel;

	sc7731g_update(r, r->msk, (u32)index << __ffs(r->msk));
	return 0;
}

static unsigned int sc7731g_div_width(struct sc7731g_reg *r)
{
	return fls(r->msk >> __ffs(r->msk));
}

static unsigned long sc7731g_recalc_rate(struct clk_hw *hw, unsigned long prate)
{
	struct sc7731g_reg *r = &to_sc(hw)->div;
	u32 val = (readl(r->reg) & r->msk) >> __ffs(r->msk);

	return divider_recalc_rate(hw, prate, val, NULL, 0, sc7731g_div_width(r));
}

static int sc7731g_determine_rate(struct clk_hw *hw, struct clk_rate_request *req)
{
	return divider_determine_rate(hw, req, NULL,
				      sc7731g_div_width(&to_sc(hw)->div), 0);
}

static int sc7731g_set_rate(struct clk_hw *hw, unsigned long rate,
			    unsigned long prate)
{
	struct sc7731g_reg *r = &to_sc(hw)->div;
	int val = divider_get_val(rate, prate, NULL, sc7731g_div_width(r), 0);

	if (val < 0)
		return val;
	sc7731g_update(r, r->msk, (u32)val << __ffs(r->msk));
	return 0;
}

static unsigned long sc7731g_pll_recalc_rate(struct clk_hw *hw,
					     unsigned long prate)
{
	u32 cfg1 = readl(to_sc(hw)->enb.reg);
	u64 nint = cfg1 & 0x3f, kint = 0;

	if (cfg1 & BIT(6))	/* SDM_EN: the fractional part counts */
		kint = cfg1 >> 12;
	return 26000000 * nint + div_u64(26000000ULL * kint, 1 << 20);
}

static const struct clk_ops sc7731g_gate_ops = {
	.prepare = sc7731g_prepare,
	.unprepare = sc7731g_unprepare,
	.is_prepared = sc7731g_is_prepared,
	.enable = sc7731g_enable,
	.disable = sc7731g_disable,
	.is_enabled = sc7731g_is_enabled,
};

static const struct clk_ops sc7731g_mux_ops = {
	.prepare = sc7731g_prepare,
	.unprepare = sc7731g_unprepare,
	.is_prepared = sc7731g_is_prepared,
	.enable = sc7731g_enable,
	.disable = sc7731g_disable,
	.is_enabled = sc7731g_is_enabled,
	.get_parent = sc7731g_get_parent,
	.set_parent = sc7731g_set_parent,
	.determine_rate = __clk_mux_determine_rate,
};

static const struct clk_ops sc7731g_div_ops = {
	.enable = sc7731g_enable,
	.disable = sc7731g_disable,
	.is_enabled = sc7731g_is_enabled,
	.recalc_rate = sc7731g_recalc_rate,
	.determine_rate = sc7731g_determine_rate,
	.set_rate = sc7731g_set_rate,
};

static const struct clk_ops sc7731g_composite_ops = {
	.enable = sc7731g_enable,
	.disable = sc7731g_disable,
	.is_enabled = sc7731g_is_enabled,
	.get_parent = sc7731g_get_parent,
	.set_parent = sc7731g_set_parent,
	.recalc_rate = sc7731g_recalc_rate,
	.determine_rate = sc7731g_determine_rate,
	.set_rate = sc7731g_set_rate,
};

static const struct clk_ops sc7731g_pll_ops = {
	.recalc_rate = sc7731g_pll_recalc_rate,
};

static void __init sc7731g_register(struct device_node *np,
				    struct sc7731g_clk *c,
				    const struct clk_ops *ops,
				    unsigned long flags)
{
	const char *name = np->name;
	const char **parents;
	struct clk_init_data init = { };
	int i, n = of_clk_get_parent_count(np);

	of_property_read_string(np, "clock-output-names", &name);
	parents = kcalloc(max(n, 1), sizeof(*parents), GFP_KERNEL);
	if (!parents)
		return;
	for (i = 0; i < n; i++)
		parents[i] = of_clk_get_parent_name(np, i);

	init.name = name;
	init.ops = ops;
	init.flags = flags;
	init.parent_names = parents;
	init.num_parents = n;
	c->hw.init = &init;
	if (clk_hw_register(NULL, &c->hw)) {
		pr_err("%pOFn: cannot register %s\n", np, name);
		kfree(parents);
		kfree(c);
		return;
	}
	kfree(parents);
	of_clk_add_hw_provider(np, of_clk_hw_simple_get, &c->hw);
}

static struct sc7731g_clk *__init sc7731g_alloc(void)
{
	return kzalloc(sizeof(struct sc7731g_clk), GFP_KERNEL);
}

static void __init sc7731g_fixed_setup(struct device_node *np)
{
	const char *name = np->name;
	struct clk_hw *hw;
	u32 rate;

	if (of_property_read_u32(np, "clock-frequency", &rate))
		return;
	of_property_read_string(np, "clock-output-names", &name);
	hw = clk_hw_register_fixed_rate(NULL, name, NULL, 0, rate);
	if (!IS_ERR(hw))
		of_clk_add_hw_provider(np, of_clk_hw_simple_get, hw);
}
CLK_OF_DECLARE(sc7731g_fixed, "sprd,sc7731g-fixed-clock", sc7731g_fixed_setup);

static void __init sc7731g_fixed_factor_setup(struct device_node *np)
{
	const char *name = np->name;
	struct clk_hw *hw;
	u32 div, mult;

	if (of_property_read_u32(np, "clock-div", &div) ||
	    of_property_read_u32(np, "clock-mult", &mult))
		return;
	of_property_read_string(np, "clock-output-names", &name);
	hw = clk_hw_register_fixed_factor(NULL, name, of_clk_get_parent_name(np, 0),
					  0, mult, div);
	if (!IS_ERR(hw))
		of_clk_add_hw_provider(np, of_clk_hw_simple_get, hw);
}
CLK_OF_DECLARE(sc7731g_fixed_factor, "sprd,sc7731g-fixed-factor-clock",
	       sc7731g_fixed_factor_setup);

static void __init sc7731g_pll_setup(struct device_node *np)
{
	struct sc7731g_clk *c = sc7731g_alloc();

	if (!c)
		return;
	/* the CFG1 register goes in .enb, which is otherwise unused here */
	if (sc7731g_read_reg(np, 0, &c->enb)) {
		kfree(c);
		return;
	}
	sc7731g_register(np, c, &sc7731g_pll_ops, 0);
}
CLK_OF_DECLARE(sc7731g_pll, "sprd,sc7731g-adjustable-pll-clock", sc7731g_pll_setup);

static void __init sc7731g_gate_setup(struct device_node *np)
{
	struct sc7731g_clk *c = sc7731g_alloc();

	if (!c)
		return;
	if (sc7731g_read_reg(np, 0, &c->enb)) {
		kfree(c);
		return;
	}
	sc7731g_read_reg(np, 1, &c->pre);
	sc7731g_register(np, c, &sc7731g_gate_ops, CLK_IGNORE_UNUSED);
}
CLK_OF_DECLARE(sc7731g_gate, "sprd,sc7731g-gate-clock", sc7731g_gate_setup);

static void __init sc7731g_mux_setup(struct device_node *np)
{
	struct sc7731g_clk *c = sc7731g_alloc();

	if (!c)
		return;
	if (sc7731g_read_reg(np, 0, &c->sel)) {
		kfree(c);
		return;
	}
	if (!sc7731g_read_reg(np, 1, &c->enb))
		sc7731g_read_reg(np, 2, &c->pre);
	sc7731g_register(np, c, &sc7731g_mux_ops, CLK_IGNORE_UNUSED);
}
CLK_OF_DECLARE(sc7731g_mux, "sprd,sc7731g-muxed-clock", sc7731g_mux_setup);

static void __init sc7731g_div_setup(struct device_node *np)
{
	struct sc7731g_clk *c = sc7731g_alloc();

	if (!c)
		return;
	if (sc7731g_read_reg(np, 0, &c->div)) {
		kfree(c);
		return;
	}
	sc7731g_read_reg(np, 1, &c->enb);
	sc7731g_register(np, c, &sc7731g_div_ops, CLK_IGNORE_UNUSED);
}
CLK_OF_DECLARE(sc7731g_div, "sprd,sc7731g-divider-clock", sc7731g_div_setup);

/*
 * `sprd,domain = <reg mask>...`: switched on (in the binding's sense, so a
 * prepare-style address with bit 0 set clears its mask) before the clock is
 * registered, and left on. For clk_gpu, whose mux and divider sit inside the
 * GPU: its power domain's force-shutdown (PMU_APB PD_GPU_TOP_CFG bit 25) and
 * the GPU's bus gate (AON_APB EB0 bit 27), what the vendor's clk_gpu_axi
 * prepare/enable did. CCF reads the mux at registration, and lima takes the
 * GPU's own PMU from there; a frame on mains doesn't miss the power gating.
 */
static void __init sc7731g_power_domain(struct device_node *np)
{
	struct sc7731g_reg r;
	int i;

	for (i = 0; !sc7731g_read_prop(np, "sprd,domain", i, &r); i++) {
		sc7731g_switch(&r, true);
		iounmap(r.reg);
	}
	if (i)
		udelay(100);
}

static void __init sc7731g_composite_setup(struct device_node *np)
{
	struct sc7731g_clk *c = sc7731g_alloc();

	if (!c)
		return;
	sc7731g_power_domain(np);
	if (sc7731g_read_reg(np, 0, &c->sel) || sc7731g_read_reg(np, 1, &c->div)) {
		kfree(c);
		return;
	}
	sc7731g_read_reg(np, 2, &c->enb);
	sc7731g_register(np, c, &sc7731g_composite_ops, CLK_IGNORE_UNUSED);
}
CLK_OF_DECLARE(sc7731g_composite, "sprd,sc7731g-composite-dev-clock",
	       sc7731g_composite_setup);
