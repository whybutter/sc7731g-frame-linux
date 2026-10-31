// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the SC7731G's WCN co-processor: an on-die ARM core (cp2) that
 * runs Spreadtrum's connectivity firmware (Wi-Fi, BT, FM) and talks to the
 * AP over shared DRAM through SIPC. The radios on top (sprdwl-sc7731g for
 * Wi-Fi; BT separately) are DT children.
 *
 * Written against the frame's own firmware (CP2_WCN_14D_W16.26.3), from the
 * user-space work in initramfs/cp2.c and docs/wifi.md wifi1..wifi5. The
 * vendor's 3.10 drivers (Samsung SM-G531H GPL kernel: drivers/misc/
 * sprd_cproc for the loader, drivers/sipc for the transport) were the
 * reference where the frame's binary didn't say otherwise.
 */

#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/mfd/syscon.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>
#include <linux/wait.h>

#include "wcn-sc7731g.h"

/* PMU_APB: the vendor DT's scproc "cpwcn" ctrl-reg/ctrl-mask said so */
#define PD_CP2_SYS_CFG		0x68
#define  CP2_SHUTDOWN		BIT(25)
#define  CP2_DEEPSLEEP		BIT(28)
#define CP_SOFT_RST		0xb0
#define  CP2_RST		BIT(2)
#define SLEEP_STATUS		0xd4
#define  CP2_SLEEP_BUSY		GENMASK(15, 12)

/* AON_APB EB0 bits 21 and 23: the IPI block and the RF2351's SPI
 * (clk_mspi, which the clock core would otherwise turn off at boot as
 * unclaimed) */
#define AON_EB0			0x0
#define  AON_IPI_EB		BIT(21)
#define  AON_MSPI_EB		BIT(23)

/* cp2 is linked to run at 0x8a808000; the image sits at cp2 + 0x8000 */
#define IMAGE_OFF		0x8000

/* smsg (include/linux/sipc.h in the vendor tree, re-proved in cp2.c): the
 * last page of cp2's region is a shared ring, with the AP's tx first, its
 * rx next, and four 32-bit pointers after. 8-byte messages, 128 slots.
 * Each slot: u8 channel, u8 type, u16 flag, u32 value */
#define RING_OFF		0x200000
#define SMSG_TX			0x000
#define SMSG_RX			0x400
#define SMSG_TX_RD		0x800
#define SMSG_TX_WR		0x804
#define SMSG_RX_RD		0x808
#define SMSG_RX_WR		0x80c
#define SMSGS			128

#define IPI_AP2CP		0x0	/* writing IPI_WCN here rings cp2 */
#define IPI_CP2AP		0x4	/* writing IPI_WCN here clears our pending */
#define IPI_WCN			0x100

/* message types (vendor smsg_type) and the OPEN flag */
#define T_OPEN			1
#define T_CLOSE			2
#define T_DATA			3
#define T_EVENT			4
#define T_CMD			5
#define T_DONE			6
#define OPEN_MAGIC		0xbeee

/* channel numbers (vendor smsg_ch); WCN is dst=3 (sipc_wcn) and these are
 * the smsg channels inside it */
#define CH_PIPE			4	/* sbuf (13 rings on this frame) */
#define CH_LOG			5	/* sbuf (1 ring: cp2's armlog) */
#define CH_DATA			7	/* sblock, Wi-Fi data frames */
#define CH_EVENT		8	/* sblock, Wi-Fi events */

/* sbuf: cp2 asks for the layout with CMD_SBUF_INIT, the AP answers DONE
 * with a smem physical address. EVENTs on an sbuf channel carry the ring
 * number in the value field */
#define CMD_SBUF_INIT		1
#define DONE_SBUF_INIT		2
#define EVT_WRPTR		1
#define EVT_RDPTR		2

/* channel 4's smem region, from cp2.c and docs/wifi.md: 13 rings of 4 KiB
 * each way, starting at smem + 0. Header: u32 ringnr, then per ring
 * 8 u32 = txaddr/txsize/txrd/txwr and rxaddr/rxsize/rxrd/rxwr */
#define SBUF4_OFF		0x00000
#define SBUF4_RINGS		13
#define SBUF4_RINGSIZE		0x1000
#define SBUF_HSIZE		(4 + SBUF4_RINGS * 32)
/* channel 5 (slog): one ring, 32 KiB tx, 256 KiB rx. We never actually
 * read from it; we lay it out and answer SBUF_INIT so cp2 moves on */
#define SBUF5_OFF		0x71000
#define SBUF5_TXSZ		0x8000
#define SBUF5_RXSZ		0x40000
/* per-ring descriptor words, as cp2 reads them */
#define SR_TXADDR		0
#define SR_TXSIZE		1
#define SR_TXRD			2
#define SR_TXWR			3
#define SR_RXADDR		4
#define SR_RXSIZE		5
#define SR_RXRD			6
#define SR_RXWR			7

/* sblock, docs/wifi.md wifi4: an sblock_header is two sblock_ring_header
 * structs (ring then pool), each 12 u32 = 48 bytes; pool starts at word 12.
 * Blocks move between the ring and the pool by pointer updates plus
 * EVT_SBLK_SEND or EVT_SBLK_RELEASE */
#define CMD_SBLOCK_INIT		1
#define DONE_SBLOCK_INIT	2
#define EVT_SBLK_SEND		1
#define EVT_SBLK_RELEASE	2

#define SB_HSIZE		96		/* two ring_header structs */
#define SB_POOL			12		/* word offset of the pool header */
#define SB_TXADDR		0
#define SB_TXCNT		1
#define SB_TXSIZE		2
#define SB_TXBLKS		3
#define SB_TXRD			4
#define SB_TXWR			5
#define SB_RXADDR		6
#define SB_RXCNT		7
#define SB_RXSIZE		8
#define SB_RXBLKS		9
#define SB_RXRD			10
#define SB_RXWR			11

/* geometry (vendor sprdwl::sipc.c), smem offsets from cp2.c */
#define SBLK7_OFF		0x1b000
#define SBLK7_TXN		64
#define SBLK7_RXN		128
#define SBLK7_SIZE		1664
#define SBLK8_OFF		0x6b000
#define SBLK8_N			1
#define SBLK8_SIZE		10240

/* The four regions, by index in the DT's reg property.
 * cp2:   its 2052 KiB block of DRAM, firmware image at +0x8000, the smsg
 *        ring in the last page at +0x200000;
 * smem:  sipc_wcn's shared window (768 KiB inside the cp-shm reserve);
 * ipi:   AON IPI block, two words: writing 0x100 to +0 rings cp2, 0x100
 *        at +4 clears our side (SPI 73);
 * iram:  the trampoline is written to its first 12 bytes on start. */
enum {
	REG_CP2, REG_SMEM, REG_IPI, REG_IRAM, REG_NR
};
static const char *const reg_name[REG_NR] = { "cp2", "smem", "ipi", "iram" };

struct wcn {
	struct device *dev;
	struct mutex lock;
	int users;		/* refcount, guarded by lock */
	bool running;
	u32 boots;		/* cp2 starts so far: a radio that hands cp2 state
				 * of its own (BT's PSKEY) knows when to again */

