// SPDX-License-Identifier: GPL-2.0-only
/*
 * Secondary CPU bring-up for the Spreadtrum SC7731G (SC8830 family).
 *
 * After reset, cores 1-3 sit in the boot ROM, powered down. The ROM releases
 * core N when bit N of HOLDING_PEN (AP_AHB + 0x14) is set, and jumps to the
 * address in CPU_JUMP[N] (AP_AHB + 0x18 + 4*N, so slot 0 is never used). The
 * core also needs power: its PMU_APB power domain (PD_CA7_Cn_CFG, + 0x8 for
 * C1) is switched to auto-shutdown and out of force-shutdown, with its reset
 * held in AP_AHB CA7_RST_SET while that happens.
 *
 * All of it is the vendor's drivers/platform/sprd/platsmp.c (Samsung's
 * SM-G531H GPL kernel, 3.10.65), CONFIG_ARCH_SCX35 branch, minus its own
 * pen_release handshake: the vendor jumped the core into a second holding pen
 * in the kernel (sci_secondary_startup), a pattern mainline dropped. Jumping
 * straight to secondary_startup is what sunxi and others do -- the generic ARM
 * code has the core's stack and page tables ready in secondary_data before
 * boot_secondary() runs.
 *
 * No hotplug (cpu_die/cpu_kill) yet: the vendor's powerdown path also needs
 * the core to park in WFI with its caches cleaned, and nothing here needs a
 * core taken offline.
 */

#include <linux/delay.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/smp.h>

#include <asm/cacheflush.h>
#include <asm/smp_plat.h>

#define AP_AHB_CA7_RST_SET	0x08
#define AP_AHB_HOLDING_PEN	0x14
#define AP_AHB_CPU_JUMP(n)	(0x18 + 4 * (n))
#define AP_AHB_MCU_PAUSE	0x3004
/* LIGHT_SLEEP_EN (3) | DEEP_SLEEP_EN (2) | SYS_SLEEP_EN (1) | CORE_SLEEP (0) */
#define MCU_SLEEP_ENABLES	0xf

#define PMU_APB_PD_CA7_CFG(n)	(0x08 + 4 * ((n) - 1))
#define PD_FORCE_SHUTDOWN	BIT(25)
#define PD_AUTO_SHUTDOWN_EN	BIT(24)
/* PWR_ON_DLY(4) | PWR_ON_SEQ_DLY(4) | ISO_ON_DLY(4), the vendor's values */
#define PD_DELAYS		((4 << 16) | (4 << 8) | 4)

static void __iomem *ap_ahb;
static void __iomem *pmu_apb;

#define PMU_APB_PD_CA7_TOP_CFG	0x00
#define PMU_APB_PD_CA7_C0_CFG	0x04
#define MCU_SLEEP_FOLLOW_CA7	BIT(4)

/*
 * sprd_noautopd: disarm the power domains' automatic shutdown. NOT the fix
 * (the idle matrix died with it as without it; the auto gates below were
 * it), kept as a switch for whoever revisits cluster power-down. The hangs of
 * 2026-10-27 all came as the system went idle (a probe's first `sleep 1`;
 * `nohlt`, which idles by polling instead of WFI, got past it), and the
 * probe showed CA7_TOP and CA7_C0 left with AUTO_SHUTDOWN_EN set by U-Boot
 * (0x0108xxxx), cores 1-3 with it set by us (the vendor does on power-up),
 * and MCU_PAUSE with SLEEP_FOLLOW_CA7. If WFI on every core is the PMU's
 * cue to power the cluster down, nothing here can bring it back (the
 * vendor's resume path is an iRAM vector this kernel doesn't set up). A
 * parameter rather than a fix, so one build can test both ways.
 */
static bool noautopd;

/*
 * AP_SYS_AUTO_SLEEP_CFG's clock auto-gating, cleared at every boot: this was
 * the early hang. The idle matrix of 2026-10-27 (initramfs `idletest`, 150 s
 * of `sleep 1`): plain WFI dead at the first sleep, sprd_noautopd too,
 * `nohlt` alive throughout, and with these gates cleared WFI alive
 * throughout as well. U-Boot leaves the register at 0x3B -- CA7 core, CA7
 * debug, CA7 EMC, AP EMC and AP AHB auto gates on, the value the vendor
 * writes only on its way into deep sleep (pm-scx35.c disable_ahb_module).
 * With them set, every core in WFI gates the cluster's and the bus's
 * clocks, and the wake has to come through Spreadtrum's own interrupt
 * controller mirroring the GIC, which the vendor kernel enables line by line
 * and mainline never touches -- nothing wakes. Gating idle clocks again
 * would need that controller driven (and is a power saving, not a need).
 */
#define AP_AHB_AP_SYS_AUTO_SLEEP_CFG	0x10
#define AUTO_GATES	(BIT(5) | BIT(4) | BIT(3) | BIT(1) | BIT(0))

static int __init sc7731g_noautopd(char *s)
{
	noautopd = true;
	return 0;
}
early_param("sprd_noautopd", sc7731g_noautopd);

static void __iomem *__init sc7731g_map(const char *compat)
{
	struct device_node *np = of_find_compatible_node(NULL, NULL, compat);
	void __iomem *base;

	if (!np) {
		pr_err("SMP: no %s node\n", compat);
		return NULL;
	}
	base = of_iomap(np, 0);
	of_node_put(np);
	if (!base)
		pr_err("SMP: cannot map %s\n", compat);
	return base;
}

