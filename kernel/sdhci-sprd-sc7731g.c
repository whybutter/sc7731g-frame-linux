// SPDX-License-Identifier: GPL-2.0-only
/*
 * SD/eMMC host of the Spreadtrum SC7731G (SC8830 family, the vendor's
 * "sprd,sdhost-3.0").
 *
 * Mainline's sdhci-sprd is for the r11 controller (SC9860 on): SDHCI v4 mode,
 * a PHY DLL at 0x200, 64-bit ADMA. This one is the generation before, an
 * SDHCI 3.0 register file the vendor drove with its own driver
 * (drivers/mmc/host/sdhost.c in Samsung's SM-G531H GPL kernel) rather than
 * the sdhci core. What that driver does differently from a standard host is
 * all there is here:
 *
 *  - The clock divider is off by one: SD clock = base / (2 * (N + 1)), with N
 *    in the standard 10-bit field (vendor _sdhost_calcDiv). The standard one
 *    is base / (2 * N).
 *  - Bit 3 of SOFTWARE_RESET is Spreadtrum's eMMC hardware-reset line, active
 *    low: the vendor writes it set with every reset (_RST_EMMC), and clearing
 *    it holds the card in reset. mainline's sdhci-sprd has the same trap.
 *  - Three delay registers outside the standard map, write (0x80), positive-
 *    and negative-edge read (0x84, 0x88), written after every full reset. The
 *    values are the vendor DT's writeDelay/readPosDelay/readNegDelay.
 *  - The vendor never sets HOST_CONTROL's high-speed bit (its
 *    _sdhost_enable_HISPD calls are commented out) and runs eMMC high speed
 *    with HOST_CONTROL2 left at SDR12; we do the same until something faster
 *    has been seen to work.
 *
 * The base clock (384 MHz, the vendor's base_clk) comes from `clocks`. There
 * is no clock driver yet: U-Boot leaves clk_emmc enabled and muxed (it read
 * our kernel off this eMMC), and the dts says what rate that is.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>

#include "sdhci-pltfm.h"

#define SC7731G_WR_DLY		0x80
#define SC7731G_RD_POS_DLY	0x84
#define SC7731G_RD_NEG_DLY	0x88

#define SC7731G_RST_EMMC	BIT(3)

/*
 * pio=1 (sdhci_sprd_sc7731g.pio=1 on the command line): no DMA. Writes from
 * our Linux hung on 2026-10-27 -- CMD25 with SDMA, transfer active, no
 * interrupt pending, the SDMA address register stopped at an odd value --
 * while reads by the same path had always worked. PIO takes the DMA engine
 * out of it, for a test and, if need be, for good.
 */
static bool pio;
module_param(pio, bool, 0444);

struct sc7731g_sdhci {
	u32 delay[3];
};

static void sc7731g_writeb(struct sdhci_host *host, u8 val, int reg)
{
	if (reg == SDHCI_SOFTWARE_RESET)
		val |= SC7731G_RST_EMMC;
	writeb(val, host->ioaddr + reg);
}

/*
 * Pulse the card's RST_n (bit 3, active low; see the header). This eMMC has
 * RST_n enabled for good (EXT_CSD[162] RST_n_FUNCTION = 1, read off the
 * frame 2026-10-27), so the pulse puts it back in its power-on state.
 *
 * Why it matters: the boot ROM reads the card after a warm reset, and twice
 * found it unreadable ("MCI ERF" on the UART, then nothing until a power
 * cycle): once after a watchdog reset mid card-init (2026-10-24), once after
 * a clean reboot that followed a 1 MiB read of a partition (docs/wifi.md,
 * wifi1). Reboots after only the partition scan came back. The
 * vendor kernel did nothing special here and still rebooted fine -- whatever
 * mainline leaves the card in (asleep after mmc_shutdown, or mid-transfer
 * on a panic), the ROM can't take it, and the reset pin makes the question
 * moot. Timings: tRSTW >= 1 us low, tRSCA >= 200 us before the first command.
 *
 * IT DOESN'T: tested 2026-10-27 -- a 1 ms pulse of this bit from user space,
 * and a read right after still worked (a reset card would be in pre-idle and
 * fail it), and the warm reset after a 1 MiB read still left the ROM with
 * MCI ERF. Bit 3 isn't reaching the card's RST_n on this board (or isn't
 * RST_n). The restart path now power-cycles the card through its PMIC LDOs
 * instead (kernel/patches/0002-spi-sprd-adi-emmc-power-cycle.patch); this
 * stays as the documented negative, and is harmless.
 */
static void sc7731g_pulse_rst_n(void __iomem *ioaddr)
{
	u8 v = readb(ioaddr + SDHCI_SOFTWARE_RESET);

	writeb(v & ~SC7731G_RST_EMMC, ioaddr + SDHCI_SOFTWARE_RESET);
	udelay(10);
	writeb(v | SC7731G_RST_EMMC, ioaddr + SDHCI_SOFTWARE_RESET);
	udelay(300);
}

/* the mmc core's card reset, for error recovery (cap-mmc-hw-reset) */
static void sc7731g_hw_reset(struct sdhci_host *host)
{
	sc7731g_pulse_rst_n(host->ioaddr);
}

/*
 * ...and on every restart, ahead of the PMIC watchdog reset (sprd-adi's
 * handler, default priority): a restart handler rather than .shutdown, so
 * panic and emergency restarts, which skip device shutdown, get it too.
 */
static int sc7731g_restart(struct sys_off_data *data)
{
	struct sdhci_host *host = data->cb_data;

	sc7731g_pulse_rst_n(host->ioaddr);
	return NOTIFY_DONE;
}