	void __iomem *reg[REG_NR];
	u32 reg_phys[REG_NR];	/* cp2 is told physical addresses (no IOMMU) */
	struct regmap *pmu, *aon;

	const char *fw_name;
	int irq_sipc, irq_wdt;
	bool irq_masked;
	u32 aon_eb0_saved;	/* AON EB0's start-time value, restored on stop */

	spinlock_t tx_lock;	/* smsg tx ring */
	wait_queue_head_t tx_wq;

	struct task_struct *slog_thread;
	bool slog_ready;

	/* sbuf state per channel-4 ring, plus the handshake flag */
	bool sbuf4_ready;
	bool sbuf4_laid_out;
	struct wcn_sbuf_ring {
		wait_queue_head_t wq;		/* rx data or tx space */
		struct wcn_notifier {
			void (*cb)(void *data, enum wcn_notify what);
			void *data;
		} nf;
	} ring[SBUF4_RINGS];
	wait_queue_head_t sbuf_wq;		/* for the init handshake */

	/* sblock state per channel: the pool offset we gave cp2, geometry,
	 * waitqueues and notify, serialising spinlocks */
	struct wcn_sblock {
		u32 off;
		u32 txn, txsz, rxn, rxsz;
		bool ready;
		spinlock_t tx_lock, rx_lock;
		wait_queue_head_t get_wq, recv_wq;
		struct wcn_notifier nf;
	} sblk[2];
	wait_queue_head_t sblock_wq;		/* for the init handshake */
};

static struct wcn_sblock *sblock_for(struct wcn *wcn, u8 ch)
{
	return ch == CH_DATA ? &wcn->sblk[0] :
	       ch == CH_EVENT ? &wcn->sblk[1] : NULL;
}

/* --- smsg: 8-byte messages over the ring at cp2 + 0x200000 --- */

static void doorbell_cp2(struct wcn *wcn)
{
	writel(IPI_WCN, wcn->reg[REG_IPI] + IPI_AP2CP);
}

static void doorbell_ack(struct wcn *wcn)
{
	writel(IPI_WCN, wcn->reg[REG_IPI] + IPI_CP2AP);
}

/* Every SIPC message and cp2's whole firmware trace on the log, for the
 * bring-up work that needs them: `echo 1 >
 * /sys/module/wcn_sc7731g/parameters/trace`. This used to be always on,
 * and with a data path a message per frame means the printk (and fbcon,
 * console=tty1) running in the data path. The slog thread drains the ring
 * either way: cp2 stalls on a full one */
static bool trace;
module_param(trace, bool, 0644);
MODULE_PARM_DESC(trace, "log SIPC messages and cp2's firmware trace");

/* Send a message to cp2. Blocks for up to timeout_ms if the ring is full
 * (cp2 isn't reading). timeout_ms 0 means don't wait; -1 means forever */
static int smsg_send(struct wcn *wcn, u8 ch, u8 type, u16 flag, u32 value,
		     int timeout_ms)
{
	void __iomem *r = wcn->reg[REG_CP2] + RING_OFF;
	unsigned long flags;
	u32 wr, slot;
	int ret;

	spin_lock_irqsave(&wcn->tx_lock, flags);
	while (readl(r + SMSG_TX_WR) - readl(r + SMSG_TX_RD) >= SMSGS) {
		spin_unlock_irqrestore(&wcn->tx_lock, flags);
		if (!timeout_ms)
			return -EAGAIN;
		ret = wait_event_interruptible_timeout(wcn->tx_wq,
			readl(r + SMSG_TX_WR) - readl(r + SMSG_TX_RD) < SMSGS,
			timeout_ms > 0 ? msecs_to_jiffies(timeout_ms)
				       : MAX_SCHEDULE_TIMEOUT);
		if (ret < 0)
			return ret;
		if (!ret)
			return -ETIMEDOUT;
		spin_lock_irqsave(&wcn->tx_lock, flags);
	}
	wr = readl(r + SMSG_TX_WR);
	slot = SMSG_TX + (wr & (SMSGS - 1)) * 8;
	writel(ch | (u32)type << 8 | (u32)flag << 16, r + slot);
	writel(value, r + slot + 4);
	wmb();
	writel(wr + 1, r + SMSG_TX_WR);
	doorbell_cp2(wcn);
	spin_unlock_irqrestore(&wcn->tx_lock, flags);
	if (trace)
		dev_info(wcn->dev, "tx ch=%u type=%u flag=0x%04x value=0x%08x\n",
			 ch, type, flag, value);
	return 0;
}

/* Drain cp2's own trace into dev_info. cp2 writes to the slog's rx side
 * (from cp2's view, its tx of that ring). Read bytes, print them. The ring
 * is 256 KiB -- cp2 will stop writing if we don't drain, which can hang
 * everything else */
static int slog_thread_fn(void *data)
{
	struct wcn *wcn = data;
	void __iomem *h = wcn->reg[REG_SMEM] + SBUF5_OFF + 4;
	void __iomem *rx;
	char line[256];
	u32 rx_addr, rx_size, rd, wr, pos = 0;

	while (!wcn->slog_ready && !kthread_should_stop())
		msleep(50);
	rx_addr = readl(h + SR_RXADDR * 4);
	rx_size = readl(h + SR_RXSIZE * 4);
	rx = wcn->reg[REG_SMEM] + (rx_addr - wcn->reg_phys[REG_SMEM]);
	dev_info(wcn->dev, "slog: addr=0x%x size=0x%x\n", rx_addr, rx_size);
	while (!kthread_should_stop()) {
		rd = readl(h + SR_RXRD * 4);
		wr = readl(h + SR_RXWR * 4);
		if (rd == wr) {
			msleep_interruptible(100);
			continue;
		}
		if (trace)
			dev_info(wcn->dev, "slog: rd=0x%x wr=0x%x\n", rd, wr);
		while (rd != wr) {
			u8 c = readb(rx + (rd & (rx_size - 1)));

			if (c == '\n' || pos >= sizeof(line) - 1) {
				line[pos] = 0;
				if (pos > 0 && trace)
					dev_info(wcn->dev, "cp2: %s\n", line);
				pos = 0;
			} else if (c == '\r' || c == 0) {
				/* skip */
			} else {
				line[pos++] = (c >= 0x20 && c < 0x7f) ? c : '.';
			}
			rd++;
		}
		wmb();
		writel(rd, h + SR_RXRD * 4);
	}
	return 0;
}

/* Hardirq: ack cp2's doorbell and hand off to the thread, which can
 * sleep on channel dispatch (regmap, memory copies, waking consumers) */
static irqreturn_t sipc_isr(int irq, void *data)
{
	struct wcn *wcn = data;

	doorbell_ack(wcn);
	return IRQ_WAKE_THREAD;
}

/* Drain every message cp2 queued and route by channel. The sbuf and
 * sblock routers are added in the next commits; for now, log and
 * acknowledge the ring position so cp2 keeps running. tx_wq wakes any
 * sender blocked on a full ring */
