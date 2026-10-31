// SPDX-License-Identifier: GPL-2.0-only
/*
 * USB 2.0 PHY of the Spreadtrum SC7731G, in front of its DWC2 (Synopsys HS
 * OTG) controller, which mainline's dwc2 drives.
 *
 * The PHY has no registers of its own: it is switches in the global blocks,
 * thrown in the order of the vendor's usb_startup() (drivers/usb/gadget/
 * dwc_otg/usb_hw.c, Samsung SM-G531H GPL kernel, the SCX30G branch):
 *
 *   1. the USB LDO on (vddusb, a PMIC LDO: `vdd-supply` when there is a
 *      regulator for it; until then whatever U-Boot/the PMIC defaults left)
 *   2. AON_APB PWR_CTRL bit 0 off -- the PHY's power-down
 *   3. AP_AHB AHB_RST bits 5-7 (controller, UTMI, PHY soft resets) pulsed 5 ms
 *   4. AP_APB USB_PHY_TUNE written with the board's tune word (vendor DT
 *      `tune_value`, 0x44073e33 on this board, "from uboot")
 *
 * The controller's AHB gate (AP_AHB EB bit 4) is clk_usb_ref's enable in the
 * clock tree, which dwc2 turns on as its "otg" clock.
 */

#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#define AON_APB_PWR_CTRL	0x24
#define  USB_PHY_PD		BIT(0)
#define AP_AHB_AHB_RST		0x04
#define  USB_SOFT_RSTS		(BIT(5) | BIT(6) | BIT(7))
#define AP_APB_USB_PHY_TUNE	0x3000

struct sc7731g_usb_phy {
	struct regmap *aon_apb, *ap_ahb, *ap_apb;
	struct regulator *vdd;
	u32 tune;
};

static int sc7731g_usb_phy_power_on(struct phy *phy)
{
	struct sc7731g_usb_phy *p = phy_get_drvdata(phy);
	int ret;

	ret = regulator_enable(p->vdd);
	if (ret)
		return ret;
	mdelay(10);
	regmap_clear_bits(p->aon_apb, AON_APB_PWR_CTRL, USB_PHY_PD);
	mdelay(2);
	regmap_set_bits(p->ap_ahb, AP_AHB_AHB_RST, USB_SOFT_RSTS);
	mdelay(5);
	regmap_clear_bits(p->ap_ahb, AP_AHB_AHB_RST, USB_SOFT_RSTS);
	mdelay(3);
	regmap_write(p->ap_apb, AP_APB_USB_PHY_TUNE, p->tune);
	return 0;
}

static int sc7731g_usb_phy_power_off(struct phy *phy)
{
	struct sc7731g_usb_phy *p = phy_get_drvdata(phy);

	regmap_set_bits(p->aon_apb, AON_APB_PWR_CTRL, USB_PHY_PD);
	return regulator_disable(p->vdd);
}

static const struct phy_ops sc7731g_usb_phy_ops = {
	.power_on = sc7731g_usb_phy_power_on,
	.power_off = sc7731g_usb_phy_power_off,
	.owner = THIS_MODULE,
};

static int sc7731g_usb_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct phy_provider *provider;
	struct sc7731g_usb_phy *p;
	struct phy *phy;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->aon_apb = syscon_regmap_lookup_by_phandle(np, "sprd,aon-apb");
	p->ap_ahb = syscon_regmap_lookup_by_phandle(np, "sprd,ap-ahb");
	p->ap_apb = syscon_regmap_lookup_by_phandle(np, "sprd,ap-apb");
	if (IS_ERR(p->aon_apb) || IS_ERR(p->ap_ahb) || IS_ERR(p->ap_apb))
		return dev_err_probe(dev, -ENODEV, "missing syscon\n");
	p->tune = 0x44073e33;
	of_property_read_u32(np, "sprd,tune", &p->tune);
	p->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(p->vdd))
		return PTR_ERR(p->vdd);

	phy = devm_phy_create(dev, NULL, &sc7731g_usb_phy_ops);
	if (IS_ERR(phy))
		return PTR_ERR(phy);
	phy_set_drvdata(phy, p);
	provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);
	return PTR_ERR_OR_ZERO(provider);
}

static const struct of_device_id sc7731g_usb_phy_of_match[] = {
	{ .compatible = "sprd,sc7731g-usb-phy" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc7731g_usb_phy_of_match);

static struct platform_driver sc7731g_usb_phy_driver = {
	.driver = {
		.name = "phy-sprd-sc7731g-usb",
		.of_match_table = sc7731g_usb_phy_of_match,
	},
	.probe = sc7731g_usb_phy_probe,
};
module_platform_driver(sc7731g_usb_phy_driver);

MODULE_DESCRIPTION("Spreadtrum SC7731G USB 2.0 PHY");
MODULE_LICENSE("GPL");