static void sc7731g_set_clock(struct sdhci_host *host, unsigned int clock)
{
	unsigned int base = host->max_clk;
	u32 n = 0;
	u16 clk;

	host->mmc->actual_clock = 0;
	sdhci_writew(host, 0, SDHCI_CLOCK_CONTROL);
	if (!clock)
		return;

	/* vendor _sdhost_calcDiv: the smallest N with base/(2(N+1)) <= clock */
	if (base > clock) {
		n = DIV_ROUND_UP(base, 2 * clock) - 1;
		n = min_t(u32, n, 0x3ff);
	}
	host->mmc->actual_clock = base / (2 * (n + 1));

	clk = ((n & 0xff) << SDHCI_DIVIDER_SHIFT) |
	      (((n >> 8) & 0x3) << SDHCI_DIVIDER_HI_SHIFT);
	sdhci_enable_clk(host, clk);
}

static void sc7731g_reset(struct sdhci_host *host, u8 mask)
{
	struct sc7731g_sdhci *p = sdhci_pltfm_priv(sdhci_priv(host));

	sdhci_reset(host, mask);
	if (mask & SDHCI_RESET_ALL) {
		sdhci_writel(host, p->delay[0], SC7731G_WR_DLY);
		sdhci_writel(host, p->delay[1], SC7731G_RD_POS_DLY);
		sdhci_writel(host, p->delay[2], SC7731G_RD_NEG_DLY);
	}
}

static void sc7731g_set_uhs_signaling(struct sdhci_host *host,
				      unsigned int timing)
{
	u16 ctrl2 = sdhci_readw(host, SDHCI_HOST_CONTROL2) & ~SDHCI_CTRL_UHS_MASK;

	switch (timing) {
	case MMC_TIMING_UHS_SDR25:
		ctrl2 |= SDHCI_CTRL_UHS_SDR25;
		break;
	case MMC_TIMING_UHS_SDR50:
		ctrl2 |= SDHCI_CTRL_UHS_SDR50;
		break;
	case MMC_TIMING_UHS_SDR104:
	case MMC_TIMING_MMC_HS200:
		ctrl2 |= SDHCI_CTRL_UHS_SDR104;
		break;
	case MMC_TIMING_UHS_DDR50:
	case MMC_TIMING_MMC_DDR52:
		ctrl2 |= SDHCI_CTRL_UHS_DDR50;
		break;
	default:
		/* legacy, SD/MMC high speed, SDR12: the vendor's SDR12 */
		break;
	}
	sdhci_writew(host, ctrl2, SDHCI_HOST_CONTROL2);
}

static unsigned int sc7731g_get_min_clock(struct sdhci_host *host)
{
	return host->max_clk / (2 * 0x400);
}

static const struct sdhci_ops sc7731g_sdhci_ops = {
	.write_b		= sc7731g_writeb,
	.set_clock		= sc7731g_set_clock,
	.get_max_clock		= sdhci_pltfm_clk_get_max_clock,
	.get_min_clock		= sc7731g_get_min_clock,
	.set_bus_width		= sdhci_set_bus_width,
	.reset			= sc7731g_reset,
	.set_uhs_signaling	= sc7731g_set_uhs_signaling,
	.hw_reset		= sc7731g_hw_reset,
};

static const struct sdhci_pltfm_data sc7731g_sdhci_pdata = {
	.ops	 = &sc7731g_sdhci_ops,
	/* the vendor computes data timeouts in SD clock cycles
	 * (_sdhost_calcTimeout), not from a capability timeout clock */
	.quirks	 = SDHCI_QUIRK_NO_HISPD_BIT |
		   SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN |
		   SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK,
	.quirks2 = SDHCI_QUIRK2_PRESET_VALUE_BROKEN,
};

static int sc7731g_sdhci_probe(struct platform_device *pdev)
{
	struct sdhci_pltfm_host *pltfm;
	struct sc7731g_sdhci *p;
	struct sdhci_host *host;
	int ret;

	host = sdhci_pltfm_init(pdev, &sc7731g_sdhci_pdata, sizeof(*p));
	if (IS_ERR(host))
		return PTR_ERR(host);
	if (pio) {
		host->quirks |= SDHCI_QUIRK_BROKEN_DMA | SDHCI_QUIRK_BROKEN_ADMA;
		dev_info(&pdev->dev, "PIO only (pio=1)\n");
	}
	pltfm = sdhci_priv(host);
	p = sdhci_pltfm_priv(pltfm);

	/* the vendor DT has 4 for all three on every slot */
	p->delay[0] = p->delay[1] = p->delay[2] = 4;
	of_property_read_u32_array(pdev->dev.of_node, "sprd,delays", p->delay, 3);

	pltfm->clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(pltfm->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(pltfm->clk), "no clock\n");

	ret = mmc_of_parse(host->mmc);
	if (ret)
		return ret;
	sdhci_get_of_property(pdev);

	ret = devm_register_sys_off_handler(&pdev->dev, SYS_OFF_MODE_RESTART,
					    SYS_OFF_PRIO_HIGH, sc7731g_restart, host);
	if (ret)
		return ret;

	return sdhci_add_host(host);
}

static const struct of_device_id sc7731g_sdhci_of_match[] = {
	{ .compatible = "sprd,sc7731g-sdhci" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc7731g_sdhci_of_match);

static struct platform_driver sc7731g_sdhci_driver = {
	.driver = {
		.name = "sdhci-sprd-sc7731g",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = sc7731g_sdhci_of_match,
	},
	.probe = sc7731g_sdhci_probe,
	.remove = sdhci_pltfm_remove,
};
module_platform_driver(sc7731g_sdhci_driver);

MODULE_DESCRIPTION("Spreadtrum SC7731G SD/eMMC host");
MODULE_LICENSE("GPL");
