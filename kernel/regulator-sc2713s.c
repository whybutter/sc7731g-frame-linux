// SPDX-License-Identifier: GPL-2.0-only
/*
 * LDOs of the Spreadtrum SC2713S PMIC (the SC7731G's), on the ADI bus.
 *
 * Why it exists: out-3 (2026-10-24) brought up i2c1 and the gslX680 didn't
 * answer (-ENXIO), and dwc2 timed out in its core soft reset. The vendor DT
 * feeds the touch chip from vdd28 and the USB PHY from vddusb, both PMIC
 * LDOs that nothing in our kernel switched on.
 *
 * Mainline's sc2731-regulator is the next PMIC generation (different
 * registers, and it hangs off the sc27xx MFD, which wants the PMIC's
 * interrupt). This is the vendor's sc2713s-regulator_dt.c (Samsung SM-G531H
 * GPL kernel) cut down to its LDO case, as a driver for the `pmic@0` child
 * of the ADI bus with the sc27xx MFD's regmap-over-ADI (spi_write of a
 * {reg, val} pair, spi_read with the register in the buffer). Registers are
 * offsets from the PMIC's slave base (0x40038000), as sprd-adi takes them.
 *
 * Each LDO under `regulators` describes itself the way the vendor DT does,
 * minus the physical base:
 *
 *   sprd,pd-set = <reg mask>   setting it powers the LDO down
 *   sprd,pd-clr = <reg mask>   setting it powers it up, over pd-set
 *                              (optional: newer LDOs have only pd-set)
 *   sprd,vsel   = <reg mask>   index into sprd,voltages
 *   sprd,voltages = <uV x4>    the vendor DT's regulator-selects
 *
 * On/off follows the vendor's ldo_turn_on/off. Its ldo_is_on returns
 * "unknown" when both bits are clear (the reset state, where the LDO runs
 * at its hardware default); here that reads as off, so that an always-on
 * or a consumer's enable writes pd-clr and makes it certain.
 *
 * Only the LDOs we use are in the dts: the regulator core switches off
 * regulators nobody claimed, and the DCDCs and the eMMC/panel LDOs are not
 * ours to lose.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>
#include <linux/spi/spi.h>
#include <linux/watchdog.h>

struct sc2713s_ldo {
	struct regulator_desc desc;
	u32 set_reg, set_mask;
	u32 clr_reg, clr_mask;	/* clr_mask 0: no pd-clr */
	unsigned int volts[4];
};

static int sc2713s_ldo_enable(struct regulator_dev *rdev)
{
	struct sc2713s_ldo *l = rdev_get_drvdata(rdev);
	int ret = 0;

	if (l->clr_mask)
		ret = regmap_set_bits(rdev->regmap, l->clr_reg, l->clr_mask);
	if (!ret)
		ret = regmap_clear_bits(rdev->regmap, l->set_reg, l->set_mask);
	return ret;
}

static int sc2713s_ldo_disable(struct regulator_dev *rdev)
{
	struct sc2713s_ldo *l = rdev_get_drvdata(rdev);
	int ret;

	ret = regmap_set_bits(rdev->regmap, l->set_reg, l->set_mask);
	if (!ret && l->clr_mask)
		ret = regmap_clear_bits(rdev->regmap, l->clr_reg, l->clr_mask);
	return ret;
}

static int sc2713s_ldo_is_enabled(struct regulator_dev *rdev)
{
	struct sc2713s_ldo *l = rdev_get_drvdata(rdev);
	unsigned int v;
	int ret;

	if (l->clr_mask) {
		ret = regmap_read(rdev->regmap, l->clr_reg, &v);
		return ret ?: !!(v & l->clr_mask);
	}
	ret = regmap_read(rdev->regmap, l->set_reg, &v);
	return ret ?: !(v & l->set_mask);
}

static const struct regulator_ops sc2713s_ldo_ops = {
	.enable = sc2713s_ldo_enable,
	.disable = sc2713s_ldo_disable,
	.is_enabled = sc2713s_ldo_is_enabled,
	.list_voltage = regulator_list_voltage_table,
	.map_voltage = regulator_map_voltage_iterate,
	.get_voltage_sel = regulator_get_voltage_sel_regmap,
	.set_voltage_sel = regulator_set_voltage_sel_regmap,
};

/* the sc27xx MFD's ADI transport (drivers/mfd/sprd-sc27xx-spi.c) */
static int sc2713s_write(void *context, const void *data, size_t count)
{
	return spi_write(to_spi_device(context), data, count);
}

static int sc2713s_read(void *context, const void *reg, size_t reg_size,
			void *val, size_t val_size)
{
	u32 buf[2] = { 0 };
	int ret;

	if (reg_size != sizeof(u32) || val_size != sizeof(u32))
		return -EINVAL;
	memcpy(buf, reg, sizeof(u32));
	ret = spi_read(to_spi_device(context), buf, 1);
	if (ret < 0)
		return ret;
	memcpy(val, buf, sizeof(u32));
	return 0;
}