/* --- sbuf on channel 4 (13 byte-stream rings) --- */

/* The header cp2 reads to learn where each ring lives in smem. Written on
 * the first CMD_SBUF_INIT from cp2 (triggered by our T_OPEN); after that,
 * all byte traffic is pointer updates on this layout plus EVENT WRPTR /
 * RDPTR messages. Reading rxrd is cp2's job: it only tells us WRPTR events. */
/* The slog's own 1-ring header, matching user-space cp2.c's slog_layout.
 * cp2 reads this header the moment it processes our T_OPEN, so we must
 * lay it out BEFORE sending OPEN -- not when answering CMD_SBUF_INIT */
static void slog_layout(struct wcn *wcn)
{
	void __iomem *s = wcn->reg[REG_SMEM] + SBUF5_OFF;
	u32 phys = wcn->reg_phys[REG_SMEM] + SBUF5_OFF;
	int i;

	for (i = 0; i < 0x40; i += 4)
		writel(0, s + i);
	writel(1, s);				/* ringnr */
	writel(phys + 0x40, s + 4 + SR_TXADDR * 4);
	writel(SBUF5_TXSZ, s + 4 + SR_TXSIZE * 4);
	writel(phys + 0x40 + SBUF5_TXSZ, s + 4 + SR_RXADDR * 4);
	writel(SBUF5_RXSZ, s + 4 + SR_RXSIZE * 4);
	wmb();
}

static void sbuf4_layout(struct wcn *wcn)
{
	void __iomem *s = wcn->reg[REG_SMEM] + SBUF4_OFF;
	u32 phys = wcn->reg_phys[REG_SMEM] + SBUF4_OFF;
	u32 off = SBUF_HSIZE;
	int r;

	writel(SBUF4_RINGS, s);
	for (r = 0; r < SBUF4_RINGS; r++) {
		void __iomem *h = s + 4 + r * 32;
		u32 tx = phys + off;
		u32 rx = tx + SBUF4_RINGSIZE;

		writel(tx, h + SR_TXADDR * 4);
		writel(SBUF4_RINGSIZE, h + SR_TXSIZE * 4);
		writel(0, h + SR_TXRD * 4);
		writel(0, h + SR_TXWR * 4);
		writel(rx, h + SR_RXADDR * 4);
		writel(SBUF4_RINGSIZE, h + SR_RXSIZE * 4);
		writel(0, h + SR_RXRD * 4);
		writel(0, h + SR_RXWR * 4);
		off += 2 * SBUF4_RINGSIZE;
	}
	wmb();
}

static void __iomem *ring_hdr(struct wcn *wcn, u8 r)
{
	return wcn->reg[REG_SMEM] + SBUF4_OFF + 4 + r * 32;
}

/* The ring's buffers live right after the header; translate the physical
 * txaddr/rxaddr cp2 reads back to our smem mapping */
static void __iomem *ring_buf(struct wcn *wcn, u32 phys)
{
	return wcn->reg[REG_SMEM] + (phys - wcn->reg_phys[REG_SMEM]);
}

u32 wcn_sbuf_avail(struct wcn *wcn, int r)
{
	void __iomem *h;

	if (r < 0 || r >= SBUF4_RINGS || !wcn->sbuf4_ready)
		return 0;
	h = ring_hdr(wcn, r);
	return readl(h + SR_RXWR * 4) - readl(h + SR_RXRD * 4);
}
EXPORT_SYMBOL_GPL(wcn_sbuf_avail);

/* Write `len` bytes to ring r's tx buffer, wrapping. All or nothing: if
 * cp2 is slow and the ring has less than `len` free, wait up to
 * timeout_ms (0: don't wait; -1: forever) for enough space */
int wcn_sbuf_write(struct wcn *wcn, int r, const void *buf, u32 len,
		   int timeout_ms)
{
	void __iomem *h, *tx;
	u32 wr, rd, i;
	int ret;

	if (r < 0 || r >= SBUF4_RINGS)
		return -EINVAL;
	if (!wcn->sbuf4_ready)
		return -ENODEV;
	if (len > SBUF4_RINGSIZE)
		return -EMSGSIZE;
	h = ring_hdr(wcn, r);
	tx = ring_buf(wcn, readl(h + SR_TXADDR * 4));

	for (;;) {
		wr = readl(h + SR_TXWR * 4);
		rd = readl(h + SR_TXRD * 4);
		if (SBUF4_RINGSIZE - (wr - rd) >= len)
			break;
		if (!timeout_ms)
			return -EAGAIN;
		ret = wait_event_interruptible_timeout(wcn->ring[r].wq,
			SBUF4_RINGSIZE - (readl(h + SR_TXWR * 4) -
					  readl(h + SR_TXRD * 4)) >= len,
			timeout_ms > 0 ? msecs_to_jiffies(timeout_ms)
				       : MAX_SCHEDULE_TIMEOUT);
		if (ret < 0)
			return ret;
		if (!ret)
			return -ETIMEDOUT;
	}
	for (i = 0; i < len; i++)
		writeb(((const u8 *)buf)[i],
		       tx + ((wr + i) & (SBUF4_RINGSIZE - 1)));
	wmb();
	writel(wr + len, h + SR_TXWR * 4);
	return smsg_send(wcn, CH_PIPE, T_EVENT, EVT_WRPTR, r, 100);
}
EXPORT_SYMBOL_GPL(wcn_sbuf_write);

u32 wcn_sbuf_tx_consumed(struct wcn *wcn, int r)
{
	void __iomem *h;

	if (r < 0 || r >= SBUF4_RINGS || !wcn->sbuf4_ready)
		return 0;
	h = ring_hdr(wcn, r);
	return readl(h + SR_TXWR * 4) - readl(h + SR_TXRD * 4);
}
EXPORT_SYMBOL_GPL(wcn_sbuf_tx_consumed);

/* Read up to `len` bytes from ring r's rx buffer, wrapping. Returns the
 * number read, 0 if the ring was empty and timeout_ms is 0 */
int wcn_sbuf_read(struct wcn *wcn, int r, void *buf, u32 len, int timeout_ms)
{
	void __iomem *h, *rx;
	u32 wr, rd, got, i;
	int ret;

	if (r < 0 || r >= SBUF4_RINGS)
		return -EINVAL;
	if (!wcn->sbuf4_ready)
		return -ENODEV;
	h = ring_hdr(wcn, r);
	rx = ring_buf(wcn, readl(h + SR_RXADDR * 4));

	for (;;) {
		wr = readl(h + SR_RXWR * 4);
		rd = readl(h + SR_RXRD * 4);
		if (wr != rd)
			break;
		if (!timeout_ms)
			return 0;
		ret = wait_event_interruptible_timeout(wcn->ring[r].wq,
			readl(h + SR_RXWR * 4) != readl(h + SR_RXRD * 4),
			timeout_ms > 0 ? msecs_to_jiffies(timeout_ms)
				       : MAX_SCHEDULE_TIMEOUT);
		if (ret < 0)
			return ret;
		if (!ret)
			return -ETIMEDOUT;
	}
	got = min_t(u32, len, wr - rd);
	for (i = 0; i < got; i++)
		((u8 *)buf)[i] = readb(rx + ((rd + i) & (SBUF4_RINGSIZE - 1)));
	wmb();
	writel(rd + got, h + SR_RXRD * 4);
	smsg_send(wcn, CH_PIPE, T_EVENT, EVT_RDPTR, r, 100);
	return got;
}
EXPORT_SYMBOL_GPL(wcn_sbuf_read);