static void __init sc7731g_smp_prepare_cpus(unsigned int max_cpus)
{
	u32 pause;

	ap_ahb = sc7731g_map("sprd,sc7731g-ap-ahb");
	pmu_apb = sc7731g_map("sprd,sc7731g-pmu-apb");
	if (!ap_ahb)
		return;

	/*
	 * The first SMP boot (2026-10-24) hard-hung ~1 s into userspace: no
	 * console, no SysRq, no RCU stall report -- every core gone at once.
	 * The single-core boots before it sat at a shell for minutes. A
	 * suspect: MCU_PAUSE's sleep enables. With any of them set, all cores
	 * in WFI is the AP's cue to sleep (light: clocks off; deep/sys: the
	 * domains armed for auto-shutdown -- CA7_TOP, PUB i.e. the DDR
	 * controller -- power down), and nothing here knows how to wake from
	 * that. The vendor's PM init clears all four first thing ("disable all
	 * sleep mode", pm-scx35.c sc_pm_init) and sets them only around its
	 * own idle states; until there is cpuidle here, they stay off.
	 * (It wasn't that: every boot read MCU_PAUSE 0x10, enables already
	 * off. The hang was the auto gates below.)
	 */
	pause = readl(ap_ahb + AP_AHB_MCU_PAUSE);
	writel(pause & ~MCU_SLEEP_ENABLES, ap_ahb + AP_AHB_MCU_PAUSE);
	pr_info("SMP: MCU_PAUSE was %#x, sleep enables cleared\n", pause);

	pause = readl(ap_ahb + AP_AHB_AP_SYS_AUTO_SLEEP_CFG);
	writel(pause & ~AUTO_GATES, ap_ahb + AP_AHB_AP_SYS_AUTO_SLEEP_CFG);
	pr_info("SMP: AP_SYS_AUTO_SLEEP_CFG was %#x, auto gates off\n", pause);

	if (noautopd && pmu_apb) {
		u32 top = readl(pmu_apb + PMU_APB_PD_CA7_TOP_CFG);
		u32 c0 = readl(pmu_apb + PMU_APB_PD_CA7_C0_CFG);

		writel(top & ~PD_AUTO_SHUTDOWN_EN, pmu_apb + PMU_APB_PD_CA7_TOP_CFG);
		writel(c0 & ~PD_AUTO_SHUTDOWN_EN, pmu_apb + PMU_APB_PD_CA7_C0_CFG);
		writel(readl(ap_ahb + AP_AHB_MCU_PAUSE) & ~MCU_SLEEP_FOLLOW_CA7,
		       ap_ahb + AP_AHB_MCU_PAUSE);
		pr_info("SMP: auto-shutdown disarmed (CA7_TOP was %#x, C0 %#x)\n",
			top, c0);
	}
}

static void sc7731g_power_on(unsigned int cpu)
{
	void __iomem *pd = pmu_apb + PMU_APB_PD_CA7_CFG(cpu);
	u32 autopd = noautopd ? 0 : PD_AUTO_SHUTDOWN_EN;
	int tries = 20;

	writel(readl(ap_ahb + AP_AHB_CA7_RST_SET) | BIT(cpu),
	       ap_ahb + AP_AHB_CA7_RST_SET);
	writel(PD_DELAYS, pd);
	writel((readl(pd) | autopd) & ~PD_FORCE_SHUTDOWN, pd);
	dmb();
	udelay(1000);

	/* the vendor loops forever here re-writing both; we give up after
	 * ~10 ms and let boot_secondary report the core as missing */
	while (((readl(pd) & PD_FORCE_SHUTDOWN) ||
		!(readl(ap_ahb + AP_AHB_CA7_RST_SET) & BIT(cpu))) && tries--) {
		writel(readl(ap_ahb + AP_AHB_CA7_RST_SET) | BIT(cpu),
		       ap_ahb + AP_AHB_CA7_RST_SET);
		writel((readl(pd) | autopd) & ~PD_FORCE_SHUTDOWN, pd);
		dmb();
		udelay(500);
	}

	writel(readl(ap_ahb + AP_AHB_CA7_RST_SET) & ~BIT(cpu),
	       ap_ahb + AP_AHB_CA7_RST_SET);
}

static int sc7731g_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	unsigned int hw = cpu_logical_map(cpu) & 0xf;

	if (!ap_ahb || !pmu_apb)
		return -ENODEV;
	if (hw < 1 || hw > 3)
		return -EINVAL;

	writel(__pa_symbol(secondary_startup_arm), ap_ahb + AP_AHB_CPU_JUMP(hw));
	writel(readl(ap_ahb + AP_AHB_HOLDING_PEN) | BIT(hw),
	       ap_ahb + AP_AHB_HOLDING_PEN);
	sc7731g_power_on(hw);

	/* a core already powered (a second boot of it) waits in WFE */
	dsb_sev();
	return 0;
}

static const struct smp_operations sc7731g_smp_ops __initconst = {
	.smp_prepare_cpus	= sc7731g_smp_prepare_cpus,
	.smp_boot_secondary	= sc7731g_boot_secondary,
};
CPU_METHOD_OF_DECLARE(sc7731g_smp, "sprd,sc7731g-smp", &sc7731g_smp_ops);
