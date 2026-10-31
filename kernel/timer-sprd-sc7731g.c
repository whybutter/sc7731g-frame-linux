// SPDX-License-Identifier: GPL-2.0-only
/*
 * Spreadtrum SC7731G (SC8830 family, "scx30g") general-purpose timers.
 *
 * Mainline's timer-sprd.c is for the SC9860's 64-bit timer and does not fit
 * this one: here each timer is 32 bits, LOAD/VALUE/CTL/INT/CNT_RD at
 * 0x0/0x4/0x8/0xc/0x10, three timers per block at a 0x20 stride, and on this
 * chip they all count down at 32768 Hz. The layout and bits are the vendor's
 * (Samsung SM-G531H GPL source, drivers/platform/sprd/timer_sc8830.c, the same
 * 3.10.65 our frame runs), checked against a U-Boot dump of 0x40230000.
 *
 * Like the vendor kernel, every CPU gets timer 0 of its own block as its tick
 * (one IRQ per block: the vendor's "local_timer" was IRQ 60 on CPU0 and 61 on
 * CPU1, i.e. SPI 28 and 29), and timer 1 of the first block free-runs as the
 * clocksource and sched_clock. The A7's architected timer is left alone: the
 * vendor never enabled it (CONFIG_HAVE_ARM_ARCH_TIMER is off in its defconfig)
 * and nothing says the system counter behind it is running.
 *
 * The blocks' clock gates live in AON_APB (0x402e0000): EB0 bits 10-12 and
 * EB1 bits 9-10, set here through the syscon until there is a clock driver
 * that owns them.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/percpu.h>
#include <linux/regmap.h>
#include <linux/sched_clock.h>

#define TIMER_LOAD	0x00
#define TIMER_CTL	0x08
#define TIMER_INT	0x0c
#define TIMER_CNT_RD	0x10
#define TIMER_STRIDE	0x20

#define CTL_PERIOD	BIT(6)
#define CTL_ENABLE	BIT(7)
#define CTL_NEW		BIT(8)	/* the vendor keeps it set on the free-running source */

#define INT_EN		BIT(0)
#define INT_CLR		BIT(3)
#define INT_BUSY	BIT(4)	/* a write to LOAD/CTL is still crossing into the 32k domain */

#define AON_APB_EB0	0x0
#define AON_APB_EB1	0x4
#define EB0_TIMERS	(BIT(10) | BIT(11) | BIT(12))	/* AP_SYST, AON_TMR, AP_TMR0 */
#define EB1_TIMERS	(BIT(9) | BIT(10))		/* AP_TMR1, AP_TMR2 */

#define MAX_BLOCKS	4

struct sc7731g_tick {
	struct clock_event_device ced;
	void __iomem *base;	/* timer 0 of this CPU's block */
	int irq;
	char name[16];
};

static void __iomem *blocks[MAX_BLOCKS];
static int block_irqs[MAX_BLOCKS];
static int nblocks;
static unsigned long timer_rate;
static void __iomem *source;	/* timer 1 of block 0 */
static DEFINE_PER_CPU(struct sc7731g_tick, ticks);

static void tick_wait_idle(void __iomem *t)
{
	while (readl_relaxed(t + TIMER_INT) & INT_BUSY)
		cpu_relax();
}

static struct sc7731g_tick *to_tick(struct clock_event_device *ced)
{
	return container_of(ced, struct sc7731g_tick, ced);
}

static int tick_next_event(unsigned long cycles, struct clock_event_device *ced)
{
	void __iomem *t = to_tick(ced)->base;

	tick_wait_idle(t);
	writel_relaxed(0, t + TIMER_CTL);
	writel_relaxed(cycles, t + TIMER_LOAD);
	writel_relaxed(CTL_ENABLE, t + TIMER_CTL);
	return 0;
}

static int tick_shutdown(struct clock_event_device *ced)
{
	void __iomem *t = to_tick(ced)->base;

	writel_relaxed(INT_CLR, t + TIMER_INT);
	writel_relaxed(0, t + TIMER_CTL);
	return 0;
}

static int tick_oneshot(struct clock_event_device *ced)
{
	void __iomem *t = to_tick(ced)->base;

	writel_relaxed(0, t + TIMER_CTL);
	writel_relaxed(INT_EN | INT_CLR, t + TIMER_INT);
	return 0;
}

static int tick_periodic(struct clock_event_device *ced)
{
	void __iomem *t = to_tick(ced)->base;

	tick_wait_idle(t);
	writel_relaxed(CTL_PERIOD, t + TIMER_CTL);
	writel_relaxed(DIV_ROUND_CLOSEST(timer_rate, HZ), t + TIMER_LOAD);
	writel_relaxed(CTL_ENABLE | CTL_PERIOD, t + TIMER_CTL);
	writel_relaxed(INT_EN | INT_CLR, t + TIMER_INT);
	return 0;
}

static irqreturn_t tick_interrupt(int irq, void *dev_id)
{
	struct sc7731g_tick *tk = dev_id;

	writel_relaxed(INT_EN | INT_CLR, tk->base + TIMER_INT);
	if (tk->ced.event_handler)
		tk->ced.event_handler(&tk->ced);
	return IRQ_HANDLED;
}