static const struct regmap_bus sc2713s_bus = {
	.write = sc2713s_write,
	.read = sc2713s_read,
	.reg_format_endian_default = REGMAP_ENDIAN_NATIVE,
	.val_format_endian_default = REGMAP_ENDIAN_NATIVE,
};

static const struct regmap_config sc2713s_regmap = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = 0xffc,
};

static int sc2713s_ldo_register(struct device *dev, struct regmap *map,
				struct device_node *np)
{
	struct regulator_config cfg = { .dev = dev, .regmap = map, .of_node = np };
	struct regulator_dev *rdev;
	struct sc2713s_ldo *l;
	u32 r[2], vsel[2];

	l = devm_kzalloc(dev, sizeof(*l), GFP_KERNEL);
	if (!l)
		return -ENOMEM;
	if (of_property_read_u32_array(np, "sprd,pd-set", r, 2))
		return dev_err_probe(dev, -EINVAL, "%pOFn: no sprd,pd-set\n", np);
	l->set_reg = r[0];
	l->set_mask = r[1];
	if (!of_property_read_u32_array(np, "sprd,pd-clr", r, 2)) {
		l->clr_reg = r[0];
		l->clr_mask = r[1];
	}

	l->desc.name = np->name;
	l->desc.type = REGULATOR_VOLTAGE;
	l->desc.owner = THIS_MODULE;
	l->desc.ops = &sc2713s_ldo_ops;
	if (!of_property_read_u32_array(np, "sprd,vsel", vsel, 2) &&
	    !of_property_read_u32_array(np, "sprd,voltages", l->volts, 4)) {
		l->desc.vsel_reg = vsel[0];
		l->desc.vsel_mask = vsel[1];
		l->desc.volt_table = l->volts;
		l->desc.n_voltages = 4;
	}

	cfg.driver_data = l;
	cfg.init_data = of_get_regulator_init_data(dev, np, &l->desc);
	rdev = devm_regulator_register(dev, &l->desc, &cfg);
	if (IS_ERR(rdev))
		return dev_err_probe(dev, PTR_ERR(rdev), "%pOFn\n", np);
	return 0;
}

/*
 * The PMIC's watchdog as /dev/watchdog. The frame userland's watchdog feeds
 * /dev/watchdog only while the frame is well (a byte every 5 s, 16 s
 * timeout on the A33) and its A/B rollback leans on that, and until now only the initramfs's
 * pmic-wdt script fed this one -- which dies with the initramfs at
 * switch_root. Same registers as the sprd-adi restart handler and pmic-wdt
 * (offsets from 0x40038000): LOAD_LOW 0x40, LOAD_HIGH 0x44, CTRL 0x48
 * (RUN bit 1, NEW = 32-bit load bit 2, RST_EN bit 3), LOCK 0x60 (0xe551
 * unlocks), the module and RTC clocks at 0x800/0x808 bit 2, 32768 Hz.
 *
 * Caveat: a timeout reset doesn't go through the restart handler, so it
 * doesn't power-cycle the eMMC first (kernel/patches/0002); a hang in the
 * middle of an eMMC transfer can still leave the boot ROM unable to read the
 * card ("MCI ERF") until a power cycle.
 */
#define WDG_LOAD_LOW	0x40
#define WDG_LOAD_HIGH	0x44
#define WDG_CTRL	0x48
#define  WDG_RUN	BIT(1)
#define  WDG_NEW	BIT(2)
#define  WDG_RST_EN	BIT(3)
#define WDG_LOCK	0x60
#define  WDG_UNLOCK_KEY	0xe551
#define GLB_ARM_MODULE_EN	0x800
#define GLB_RTC_CLK_EN		0x808
#define GLB_POR_RST_MONITOR	0x8e8
#define  WDG_EN		BIT(2)

static int sc2713s_wdt_load(struct regmap *m, unsigned int timeout)
{
	u32 n = timeout * 32768;

	regmap_write(m, WDG_LOCK, WDG_UNLOCK_KEY);
	regmap_write(m, WDG_LOAD_HIGH, n >> 16);
	regmap_write(m, WDG_LOAD_LOW, n & 0xffff);
	return regmap_write(m, WDG_LOCK, ~WDG_UNLOCK_KEY & 0xffff);
}

