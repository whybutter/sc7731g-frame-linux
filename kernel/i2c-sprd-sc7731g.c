// SPDX-License-Identifier: GPL-2.0-only
/*
 * I2C controller of the Spreadtrum SC7731G (SC8830 family, the vendor's
 * "sprd,i2c"; the bus the gslX680 touch controller is on).
 *
 * Mainline's i2c-sprd is the SC9860's controller, a different design (byte
 * counts and FIFOs). This one is driven a byte at a time through one command
 * register: write the byte and the START/WRITE/READ/STOP/TX_ACK bits to
 * I2C_CMD, wait for CTL.INT, acknowledge it, check the ACK bit. Ported from
 * the vendor's drivers/i2c/busses/i2c-sprd.c (Samsung SM-G531H GPL kernel),
 * which polls the same way -- the vendor never used the interrupt either,
 * and at 100-400 kHz a byte is 20-90 us.
 *
 * Bus clock: SCL = clk / (4 * (div + 1)), div split over CLKD0 (low 16 bits)
 * and CLKD1 (high). The vendor assumed 26 MHz; we read the rate of the clock
 * the dts gives (muxed to ext_26m by assigned-clock-parents).
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#define I2C_CTL			0x00
#define  CTL_INT		BIT(0)
#define  CTL_IE			BIT(3)
#define  CTL_EN			BIT(4)
#define I2C_CMD			0x04
#define  CMD_INT_ACK		BIT(0)
#define  CMD_TX_ACK		BIT(1)
#define  CMD_WRITE		BIT(2)
#define  CMD_READ		BIT(3)
#define  CMD_STOP		BIT(4)
#define  CMD_START		BIT(5)
#define  CMD_ACK		BIT(6)	/* set: the target did NOT ack */
#define  CMD_BUSY		BIT(7)
#define I2C_CLKD0		0x08
#define I2C_CLKD1		0x0c
#define I2C_RST			0x10
#define I2C_CMD_BUF_CTL		0x18
#define I2C_STA_STO_DVD		0x1c

struct sc7731g_i2c {
	struct i2c_adapter adap;
	struct device *dev;
	void __iomem *base;
	struct clk *clk;
	u32 version;
};

/* the vendor's poll loops: 5000 x 1 us each */
static int sc7731g_i2c_byte(struct sc7731g_i2c *i, u32 cmd, u8 *rx)
{
	u32 v;
	int ret;

	writel(cmd, i->base + I2C_CMD);
	ret = readl_poll_timeout_atomic(i->base + I2C_CTL, v, v & CTL_INT, 1, 5000);
	if (ret)
		return ret;
	/* acknowledge the interrupt without disturbing the data byte */
	ret = readl_poll_timeout_atomic(i->base + I2C_CMD, v, !(v & CMD_BUSY), 1, 5000);
	if (ret)
		return ret;
	writel((readl(i->base + I2C_CMD) & 0xff00) | CMD_INT_ACK, i->base + I2C_CMD);
	if (rx)
		*rx = readl(i->base + I2C_CMD) >> 8;
	ret = readl_poll_timeout_atomic(i->base + I2C_CMD, v, !(v & CMD_ACK), 1, 5000);
	return ret ? -ENXIO : 0;
}

static int sc7731g_i2c_msg(struct sc7731g_i2c *i, struct i2c_msg *m, bool last)
{
	u32 addr = i2c_8bit_addr_from_msg(m);
	int n, ret;

	writel(readl(i->base + I2C_CTL) | CTL_EN | CTL_IE, i->base + I2C_CTL);
	ret = sc7731g_i2c_byte(i, addr << 8 | CMD_START | CMD_WRITE, NULL);
	if (ret)
		goto stop;

	for (n = 0; n < m->len; n++) {
		bool end = n == m->len - 1;

		if (m->flags & I2C_M_RD)
			ret = sc7731g_i2c_byte(i, CMD_READ |
					       (end ? CMD_STOP | CMD_TX_ACK : 0),
					       &m->buf[n]);
		else
			ret = sc7731g_i2c_byte(i, m->buf[n] << 8 | CMD_WRITE |
					       (end && last ? CMD_STOP : 0), NULL);
		if (ret)
			goto stop;
	}
	return 0;

stop:
	/* release the bus so the next transfer starts clean */
	writel(CMD_STOP | CMD_WRITE, i->base + I2C_CMD);
	udelay(20);
	writel((readl(i->base + I2C_CMD) & 0xff00) | CMD_INT_ACK, i->base + I2C_CMD);
	return ret;
}