static int tick_starting_cpu(unsigned int cpu)
{
	struct sc7731g_tick *tk = per_cpu_ptr(&ticks, cpu);

	if (cpu >= nblocks) {
		pr_warn("sc7731g-timer: no timer block for cpu%u\n", cpu);
		return 0;
	}
	tk->base = blocks[cpu];
	tk->irq = block_irqs[cpu];
	snprintf(tk->name, sizeof(tk->name), "sc7731g-tick%u", cpu);
	tk->ced.name = tk->name;
	tk->ced.features = CLOCK_EVT_FEAT_ONESHOT | CLOCK_EVT_FEAT_PERIODIC;
	tk->ced.rating = 300;
	tk->ced.irq = tk->irq;
	tk->ced.cpumask = cpumask_of(cpu);
	tk->ced.set_next_event = tick_next_event;
	tk->ced.set_state_shutdown = tick_shutdown;
	tk->ced.set_state_oneshot = tick_oneshot;
	tk->ced.set_state_oneshot_stopped = tick_shutdown;
	tk->ced.set_state_periodic = tick_periodic;
	tick_shutdown(&tk->ced);

	irq_force_affinity(tk->irq, cpumask_of(cpu));
	enable_irq(tk->irq);
	/* the vendor's limits: at least 4 ticks, at most 0xf0000000 */
	clockevents_config_and_register(&tk->ced, timer_rate, 4, 0xf0000000);
	return 0;
}

static int tick_dying_cpu(unsigned int cpu)
{
	struct sc7731g_tick *tk = per_cpu_ptr(&ticks, cpu);

	if (cpu >= nblocks)
		return 0;
	tick_shutdown(&tk->ced);
	disable_irq(tk->irq);
	return 0;
}

static u64 notrace source_read(void)
{
	return ~readl_relaxed(source + TIMER_CNT_RD);	/* counts down */
}

static u64 source_cs_read(struct clocksource *cs)
{
	return source_read();
}

static struct clocksource source_cs = {
	.name = "sc7731g-timer",
	.rating = 300,
	.read = source_cs_read,
	.mask = CLOCKSOURCE_MASK(32),
	.flags = CLOCK_SOURCE_IS_CONTINUOUS,
};

static void __init source_init(void)
{
	writel_relaxed(0, source + TIMER_INT);
	writel_relaxed(CTL_NEW | CTL_PERIOD, source + TIMER_CTL);
	writel_relaxed(U32_MAX, source + TIMER_LOAD);
	writel_relaxed(CTL_ENABLE | CTL_NEW | CTL_PERIOD, source + TIMER_CTL);
	sched_clock_register(source_read, 32, timer_rate);
	clocksource_register_hz(&source_cs, timer_rate);
}

static int __init sc7731g_timer_init(struct device_node *np)
{
	struct regmap *aon_apb;
	struct clk *clk;
	int i, b, ret;

	aon_apb = syscon_regmap_lookup_by_phandle(np, "sprd,aon-apb");
	if (IS_ERR(aon_apb)) {
		pr_err("sc7731g-timer: no sprd,aon-apb syscon\n");
		return PTR_ERR(aon_apb);
	}
	regmap_set_bits(aon_apb, AON_APB_EB0, EB0_TIMERS);
	regmap_set_bits(aon_apb, AON_APB_EB1, EB1_TIMERS);

	clk = of_clk_get(np, 0);
	if (IS_ERR(clk)) {
		pr_err("sc7731g-timer: no clock\n");
		return PTR_ERR(clk);
	}
	clk_prepare_enable(clk);
	timer_rate = clk_get_rate(clk);
	if (!timer_rate)
		return -EINVAL;

	for (b = 0; b < MAX_BLOCKS; b++) {
		blocks[b] = of_iomap(np, b);
		if (!blocks[b])
			break;
		block_irqs[b] = irq_of_parse_and_map(np, b);
		if (!block_irqs[b]) {
			iounmap(blocks[b]);
			blocks[b] = NULL;
			break;
		}
		/* everything off and acknowledged, as the vendor's early init does */
		for (i = 0; i < 3; i++) {
			writel_relaxed(0, blocks[b] + i * TIMER_STRIDE + TIMER_CTL);
			writel_relaxed(INT_CLR, blocks[b] + i * TIMER_STRIDE + TIMER_INT);
		}
	}
	nblocks = b;
	if (!nblocks) {
		pr_err("sc7731g-timer: no timer blocks\n");
		return -ENXIO;
	}

	source = blocks[0] + TIMER_STRIDE;
	source_init();

	for (b = 0; b < nblocks; b++) {
		struct sc7731g_tick *tk = per_cpu_ptr(&ticks, b);

		irq_set_status_flags(block_irqs[b], IRQ_NOAUTOEN);
		ret = request_irq(block_irqs[b], tick_interrupt,
				  IRQF_TIMER | IRQF_NOBALANCING, "sc7731g-tick", tk);
		if (ret) {
			pr_err("sc7731g-timer: irq %d: %d\n", block_irqs[b], ret);
			return ret;
		}
	}

	/* a STARTING state (added to cpuhotplug.h by our build): a secondary CPU
	 * must have its tick before it is marked online, not after */
	ret = cpuhp_setup_state(CPUHP_AP_SC7731G_TIMER_STARTING, "clockevents/sc7731g:starting",
				tick_starting_cpu, tick_dying_cpu);
	if (ret < 0)
		return ret;

	pr_info("sc7731g-timer: %d blocks at %lu Hz\n", nblocks, timer_rate);
	return 0;
}

TIMER_OF_DECLARE(sc7731g_timer, "sprd,sc7731g-timer", sc7731g_timer_init);