/* --- sblock on channels 7 and 8 (fixed-size block pools) --- */

/* After the header (two ring_headers), the layout is: tx blocks, rx
 * blocks, ring tx+rx blks arrays, pool tx+rx blks arrays. Initial state
 * (vendor sblock_create): ring pointers at 0, pool rxblk_wrptr and
 * txblk_wrptr at count (every block is in the pool, free). The pool blks
 * arrays are pre-filled with every block's (addr, size); the ring blks
 * arrays stay zero. The pool header is at word 12. */
static void sblock_layout(struct wcn *wcn, u8 ch)
{
	struct wcn_sblock *s = sblock_for(wcn, ch);
	void __iomem *base = wcn->reg[REG_SMEM] + s->off;
	u32 phys = wcn->reg_phys[REG_SMEM] + s->off;
	u32 tx = phys + SB_HSIZE;
	u32 rx = tx + s->txn * s->txsz;
	u32 r_tx_blks = rx + s->rxn * s->rxsz;
	u32 r_rx_blks = r_tx_blks + s->txn * 8;
	u32 p_tx_blks = r_rx_blks + s->rxn * 8;
	u32 p_rx_blks = p_tx_blks + s->txn * 8;
	u32 total = (p_rx_blks + s->rxn * 8) - phys;
	u32 i;

	for (i = 0; i < total; i += 4)
		writel(0, base + i);
	writel(tx, base + SB_TXADDR * 4);
	writel(s->txn, base + SB_TXCNT * 4);
	writel(s->txsz, base + SB_TXSIZE * 4);
	writel(r_tx_blks, base + SB_TXBLKS * 4);
	writel(rx, base + SB_RXADDR * 4);
	writel(s->rxn, base + SB_RXCNT * 4);
	writel(s->rxsz, base + SB_RXSIZE * 4);
	writel(r_rx_blks, base + SB_RXBLKS * 4);
	writel(tx, base + (SB_POOL + SB_TXADDR) * 4);
	writel(s->txn, base + (SB_POOL + SB_TXCNT) * 4);
	writel(s->txsz, base + (SB_POOL + SB_TXSIZE) * 4);
	writel(p_tx_blks, base + (SB_POOL + SB_TXBLKS) * 4);
	writel(s->txn, base + (SB_POOL + SB_TXWR) * 4);
	writel(rx, base + (SB_POOL + SB_RXADDR) * 4);
	writel(s->rxn, base + (SB_POOL + SB_RXCNT) * 4);
	writel(s->rxsz, base + (SB_POOL + SB_RXSIZE) * 4);
	writel(p_rx_blks, base + (SB_POOL + SB_RXBLKS) * 4);
	writel(s->rxn, base + (SB_POOL + SB_RXWR) * 4);
	/* pool blks[] lives at p_tx_blks / p_rx_blks (physical). Translate
	 * to the SMEM mapping by subtracting the SMEM base, NOT the sblock
	 * region base -- that put the array 0x1b000 (SBLKn_OFF) short, so
	 * cp2 read zeros when popping the pool and silently stalled DEV_OPEN
	 * (seen in docs/wifi.md wifi7's pool dump: TXRD=1 but no SBLK_SEND) */
	for (i = 0; i < s->txn; i++) {
		writel(tx + i * s->txsz,
		       wcn->reg[REG_SMEM]
		       + (p_tx_blks - wcn->reg_phys[REG_SMEM]) + i * 8);
		writel(s->txsz,
		       wcn->reg[REG_SMEM]
		       + (p_tx_blks - wcn->reg_phys[REG_SMEM]) + i * 8 + 4);
	}
	for (i = 0; i < s->rxn; i++) {
		writel(rx + i * s->rxsz,
		       wcn->reg[REG_SMEM]
		       + (p_rx_blks - wcn->reg_phys[REG_SMEM]) + i * 8);
		writel(s->rxsz,
		       wcn->reg[REG_SMEM]
		       + (p_rx_blks - wcn->reg_phys[REG_SMEM]) + i * 8 + 4);
	}
	wmb();
}

/* Walk the pool.tx entries for the next free block, hand it back. The
 * mask works because the vendor's counts are powers of two (64, 128, 1) */
int wcn_sblock_get(struct wcn *wcn, int ch, struct wcn_block *blk)
{
	struct wcn_sblock *s = sblock_for(wcn, ch);
	void __iomem *base, *entry;
	u32 rd, wr, cnt, size, addr, pos, blks;
	unsigned long flags;

	if (!s || !s->ready)
		return -ENODEV;
	base = wcn->reg[REG_SMEM] + s->off;
	spin_lock_irqsave(&s->tx_lock, flags);
	rd = readl(base + (SB_POOL + SB_TXRD) * 4);
	wr = readl(base + (SB_POOL + SB_TXWR) * 4);
	if (rd == wr) {
		spin_unlock_irqrestore(&s->tx_lock, flags);
		return -EAGAIN;
	}
	cnt = readl(base + (SB_POOL + SB_TXCNT) * 4);
	size = readl(base + (SB_POOL + SB_TXSIZE) * 4);
	blks = readl(base + (SB_POOL + SB_TXBLKS) * 4);
	pos = rd & (cnt - 1);
	entry = wcn->reg[REG_SMEM] + (blks - wcn->reg_phys[REG_SMEM]) + pos * 8;
	addr = readl(entry);
	writel(rd + 1, base + (SB_POOL + SB_TXRD) * 4);
	spin_unlock_irqrestore(&s->tx_lock, flags);
	blk->addr = wcn->reg[REG_SMEM] + (addr - wcn->reg_phys[REG_SMEM]);
	blk->length = size;
	return 0;
}
EXPORT_SYMBOL_GPL(wcn_sblock_get);

/* Hand a got-but-unused block back to the pool (abort). Reverse of _get. */
void wcn_sblock_put(struct wcn *wcn, int ch, struct wcn_block *blk)
{
	struct wcn_sblock *s = sblock_for(wcn, ch);
	void __iomem *base;
	unsigned long flags;
	u32 rd;

	if (!s || !s->ready)
		return;
	base = wcn->reg[REG_SMEM] + s->off;
	spin_lock_irqsave(&s->tx_lock, flags);
	rd = readl(base + (SB_POOL + SB_TXRD) * 4);
	writel(rd - 1, base + (SB_POOL + SB_TXRD) * 4);
	spin_unlock_irqrestore(&s->tx_lock, flags);
}
EXPORT_SYMBOL_GPL(wcn_sblock_put);

/* Send a filled block to cp2: write its (addr, len) into ring.tx and
 * advance ring.txblk_wrptr, then an EVT_SBLK_SEND */