static int sc7731g_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct sc7731g_i2c *i = i2c_get_adapdata(adap);
	int n, ret;

	for (n = 0; n < num; n++) {
		if (msgs[n].flags & I2C_M_TEN)
			return -EOPNOTSUPP;
		ret = sc7731g_i2c_msg(i, &msgs[n], n == num - 1);
		if (ret)
			return ret;
	}
	return num;
}

static u32 sc7731g_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm sc7731g_i2c_algo = {
	.xfer = sc7731g_i2c_xfer,
	.functionality = sc7731g_i2c_func,
};

static void sc7731g_i2c_hw_init(struct sc7731g_i2c *i, u32 freq)
{
	unsigned long rate = clk_get_rate(i->clk) ?: 26000000;
	u32 div = rate / (4 * freq) - 1;
	u32 ctl = readl(i->base + I2C_CTL);

	writel(ctl & ~(CTL_EN | CTL_IE), i->base + I2C_CTL);
	writel(0, i->base + I2C_CMD_BUF_CTL);	/* no command-buffer mode */
	writel(div & 0xffff, i->base + I2C_CLKD0);
	writel(div >> 16, i->base + I2C_CLKD1);
	i->version = readl(i->base + I2C_CTL) >> 16;
	/* r7p0 and later: the vendor's fix for the 400 kHz start time */
	if (freq == 400000 && (i->version >> 8) >= 7)
		writel(0x10, i->base + I2C_STA_STO_DVD);
	writel(readl(i->base + I2C_CTL) | CTL_EN | CTL_IE, i->base + I2C_CTL);
	writel(CMD_INT_ACK, i->base + I2C_CMD);
}

static int sc7731g_i2c_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sc7731g_i2c *i;
	u32 freq = I2C_MAX_STANDARD_MODE_FREQ;

	i = devm_kzalloc(dev, sizeof(*i), GFP_KERNEL);
	if (!i)
		return -ENOMEM;
	i->dev = dev;
	i->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(i->base))
		return PTR_ERR(i->base);
	i->clk = devm_clk_get_optional_enabled(dev, NULL);
	if (IS_ERR(i->clk))
		return dev_err_probe(dev, PTR_ERR(i->clk), "no clock\n");
	of_property_read_u32(dev->of_node, "clock-frequency", &freq);

	sc7731g_i2c_hw_init(i, freq);

	i->adap.owner = THIS_MODULE;
	i->adap.algo = &sc7731g_i2c_algo;
	i->adap.dev.parent = dev;
	i->adap.dev.of_node = dev->of_node;
	i->adap.retries = 2;
	i->adap.nr = of_alias_get_id(dev->of_node, "i2c");
	strscpy(i->adap.name, dev_name(dev), sizeof(i->adap.name));
	i2c_set_adapdata(&i->adap, i);
	platform_set_drvdata(pdev, i);

	dev_info(dev, "controller r%up%u, %u Hz from %lu Hz\n", i->version >> 8,
		 i->version & 0xff, freq, clk_get_rate(i->clk));
	return i2c_add_numbered_adapter(&i->adap);
}

static void sc7731g_i2c_remove(struct platform_device *pdev)
{
	struct sc7731g_i2c *i = platform_get_drvdata(pdev);

	i2c_del_adapter(&i->adap);
}

static const struct of_device_id sc7731g_i2c_of_match[] = {
	{ .compatible = "sprd,sc7731g-i2c" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc7731g_i2c_of_match);

static struct platform_driver sc7731g_i2c_driver = {
	.driver = {
		.name = "i2c-sprd-sc7731g",
		.of_match_table = sc7731g_i2c_of_match,
	},
	.probe = sc7731g_i2c_probe,
	.remove = sc7731g_i2c_remove,
};
module_platform_driver(sc7731g_i2c_driver);

MODULE_DESCRIPTION("Spreadtrum SC7731G I2C controller");
MODULE_LICENSE("GPL");