static int sc2713s_wdt_start(struct watchdog_device *w)
{
	struct regmap *m = watchdog_get_drvdata(w);

	/* a timeout reset should read as an ordinary reboot to U-Boot:
	 * reason NORMAL (0x40), as the restart handler records it */
	regmap_update_bits(m, GLB_POR_RST_MONITOR, 0xf0, 0x40);
	regmap_write(m, WDG_LOCK, WDG_UNLOCK_KEY);
	regmap_set_bits(m, WDG_CTRL, WDG_NEW);
	sc2713s_wdt_load(m, w->timeout);
	regmap_write(m, WDG_LOCK, WDG_UNLOCK_KEY);
	regmap_set_bits(m, WDG_CTRL, WDG_RUN | WDG_RST_EN);
	return regmap_write(m, WDG_LOCK, ~WDG_UNLOCK_KEY & 0xffff);
}

static int sc2713s_wdt_stop(struct watchdog_device *w)
{
	struct regmap *m = watchdog_get_drvdata(w);

	regmap_write(m, WDG_LOCK, WDG_UNLOCK_KEY);
	regmap_clear_bits(m, WDG_CTRL, WDG_RUN | WDG_RST_EN);
	return regmap_write(m, WDG_LOCK, ~WDG_UNLOCK_KEY & 0xffff);
}

static int sc2713s_wdt_ping(struct watchdog_device *w)
{
	return sc2713s_wdt_load(watchdog_get_drvdata(w), w->timeout);
}

static int sc2713s_wdt_set_timeout(struct watchdog_device *w, unsigned int t)
{
	w->timeout = t;
	return watchdog_active(w) ? sc2713s_wdt_ping(w) : 0;
}

static const struct watchdog_info sc2713s_wdt_info = {
	.options = WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING | WDIOF_MAGICCLOSE,
	.identity = "SC2713S PMIC watchdog",
};

static const struct watchdog_ops sc2713s_wdt_ops = {
	.owner = THIS_MODULE,
	.start = sc2713s_wdt_start,
	.stop = sc2713s_wdt_stop,
	.ping = sc2713s_wdt_ping,
	.set_timeout = sc2713s_wdt_set_timeout,
};

static int sc2713s_wdt_register(struct device *dev, struct regmap *map)
{
	struct watchdog_device *w;
	unsigned int ctrl;

	w = devm_kzalloc(dev, sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;
	w->info = &sc2713s_wdt_info;
	w->ops = &sc2713s_wdt_ops;
	w->min_timeout = 1;
	w->max_timeout = 3600;
	w->timeout = 16;	/* the userland's feeder was written for 16 s */
	w->parent = dev;
	watchdog_init_timeout(w, 0, dev);
	watchdog_set_drvdata(w, map);
	watchdog_stop_on_reboot(w);

	regmap_set_bits(map, GLB_ARM_MODULE_EN, WDG_EN);
	regmap_set_bits(map, GLB_RTC_CLK_EN, WDG_EN);
	/* armed by U-Boot (uboot-boot.py --wdt) or an earlier kernel: keep
	 * feeding it from the kernel until userspace takes over */
	if (!regmap_read(map, WDG_CTRL, &ctrl) && (ctrl & WDG_RUN)) {
		sc2713s_wdt_ping(w);
		set_bit(WDOG_HW_RUNNING, &w->status);
	}
	return devm_watchdog_register_device(dev, w);
}

static int sc2713s_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct device_node *regs;
	struct regmap *map;
	int ret = 0;

	map = devm_regmap_init(dev, &sc2713s_bus, dev, &sc2713s_regmap);
	if (IS_ERR(map))
		return PTR_ERR(map);

	ret = sc2713s_wdt_register(dev, map);
	if (ret)
		dev_warn(dev, "no watchdog: %d\n", ret);

	/* the PMIC's other functions with a node of their own (the audio
	 * codec, sc2713s-codec.c) take this regmap from their parent */
	ret = devm_of_platform_populate(dev);
	if (ret)
		dev_warn(dev, "children: %d\n", ret);

	regs = of_get_child_by_name(dev->of_node, "regulators");
	if (!regs)
		return 0;
	for_each_available_child_of_node_scoped(regs, np) {
		ret = sc2713s_ldo_register(dev, map, np);
		if (ret)
			break;
	}
	of_node_put(regs);
	return ret;
}

static const struct of_device_id sc2713s_of_match[] = {
	{ .compatible = "sprd,sc2713s" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc2713s_of_match);

static const struct spi_device_id sc2713s_spi_ids[] = {
	{ "sc2713s" },
	{ }
};
MODULE_DEVICE_TABLE(spi, sc2713s_spi_ids);

static struct spi_driver sc2713s_driver = {
	.driver = {
		.name = "regulator-sc2713s",
		.of_match_table = sc2713s_of_match,
	},
	.probe = sc2713s_probe,
	.id_table = sc2713s_spi_ids,
};
module_spi_driver(sc2713s_driver);

MODULE_DESCRIPTION("Spreadtrum SC2713S PMIC LDOs");
MODULE_LICENSE("GPL");