int wcn_sblock_send(struct wcn *wcn, int ch, struct wcn_block *blk)
{
	struct wcn_sblock *s = sblock_for(wcn, ch);
	void __iomem *base, *entry;
	u32 wr, cnt, pos, blks, phys;
	unsigned long flags;

	if (!s || !s->ready)
		return -ENODEV;
	base = wcn->reg[REG_SMEM] + s->off;
	phys = (u8 *)blk->addr - (u8 *)wcn->reg[REG_SMEM]
	       + wcn->reg_phys[REG_SMEM];
	spin_lock_irqsave(&s->tx_lock, flags);
	wr = readl(base + SB_TXWR * 4);
	cnt = readl(base + SB_TXCNT * 4);
	blks = readl(base + SB_TXBLKS * 4);
	pos = wr & (cnt - 1);
	entry = wcn->reg[REG_SMEM] + (blks - wcn->reg_phys[REG_SMEM]) + pos * 8;
	writel(phys, entry);
	writel(blk->length, entry + 4);
	writel(wr + 1, base + SB_TXWR * 4);
	spin_unlock_irqrestore(&s->tx_lock, flags);
	return smsg_send(wcn, ch, T_EVENT, EVT_SBLK_SEND, 0, 100);
}
EXPORT_SYMBOL_GPL(wcn_sblock_send);

/* Receive a filled block from cp2: ring.rxblk_wrptr > rxblk_rdptr */
int wcn_sblock_receive(struct wcn *wcn, int ch, struct wcn_block *blk)
{
	struct wcn_sblock *s = sblock_for(wcn, ch);
	void __iomem *base, *entry;
	u32 rd, wr, cnt, pos, blks, addr;
	unsigned long flags;

	if (!s || !s->ready)
		return -ENODEV;
	base = wcn->reg[REG_SMEM] + s->off;
	spin_lock_irqsave(&s->rx_lock, flags);
	rd = readl(base + SB_RXRD * 4);
	wr = readl(base + SB_RXWR * 4);
	if (rd == wr) {
		spin_unlock_irqrestore(&s->rx_lock, flags);
		return -EAGAIN;
	}
	cnt = readl(base + SB_RXCNT * 4);
	blks = readl(base + SB_RXBLKS * 4);
	pos = rd & (cnt - 1);
	entry = wcn->reg[REG_SMEM] + (blks - wcn->reg_phys[REG_SMEM]) + pos * 8;
	addr = readl(entry);
	blk->length = readl(entry + 4);
	writel(rd + 1, base + SB_RXRD * 4);
	spin_unlock_irqrestore(&s->rx_lock, flags);
	blk->addr = wcn->reg[REG_SMEM] + (addr - wcn->reg_phys[REG_SMEM]);
	return 0;
}
EXPORT_SYMBOL_GPL(wcn_sblock_receive);

/* Give a received block back to cp2 for reuse: pool.rx. If the pool was
 * empty cp2 may be stalled, so we announce the release */
void wcn_sblock_release(struct wcn *wcn, int ch, struct wcn_block *blk)
{
	struct wcn_sblock *s = sblock_for(wcn, ch);
	void __iomem *base, *entry;
	u32 wr, rd, cnt, pos, blks, phys;
	unsigned long flags;
	bool was_empty;

	if (!s || !s->ready)
		return;
	base = wcn->reg[REG_SMEM] + s->off;
	phys = (u8 *)blk->addr - (u8 *)wcn->reg[REG_SMEM]
	       + wcn->reg_phys[REG_SMEM];
	spin_lock_irqsave(&s->rx_lock, flags);
	wr = readl(base + (SB_POOL + SB_RXWR) * 4);
	rd = readl(base + (SB_POOL + SB_RXRD) * 4);
	cnt = readl(base + (SB_POOL + SB_RXCNT) * 4);
	blks = readl(base + (SB_POOL + SB_RXBLKS) * 4);
	was_empty = (wr == rd);
	pos = wr & (cnt - 1);
	entry = wcn->reg[REG_SMEM] + (blks - wcn->reg_phys[REG_SMEM]) + pos * 8;
	writel(phys, entry);
	writel(readl(base + (SB_POOL + SB_RXSIZE) * 4), entry + 4);
	writel(wr + 1, base + (SB_POOL + SB_RXWR) * 4);
	spin_unlock_irqrestore(&s->rx_lock, flags);
	if (was_empty)
		smsg_send(wcn, ch, T_EVENT, EVT_SBLK_RELEASE, 0, 100);
}
EXPORT_SYMBOL_GPL(wcn_sblock_release);

int wcn_register_notify(struct wcn *wcn, int key,
			void (*cb)(void *data, enum wcn_notify what),
			void *data)
{
	struct wcn_sblock *s;

	if (key >= 0 && key < SBUF4_RINGS) {
		wcn->ring[key].nf.cb = cb;
		wcn->ring[key].nf.data = data;
		return 0;
	}
	s = sblock_for(wcn, key - 0x100);
	if (s) {
		s->nf.cb = cb;
		s->nf.data = data;
		return 0;
	}
	return -EINVAL;
}
EXPORT_SYMBOL_GPL(wcn_register_notify);

static void dispatch(struct wcn *wcn, u8 ch, u8 type, u16 flag, u32 value)
{
	if (trace)
		dev_info(wcn->dev, "rx ch=%u type=%u flag=0x%04x value=0x%08x\n",
		 ch, type, flag, value);
	if (ch == CH_PIPE && type == T_CMD && flag == CMD_SBUF_INIT) {
		/* layout happens in wait_sbuf4() before OPEN */
		if (!wcn->sbuf4_laid_out) {
			sbuf4_layout(wcn);
			wcn->sbuf4_laid_out = true;
		}
		smsg_send(wcn, CH_PIPE, T_DONE, DONE_SBUF_INIT,
			  wcn->reg_phys[REG_SMEM] + SBUF4_OFF, 100);
		wcn->sbuf4_ready = true;
		wake_up(&wcn->sbuf_wq);
		dev_info(wcn->dev, "sbuf ch 4 ready\n");
		return;
	}
	if (ch == CH_LOG && type == T_CMD && flag == CMD_SBUF_INIT) {
		u32 phys = wcn->reg_phys[REG_SMEM] + SBUF5_OFF;

		wcn->slog_ready = true;
		/* The slog header was laid out before we opened ch 5; now
		 * cp2 asks us to confirm it with the base address */
		smsg_send(wcn, CH_LOG, T_DONE, DONE_SBUF_INIT, phys, 100);
		wake_up(&wcn->sbuf_wq);
		dev_info(wcn->dev, "sbuf ch 5 (slog) ready\n");
		return;
	}
	if (ch == CH_PIPE && type == T_EVENT && flag == EVT_WRPTR &&
	    value < SBUF4_RINGS) {
		struct wcn_sbuf_ring *rr = &wcn->ring[value];

		if (trace)
			dev_info(wcn->dev, "wrptr ring=%u\n", value);
		wake_up(&rr->wq);
		if (rr->nf.cb)
			rr->nf.cb(rr->nf.data, WCN_SBUF_RX);
		return;
	}
	if (ch == CH_PIPE && type == T_EVENT && flag == EVT_RDPTR &&
	    value < SBUF4_RINGS) {
		wake_up(&wcn->ring[value].wq);
		return;
	}

	/* sblock: cp2 sends OPEN then CMD_SBLOCK_INIT after it processes the
	 * first command (DEV_OPEN). We lay out the pool and answer. SEND
	 * events wake the receive path (sblock_receive is non-blocking so
	 * clients re-check); RELEASE wakes anyone waiting to send */
	if ((ch == CH_DATA || ch == CH_EVENT) && type == T_CMD &&
	    flag == CMD_SBLOCK_INIT) {
		struct wcn_sblock *s = sblock_for(wcn, ch);
		int sret;

		if (!s->ready) {
			sblock_layout(wcn, ch);
			s->ready = true;
		}
		sret = smsg_send(wcn, ch, T_DONE, DONE_SBLOCK_INIT,
				 wcn->reg_phys[REG_SMEM] + s->off, 100);
		wake_up(&wcn->sblock_wq);
		dev_info(wcn->dev,
			 "sblock ch %u ready, DONE addr=0x%x sret=%d\n",
			 ch, wcn->reg_phys[REG_SMEM] + s->off, sret);
		return;
	}
	if ((ch == CH_DATA || ch == CH_EVENT) && type == T_EVENT) {
		struct wcn_sblock *s = sblock_for(wcn, ch);

		if (flag == EVT_SBLK_SEND) {
			wake_up(&s->recv_wq);
			if (s->nf.cb)
				s->nf.cb(s->nf.data, WCN_SBLOCK_RX);
		} else if (flag == EVT_SBLK_RELEASE) {
			wake_up(&s->get_wq);
			if (s->nf.cb)
				s->nf.cb(s->nf.data, WCN_SBLOCK_TX_FREE);
		}
		return;
	}
	/* every rx is already logged at the top; unhandled messages are
	 * harmless -- cp2 sends asserts, extra OPEN_MAGIC acks and so on */
}

static irqreturn_t sipc_thread(int irq, void *data)
{
	struct wcn *wcn = data;
	void __iomem *r = wcn->reg[REG_CP2] + RING_OFF;
	u32 rd, wr;

	/* Loop until the ring is empty: cp2 may write more messages while
	 * we are processing, and the hardirq doorbell ack covers only the
	 * one ring we were told about -- new writes may not raise a new IRQ
	 * if the level-triggered one is still pending */
	for (;;) {
		rd = readl(r + SMSG_RX_RD);
		wr = readl(r + SMSG_RX_WR);
		if (rd == wr)
			break;
		while (rd != wr) {
			u32 slot = SMSG_RX + (rd & (SMSGS - 1)) * 8;
			u32 w0 = readl(r + slot);
			u32 value = readl(r + slot + 4);

			dispatch(wcn, w0 & 0xff, (w0 >> 8) & 0xff,
				 w0 >> 16, value);
			rd++;
		}
		wmb();
		writel(rd, r + SMSG_RX_RD);
	}
	wake_up(&wcn->tx_wq);
	return IRQ_HANDLED;
}

/* Load the firmware blob into the cp2 image region. Deferred by
 * request_firmware if the filesystem isn't up yet. */
static int load_firmware(struct wcn *wcn)
{
	const struct firmware *fw;
	int ret;

	ret = request_firmware(&fw, wcn->fw_name, wcn->dev);
	if (ret)
		return dev_err_probe(wcn->dev, ret, "firmware %s\n",
				     wcn->fw_name);
	if (fw->size > (0x201000 - IMAGE_OFF - 0x1000)) {
		release_firmware(fw);
		return dev_err_probe(wcn->dev, -E2BIG, "firmware too large\n");
	}
	memcpy_toio(wcn->reg[REG_CP2] + IMAGE_OFF, fw->data, fw->size);
	dev_info(wcn->dev, "loaded %zu bytes of %s\n", fw->size, wcn->fw_name);
	release_firmware(fw);
	return 0;
}

/* The three-word trampoline sprd_cproc writes into IRAM 0x50003000 before
 * lifting cp2 out of reset: ldr r0, [pc]; bx r0; .word 0x8a808000. Once
 * cp2 runs, its own vector table takes over the window */
static void write_trampoline(struct wcn *wcn)
{
	writel(0xe59f0000, wcn->reg[REG_IRAM] + 0);  /* ldr r0, [pc] */
	writel(0xe12fff10, wcn->reg[REG_IRAM] + 4);  /* bx r0 */
	writel(0x8a808000, wcn->reg[REG_IRAM] + 8);  /* entry */
}

/* /proc/cpwcn/stop: wait for cp2 to leave any sleep transition, then
 * reset, deep sleep, shutdown. SLEEP_STATUS bits 12-15 clear means not
 * busy; cp2.c polled it for 200 ms. */
static void cp2_stop(struct wcn *wcn)
{
	unsigned int v;
	int i;

	for (i = 0; i < 200; i++) {
		if (!regmap_read(wcn->pmu, SLEEP_STATUS, &v) &&
		    !(v & CP2_SLEEP_BUSY))
			break;
		usleep_range(1000, 2000);
	}
	regmap_set_bits(wcn->pmu, CP_SOFT_RST, CP2_RST);
	regmap_set_bits(wcn->pmu, PD_CP2_SYS_CFG, CP2_DEEPSLEEP);
	regmap_set_bits(wcn->pmu, PD_CP2_SYS_CFG, CP2_SHUTDOWN);
	/* restore AON gates we lit at start */
	regmap_write_bits(wcn->aon, AON_EB0,
			  AON_IPI_EB | AON_MSPI_EB,
			  wcn->aon_eb0_saved & (AON_IPI_EB | AON_MSPI_EB));
}

/* /proc/cpwcn/start, sprd_cproc_native_cp2_start's SCX30G branch: AON
 * gates, trampoline, PMU dance (shutdown lifted, reset pulsed around deep
 * sleep lifted). cp2.c proved the order against the frame's firmware. */
static int cp2_start(struct wcn *wcn)
{
	unsigned int v;
	int ret, i;

	ret = regmap_read(wcn->aon, AON_EB0, &wcn->aon_eb0_saved);
	if (ret)
		return ret;
	regmap_set_bits(wcn->aon, AON_EB0, AON_IPI_EB | AON_MSPI_EB);
	/* Zero the smsg ring page BEFORE lifting cp2 from shutdown: cp2
	 * should initialise it itself, but the page still holds whatever the
	 * last session left (Android, our previous bringup, cp2 crashing
	 * mid-smsg). User-space cp2.c does this; without it DEV_OPEN hangs
	 * after sblock DONE because cp2 interprets stale ring state */
	memset_io(wcn->reg[REG_CP2] + RING_OFF, 0, 0x1000);
	write_trampoline(wcn);
	regmap_clear_bits(wcn->pmu, PD_CP2_SYS_CFG, CP2_SHUTDOWN);
	usleep_range(5000, 6000);
	regmap_set_bits(wcn->pmu, CP_SOFT_RST, CP2_RST);
	usleep_range(5000, 6000);
	regmap_clear_bits(wcn->pmu, PD_CP2_SYS_CFG, CP2_DEEPSLEEP);
	usleep_range(5000, 6000);
	regmap_clear_bits(wcn->pmu, CP_SOFT_RST, CP2_RST);
	for (i = 0; i < 100; i++) {
		if (!regmap_read(wcn->pmu, CP_SOFT_RST, &v) && !(v & CP2_RST))
			return 0;
		usleep_range(1000, 2000);
	}
	dev_err(wcn->dev, "cp2 reset bit stuck\n");
	return -EIO;
}

/* Refcounted. The first up loads the firmware (sleeps on request_firmware)
 * and starts cp2; the last down stops it. */
/* After cp2 is up, open the channels we care about and wait for the sbuf
 * handshake. cp2 opens channel 4 itself before the AP does (seen in
 * docs/wifi.md wifi2), so our T_OPEN is a nudge; the real wait is for
 * SBUF_INIT. Channels 7 and 8 (sblock) stay unopened here -- cp2 only
 * asks for the SBLOCK_INIT after the radio sends its first command on
 * sbuf ring 7, which is sprdwl's job; dispatch() handles it there. */
static int wait_sbuf4(struct wcn *wcn)
{
	static const char armlog[] = "at+armlog=1\r";
	static const char enter[] = "at+cp2_enter_user=1\r";
	u8 drain[64];
	int ret, w, r;

	wcn->sbuf4_ready = false;
	wcn->sbuf4_laid_out = false;
	wcn->slog_ready = false;
	wcn->sblk[0].ready = false;
	wcn->sblk[1].ready = false;

	/* Lay out the sbuf4 header BEFORE opening ch 4 -- user-space cp2.c
	 * does this in hello() before send(CH_PIPE, T_OPEN) so cp2 has the
	 * ring layout to read as soon as it processes OPEN. dispatch()
	 * flips sbuf4_ready to true when cp2 sends CMD_SBUF_INIT. */
	sbuf4_layout(wcn);
	wcn->sbuf4_laid_out = true;

	/* Open ch 4, wait for cp2's CMD_SBUF_INIT, answer DONE. cp2
	 * opens ch 4 itself before the AP does -- our T_OPEN is a nudge;
	 * the real wait is for SBUF_INIT */
	ret = smsg_send(wcn, CH_PIPE, T_OPEN, OPEN_MAGIC, 0, 1000);
	if (ret)
		return ret;
	ret = wait_event_interruptible_timeout(wcn->sbuf_wq,
					       wcn->sbuf4_ready,
					       msecs_to_jiffies(2000));
	if (ret < 0)
		return ret;
	if (!ret)
		return -ETIMEDOUT;

	/* Lay out the slog's own header BEFORE opening ch 5. cp2 reads
	 * it as soon as it processes our T_OPEN, so laying it out only
	 * in response to CMD_SBUF_INIT (as the old code did) leaves cp2
	 * reading garbage; the trace comes back as binary noise instead
	 * of the firmware's own text. Then open ch 5 and wait for its
	 * SBUF_INIT handshake BEFORE writing at+armlog=1 to its ring --
	 * the old order (open ch 4,5,7,8 together) sent that AT command
	 * to an uninitialised sbuf, so cp2 never saw armlog=1 (seen in
	 * docs/wifi.md wifi7: kernel armlog read=-110, user-space OK) */
	slog_layout(wcn);
	smsg_send(wcn, CH_LOG, T_OPEN, OPEN_MAGIC, 0, 100);
	ret = wait_event_interruptible_timeout(wcn->sbuf_wq,
					       wcn->slog_ready,
					       msecs_to_jiffies(2000));
	if (ret < 0)
		return ret;
	if (!ret)
		return -ETIMEDOUT;

	/* at+armlog=1 enables cp2's internal trace on ring 5; without it
	 * enter_user doesn't respond (seen in trial 5). at+cp2_enter_user=1
	 * is what the frame's wcnd sends on a user build ("in user
	 * version, need to config CP2 to enter user mode!!!"); without
	 * it cp2 processes nothing on ring 7 (docs/wifi.md wifi4). cp2
	 * can take ~1 s to answer, so the read timeout is generous */
	w = wcn_sbuf_write(wcn, WCN_RING_AT, armlog, sizeof(armlog) - 1, 500);
	r = wcn_sbuf_read(wcn, WCN_RING_AT, drain, sizeof(drain), 3000);
	dev_info(wcn->dev, "armlog: write=%d read=%d first=%02x%02x%02x%02x\n",
		 w, r, drain[0], drain[1], drain[2], drain[3]);
	msleep(200);
	w = wcn_sbuf_write(wcn, WCN_RING_AT, enter, sizeof(enter) - 1, 500);
	r = wcn_sbuf_read(wcn, WCN_RING_AT, drain, sizeof(drain), 3000);
	dev_info(wcn->dev, "enter_user: write=%d read=%d first=%02x%02x%02x%02x\n",
		 w, r, drain[0], drain[1], drain[2], drain[3]);
	msleep(200);

	/* Lay out the sblock pools BEFORE opening ch 7 and 8, matching
	 * user-space cp2.c's order: cp2 reads the pool header during
	 * CMD_SBLOCK_INIT handling, so leaving the layout to dispatch()
	 * (which runs when cp2 asks) creates a window where cp2 reads
	 * an empty pool before we fill it. The dispatch handler notices
	 * ->ready is already set and only sends DONE */
	sblock_layout(wcn, CH_DATA);
	wcn->sblk[0].ready = true;
	sblock_layout(wcn, CH_EVENT);
	wcn->sblk[1].ready = true;
	smsg_send(wcn, CH_DATA, T_OPEN, OPEN_MAGIC, 0, 100);
	smsg_send(wcn, CH_EVENT, T_OPEN, OPEN_MAGIC, 0, 100);
	msleep(100);
	return 0;
}

/* Everything after cp2_stop() must keep off the IPI block: it is gated
 * again (AON_IPI_EB), and touching a gated peripheral stalls the bus with
 * no fault to report. So no ISR (it writes the ack, and cp2 may have rung
 * on its way down), no thread in dispatch, and no sender getting as far as
 * the doorbell -- the sbuf and sblock calls check these ready flags. The
 * slog thread used to survive this and a second power-up added another
 * one, both draining the same ring. */
static void wcn_stop_locked(struct wcn *wcn)
{
	if (wcn->slog_thread) {
		kthread_stop(wcn->slog_thread);
		wcn->slog_thread = NULL;
	}
	if (!wcn->irq_masked) {
		disable_irq(wcn->irq_sipc);
		wcn->irq_masked = true;
	}
	wcn->sbuf4_ready = false;
	wcn->sblk[0].ready = false;
	wcn->sblk[1].ready = false;
	cp2_stop(wcn);
	wcn->running = false;
}

int wcn_power_up(struct wcn *wcn)
{
	int ret = 0;

	mutex_lock(&wcn->lock);
	if (!wcn->users) {
		cp2_stop(wcn);		/* defensive: cp2 may have been left running */
		ret = load_firmware(wcn);
		if (!ret)
			ret = cp2_start(wcn);
		if (!ret) {
			wcn->running = true;
			wcn->boots++;
			if (wcn->irq_masked) {	/* cp2_start lit the IPI gate */
				enable_irq(wcn->irq_sipc);
				wcn->irq_masked = false;
			}
			wcn->slog_ready = false;
			wcn->slog_thread = kthread_run(slog_thread_fn, wcn,
						       "wcn-slog");
			if (IS_ERR(wcn->slog_thread))
				wcn->slog_thread = NULL;
			ret = wait_sbuf4(wcn);
		}
		if (ret)
			wcn_stop_locked(wcn);
	}
	if (!ret)
		wcn->users++;
	mutex_unlock(&wcn->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(wcn_power_up);

void wcn_power_down(struct wcn *wcn)
{
	mutex_lock(&wcn->lock);
	if (WARN_ON(!wcn->users))
		goto out;
	if (--wcn->users == 0)
		wcn_stop_locked(wcn);
out:
	mutex_unlock(&wcn->lock);
}
EXPORT_SYMBOL_GPL(wcn_power_down);

u32 wcn_boot_count(struct wcn *wcn)
{
	return READ_ONCE(wcn->boots);
}
EXPORT_SYMBOL_GPL(wcn_boot_count);

struct wcn *wcn_get(struct device *child)
{
	struct device *parent = child->parent;

	if (!parent || !of_device_is_compatible(parent->of_node, "sprd,sc7731g-wcn"))
		return NULL;
	return dev_get_drvdata(parent);
}
EXPORT_SYMBOL_GPL(wcn_get);

/* The cp2 and smem regions sit inside reserved-memory ranges (no-map) and
 * cp2 is told physical addresses, so cached mapping would miscohere; the
 * IPI/IRAM are MMIO. Use strict ioremap for all four so writes retire in
 * order on this ARM core before the doorbell -- the smsg ring and the sbuf
 * pointers are the handshake with cp2 and _wc's write-combining buffers
 * can reorder them around the doorbell write and lose a message */
static void __iomem *map_region(struct device *dev, int i, struct resource *r)
{
	return devm_ioremap(dev, r->start, resource_size(r));
}

static int wcn_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct wcn *wcn;
	int i, ret;

	wcn = devm_kzalloc(dev, sizeof(*wcn), GFP_KERNEL);
	if (!wcn)
		return -ENOMEM;
	wcn->dev = dev;
	mutex_init(&wcn->lock);
	spin_lock_init(&wcn->tx_lock);
	init_waitqueue_head(&wcn->tx_wq);
	init_waitqueue_head(&wcn->sbuf_wq);
	init_waitqueue_head(&wcn->sblock_wq);
	for (i = 0; i < SBUF4_RINGS; i++)
		init_waitqueue_head(&wcn->ring[i].wq);
	wcn->sblk[0] = (struct wcn_sblock){ .off = SBLK7_OFF,
		.txn = SBLK7_TXN, .txsz = SBLK7_SIZE,
		.rxn = SBLK7_RXN, .rxsz = SBLK7_SIZE };
	wcn->sblk[1] = (struct wcn_sblock){ .off = SBLK8_OFF,
		.txn = SBLK8_N, .txsz = SBLK8_SIZE,
		.rxn = SBLK8_N, .rxsz = SBLK8_SIZE };
	for (i = 0; i < 2; i++) {
		spin_lock_init(&wcn->sblk[i].tx_lock);
		spin_lock_init(&wcn->sblk[i].rx_lock);
		init_waitqueue_head(&wcn->sblk[i].get_wq);
		init_waitqueue_head(&wcn->sblk[i].recv_wq);
	}
	platform_set_drvdata(pdev, wcn);

	for (i = 0; i < REG_NR; i++) {
		struct resource *r = platform_get_resource(pdev,
							   IORESOURCE_MEM, i);

		if (!r)
			return dev_err_probe(dev, -EINVAL,
					     "no %s region\n", reg_name[i]);
		wcn->reg[i] = map_region(dev, i, r);
		if (IS_ERR(wcn->reg[i]))
			return dev_err_probe(dev, PTR_ERR(wcn->reg[i]),
					     "ioremap %s\n", reg_name[i]);
		wcn->reg_phys[i] = r->start;
	}

	wcn->pmu = syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,pmu-apb");
	if (IS_ERR(wcn->pmu))
		return dev_err_probe(dev, PTR_ERR(wcn->pmu),
				     "sprd,pmu-apb\n");
	wcn->aon = syscon_regmap_lookup_by_phandle(dev->of_node, "sprd,aon-apb");
	if (IS_ERR(wcn->aon))
		return dev_err_probe(dev, PTR_ERR(wcn->aon),
				     "sprd,aon-apb\n");

	wcn->irq_sipc = platform_get_irq_byname(pdev, "sipc");
	if (wcn->irq_sipc < 0)
		return wcn->irq_sipc;
	wcn->irq_wdt = platform_get_irq_byname(pdev, "watchdog");
	if (wcn->irq_wdt < 0)
		return wcn->irq_wdt;

	if (of_property_read_string(dev->of_node, "firmware-name", &wcn->fw_name))
		wcn->fw_name = "sprd/wcnmodem.bin";

	/* Make sure cp2 is off whatever state the bootloader left it in */
	cp2_stop(wcn);

	ret = devm_request_threaded_irq(dev, wcn->irq_sipc, sipc_isr,
					sipc_thread, IRQF_ONESHOT,
					"wcn-sipc", wcn);
	if (ret)
		return dev_err_probe(dev, ret, "sipc irq\n");

	dev_info(dev, "SC7731G WCN at cp2 %pR, smem %pR, fw %s\n",
		 platform_get_resource(pdev, IORESOURCE_MEM, REG_CP2),
		 platform_get_resource(pdev, IORESOURCE_MEM, REG_SMEM),
		 wcn->fw_name);

	/* Bring up DT children (sprdwl, BT); each probes with wcn_get(child)
	 * and calls wcn_power_up when it needs cp2 */
	return devm_of_platform_populate(dev);
}

static void wcn_remove(struct platform_device *pdev)
{
	struct wcn *wcn = platform_get_drvdata(pdev);

	of_platform_depopulate(wcn->dev);
	if (wcn->running)
		cp2_stop(wcn);
}

static const struct of_device_id wcn_of_match[] = {
	{ .compatible = "sprd,sc7731g-wcn" },
	{}
};
MODULE_DEVICE_TABLE(of, wcn_of_match);

static struct platform_driver wcn_driver = {
	.probe = wcn_probe,
	.remove = wcn_remove,
	.driver = {
		.name = "wcn-sc7731g",
		.of_match_table = wcn_of_match,
	},
};
module_platform_driver(wcn_driver);

MODULE_DESCRIPTION("Spreadtrum SC7731G WCN co-processor and SIPC");
MODULE_LICENSE("GPL");
