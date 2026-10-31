// SPDX-License-Identifier: GPL-2.0-only
/*
 * cp2 -- start the SC7731G's WCN co-processor (cp2: the Wi-Fi/BT/FM ARM)
 * from user space and talk to it over SIPC, with nothing but /dev/mem.
 *
 *   cp2 regs          power/reset/IPI state, the smsg ring, a checksum of
 *                     cp2's RAM past the image (it changes once cp2 runs)
 *   cp2 stop          what the vendor's sprd_cproc does on /proc/cpwcn/stop
 *   cp2 load FILE     copy FILE (the wcnmodem partition) to 0x8a808000
 *   cp2 start         IRAM trampoline + the PMU sequence of /proc/cpwcn/start
 *   cp2 hello [SECS]  open SIPC channel 4, answer cp2's sbuf init, send
 *                     at+spatgetcp2info on rings 0 and 5, print what comes back
 *   cp2 rings         the sbuf rings' pointers (after hello)
 *   cp2 watch [SECS]  print every smsg cp2 sends
 *   cp2 scan [SECS [MODE [NVM [eng]]]]
 *                     after hello: cp2's log (channel 5) and user mode,
 *                     the sblock 7/8 pools, WIFI_CMD_DEV_OPEN (MODE,
 *                     default 0x81 = station | zero-copy, with the NVM
 *                     calibration block) and WIFI_CMD_SCAN, then the
 *                     scandone event's BSS list (docs/wifi.md wifi4)
 *   cp2 connect SECS SSID BSSID CHAN PMKFILE [NVM]
 *                     after hello: the same bring-up, then a WPA2-PSK
 *                     association the way the frame's sprdwl.ko does it
 *                     (cp2 runs the 4-way handshake from the PMK), the data
 *                     frames that arrive for 15 s, and a disconnect
 *   cp2 sblk          the sblock 7/8 ring and pool headers
 *   cp2 slogh         the log channel's sbuf header
 *   cp2 adibits REG SET CLR
 *                     a bit-only read-modify-write of one SC2713S PMIC
 *                     register; 0 0 only reads
 *
 * Why: there is no mainline driver for any of the vendor's WCN stack
 * (sprd_cproc, sipc/smsg/sbuf/sblock, sprdwl; docs/wifi.md). Before writing
 * one, this checks the two things a driver would rest on -- that cp2 boots
 * from our kernel at all, and that the SIPC transport works as the vendor
 * source describes -- without a kernel change: the smsg ring is polled
 * instead of taking cp2's interrupt (SPI 73), and the sbuf rings are laid
 * out by hand in the shared memory the dts already reserves.
 *
 * Where the numbers come from: the frame's own vendor DT (scproc@8a800000
 * "cpwcn", sipc_wcn under sipc@87800000, spipe_cpwcn) and the SM-G531H GPL
 * kernel's drivers/misc/sprd_cproc, drivers/sipc and chip_x30g register
 * headers. The firmware is the frame's: Project Version CP2_WCN_14D_W16.26.3.
 *
 * Freestanding like devmem.c (no ARM libc in the build image). /dev/mem
 * with O_SYNC maps as device memory, where unaligned and merged accesses
 * fault: everything goes through volatile 32-bit or 8-bit accesses.
 */

#define O_RDONLY	0
#define O_RDWR		2
#define O_SYNC		04010000
#define PROT_READ	1
#define PROT_WRITE	2
#define MAP_SHARED	1

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

static long sys(long nr, long a, long b, long c, long d, long e, long f)
{
	register long r7 asm("r7") = nr;
	register long r0 asm("r0") = a;
	register long r1 asm("r1") = b;
	register long r2 asm("r2") = c;
	register long r3 asm("r3") = d;
	register long r4 asm("r4") = e;
	register long r5 asm("r5") = f;

	asm volatile("svc #0" : "+r"(r0)
		     : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5)
		     : "memory");
	return r0;
}

#define sys_exit(c)		sys(1, c, 0, 0, 0, 0, 0)
#define sys_read(fd, b, n)	sys(3, fd, (long)(b), n, 0, 0, 0)
#define sys_write(fd, b, n)	sys(4, fd, (long)(b), n, 0, 0, 0)
#define sys_open(p, fl)		sys(5, (long)(p), fl, 0, 0, 0, 0)
#define sys_close(fd)		sys(6, fd, 0, 0, 0, 0, 0)
#define sys_nanosleep(t)	sys(162, (long)(t), 0, 0, 0, 0, 0)
/* mmap2 takes the offset in 4 KiB pages */
#define sys_mmap2(len, prot, fl, fd, pg) sys(192, 0, len, prot, fl, fd, pg)

#define dmb()	asm volatile("dmb" ::: "memory")

/* --- the hardware, from the vendor DT and chip_x30g headers --- */

/* PMU_APB. The vendor DT's scproc "cpwcn": sprd,ctrl-reg <0x68 0x68 0xb0
 * 0xb0>, sprd,ctrl-mask <0x2000000 0x10000000 0x04 0x04>, in the order
 * shutdown, deep sleep, reset, status */
#define PMU		0x402b0000
#define PD_CP2_ARM9_CFG	0x5c
#define PD_CP2_WIFI_CFG	0x60
#define PD_CP2_SYS_CFG	0x68		/* bit 25 force shutdown, 28 force deep sleep */
#define  CP2_SHUTDOWN	(1u << 25)
#define  CP2_DEEPSLEEP	(1u << 28)
#define CP_SOFT_RST	0xb0		/* bit 2: cp2 held in reset */
#define  CP2_RST	(1u << 2)
#define SLEEP_STATUS	0xd4		/* bits 12-15: cp2's sleep state */

/* AON_APB EB0: bit 21 the IPI block, bit 23 MSPI (the RF2351's SPI; the
 * vendor's sprd_2351.c never turns it off "because BT can't work" then).
 * clk_mspi is in our clock tree and unclaimed, so the clock core turns it
 * off at boot: start sets both again */
#define AON		0x402e0000
#define  IPI_EB		(1u << 21)
#define  MSPI_EB	(1u << 23)

/* the IPI block (sipc_wcn: sprd,ap2cp 0x402c0000, sprd,cp2ap 0x402c0004,
 * sprd,trig = sprd,clr = 0x100): writing 0x100 to the first raises cp2's
 * interrupt, to the second clears ours (SPI 73) */
#define IPI		0x402c0000
#define  IPI_WCN	0x100

/* cp2 boots from IRAM: the vendor's sprd,iram-data is "ldr r0, [pc]; bx r0;
 * .word 0x8a808000", copied to 0x50003000 on every start */
#define IRAM		0x50003000

/* cp2's memory, reserved in the dts (cp2@8a800000, 0x201000): the image at
 * +0x8000 (its reset vector points into it: the firmware is linked there),
 * the smsg ring in the last page */
#define CP2MEM		0x8a800000
#define CP2SIZE		0x201000
#define IMAGE_OFF	0x8000
#define RING_OFF	0x200000
/* the smsg ring, AP's view (drivers/sipc/sipc.c): 128 tx and 128 rx
 * messages of 8 bytes, then the four pointers */
#define TXBUF		0x000
#define RXBUF		0x400
#define TX_RD		0x800
#define TX_WR		0x804
#define RX_RD		0x808
#define RX_WR		0x80c
#define SMSGS		128

/* shared memory for the sbuf rings: sipc_wcn's smem window, inside the
 * dts's cp-shm@87800000. The vendor allocates sbufs from all of
 * 0x87800000-0x879fffff; cp2 is told the address, so any part of it works */
#define SMEM		0x87940000
#define SMEMSIZE	0xc0000

/* smsg (include/linux/sipc.h) */
#define CH_PIPE		4		/* spipe_wcn / sprdwl's sbuf: dst 3, channel 4 */
#define T_OPEN		1
#define T_CLOSE		2
#define T_DATA		3
#define T_EVENT		4
#define T_CMD		5
#define T_DONE		6
#define OPEN_MAGIC	0xbeee
#define CLOSE_MAGIC	0xeddd
#define CMD_SBUF_INIT	1
#define DONE_SBUF_INIT	2
#define EVT_WRPTR	1
#define EVT_RDPTR	2

/* spipe_cpwcn's geometry: 13 rings of 4 KiB each way. Ring 7 is sprdwl's
 * commands. wcnd names /dev/spipe_wcn5 its "ATCMD interface", but wifi1
 * (2026-10-27) sent AT+SPATGETCP2INFO there and cp2 never answered. wcnd's
 * own string is lowercase, "at+spatgetcp2info\r", and sits with its loop
 * check, whose interface is /dev/spipe_wcn0. So hello now asks on ring 0
 * first, then ring 5, and dumps every ring's pointers: a tx rdptr that
 * moved says cp2 read that ring, whether or not it answered */
#define RINGS		13
#define RINGSIZE	0x1000
#define AT_CMD		"at+spatgetcp2info\r"

/* sprdwl, over sbuf channel 4 ring 7 (commands) and sblock channels 7 (data)
 * and 8 (events). Taken from the Samsung GPL kernel's
 * drivers/net/wireless/sprdwl/sipc.h and drivers/sipc/sblock.h; the frame's
 * sprdwl.ko builds its DEV_OPEN as 8 + 1 + 6 + 256 + 8 = 279 bytes, the
 * same shape */
#define CH_DATA		7		/* SMSG_CH_DATA0 */
#define CH_EVENT	8		/* SMSG_CH_DATA1 */
#define WL_SBUF_RING	7		/* sbuf 4, ring 7 */

/* sblock init handshake (sblock.h), and the two data-direction events */
#define CMD_SBLOCK_INIT	1
#define DONE_SBLOCK_INIT 2
#define EVT_SBLK_SEND	1
#define EVT_SBLK_RELEASE 2

/* sblock geometry, from sipc.h: ring 7 data path, ring 8 events */
#define BLK7_TXN	64
#define BLK7_RXN	128
#define BLK7_SIZE	1664
#define BLK8_N		1
#define BLK8_SIZE	(10 * 1024)

/* sprdwl message header: u32 msg_hdr = type | len << 16, then u16 cmd_tag
 * (type << 12 | dir << 11 | level << 8 | id) and u16 seq_no. A response adds
 * u16 status and u16 reserved. An event is u16 event_tag + u16 seq + u16 resv */
#define MSG_CMD		0
#define MSG_CMD_RSP	1
#define MSG_EVENT	2
#define CMD_TYPE_SET	1
#define CMD_LEVEL_NORM	1
#define CMD_DIR_AP2CP	0
#define CMD_HDR_SZ	8
#define RSP_HDR_SZ	12
#define EVT_HDR_SZ	10

#define WIFI_CMD_SCAN		1
#define WIFI_CMD_DEV_OPEN	20
#define WIFI_CMD_DEV_CLOSE	21
#define WIFI_EV_CONNECT		0
#define WIFI_EV_DISCONNECT	1
#define WIFI_EV_SCANDONE	2
#define WIFI_EV_READY		4

#define DEV_MODE_STA	1		/* not the Samsung source's 0: the frame's sprdwl_open_mac maps
				 * NL80211_IFTYPE_STATION (and P2P_CLIENT) to 1, AP and P2P_GO to
				 * 2, and this firmware's open handler takes 0 as "error mode" */
#define FW_ZEROCOPY	0x80		/* sprdwl.h: the frame's firmware is the zero-copy build */
#define NVM_SIZE	256		/* struct wifi_nvm_data, tools/wcn-nvm.py */

/* smem offsets we add on top of hello's sbuf layout. The sbuf header +
 * ring buffers take 0x1a1a4 bytes from SMEM; pick safely aligned offsets
 * past that for channel 7 and then channel 8 (sizes below).
 * ch 7:  96 + 64*1664 (tx) + 128*1664 (rx) + 2*(64+128)*8 = 325728
 * ch 8:  96 + 1*10240 + 1*10240 + 2*(1+1)*8 = 20640
 * Both inside SMEMSIZE = 768 KiB */
#define SBLK7_OFF	0x1b000		/* > 0x1a1a4, aligned */
#define SBLK8_OFF	0x6b000		/* > 0x1b000 + 325728 (= 0x6a4e0) */

/* --- output --- */

static void say(const char *s)
{
	int n = 0;

	while (s[n])
		n++;
	sys_write(1, s, n);
}

static void hex(u32 v)
{
	char b[11];
	int i;

	/* no initialiser: gcc may fill the tail with a memset call, and
	 * there is no libc to link one from */
	b[0] = '0';
	b[1] = 'x';
	for (i = 9; i >= 2; i--, v >>= 4)
		b[i] = "0123456789abcdef"[v & 0xf];
	b[10] = 0;
	say(b);
}

/* -Os turns / and % into libgcc calls, and there is no libgcc: digits by
 * subtraction */
static void dec(u32 v)
{
	static const u32 p10[] = { 1000000000, 100000000, 10000000, 1000000,
				   100000, 10000, 1000, 100, 10, 1 };
	char b[11];
	int i, n = 0;

	for (i = 0; i < 10; i++) {
		char d = '0';

		while (v >= p10[i]) {
			v -= p10[i];
			d++;
		}
		if (d != '0' || n || i == 9)
			b[n++] = d;
	}
	b[n] = 0;
	say(b);
}

static void kv(const char *k, u32 v)
{
	say("cp2: ");
	say(k);
	say(" ");
	hex(v);
	say("\n");
}

/* gcc at -Os turns byte copies and fills past a few bytes into memcpy /
 * memset calls, and there is no libc to link them from */
void *memcpy(void *d, const void *s, unsigned int n)
{
	u8 *dd = d;
	const u8 *ss = s;

	while (n--)
		*dd++ = *ss++;
	return d;
}

void *memset(void *d, int c, unsigned int n)
{
	u8 *dd = d;

	while (n--)
		*dd++ = c;
	return d;
}

static void hexbyte(u8 v)
{
	char b[3];

	b[0] = "0123456789abcdef"[v >> 4];
	b[1] = "0123456789abcdef"[v & 0xf];
	b[2] = 0;
	say(b);
}

static void hexdump(const char *what, const u8 *p, u32 n)
{
	u32 i;

	say("cp2: ");
	say(what);
	for (i = 0; i < n; i++) {
		if (i && (i % 16 == 0))
			say("\ncp2:  ");
		else
			say(" ");
		hexbyte(p[i]);
	}
	say("\n");
}

static unsigned long num(const char *s)
{
	unsigned long v = 0, base = 10;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		base = 16;
		s += 2;
	}
	for (; *s; s++) {
		int d = *s >= '0' && *s <= '9' ? *s - '0' :
			*s >= 'a' && *s <= 'f' ? *s - 'a' + 10 :
			*s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : 99;
		if (d >= (int)base)
			break;
		v = v * base + d;
	}
	return v;
}

static void msleep(long ms)
{
	long t[2] = { 0, 0 };

	for (; ms >= 1000; ms -= 1000)
		t[0]++;
	t[1] = ms * 1000000;
	sys_nanosleep(t);
}

/* --- /dev/mem --- */

static long memfd = -1;

static volatile u32 *map(u32 phys, u32 len)
{
	long m;

	if (memfd < 0)
		memfd = sys_open("/dev/mem", O_RDWR | O_SYNC);
	if (memfd < 0) {
		say("cp2: cannot open /dev/mem\n");
		sys_exit(1);
	}
	m = sys_mmap2((len + 0xfff) & ~0xfff, PROT_READ | PROT_WRITE,
		      MAP_SHARED, memfd, phys >> 12);
	if ((unsigned long)m >= -4095UL) {
		say("cp2: mmap failed at ");
		hex(phys);
		say("\n");
		sys_exit(1);
	}
	return (volatile u32 *)(m + (phys & 0xfff));
}

static volatile u32 *pmu, *aon, *ipi, *iram, *cp2, *ring, *smem;

#define R(base, off)	((base)[(off) / 4])

static void maps(void)
{
	pmu = map(PMU, 0x1000);
	aon = map(AON, 0x1000);
	ipi = map(IPI, 0x1000);
	iram = map(IRAM, 0x1000);
	cp2 = map(CP2MEM, CP2SIZE);
	ring = cp2 + RING_OFF / 4;
	smem = map(SMEM, SMEMSIZE);
}

static void set(volatile u32 *base, u32 off, u32 bits, int on)
{
	u32 v = R(base, off);

	R(base, off) = on ? v | bits : v & ~bits;
	dmb();
}

/* the word sum of cp2's RAM between the end of the image and the ring:
 * whatever cp2 runs on (its bss, stacks, heaps) is there, so a change
 * after start says cp2 executed */
static u32 tailsum(u32 from)
{
	u32 s = 0, i;

	for (i = from / 4; i < RING_OFF / 4; i++)
		s += cp2[i];
	return s;
}

static void regs(void)
{
	/* GIC distributor pending bits: SPI 73 (ID 105, cp2 -> AP smsg) and
	 * SPI 85 (ID 117, cp2's watchdog), both in ISPENDR3 */
	volatile u32 *gicd = map(0x12001000, 0x1000);
	u32 pend = R(gicd, 0x200 + 3 * 4);

	kv("pd_cp2_sys_cfg", R(pmu, PD_CP2_SYS_CFG));
	kv("pd_cp2_arm9_cfg", R(pmu, PD_CP2_ARM9_CFG));
	kv("pd_cp2_wifi_cfg", R(pmu, PD_CP2_WIFI_CFG));
	kv("cp_soft_rst", R(pmu, CP_SOFT_RST));
	kv("sleep_status", R(pmu, SLEEP_STATUS));
	kv("aon_apb_eb0", R(aon, 0));
	kv("ipi_ap2cp", R(ipi, 0));
	kv("ipi_cp2ap", R(ipi, 4));
	kv("iram_3000", R(iram, 0));
	kv("iram_3004", R(iram, 4));
	kv("iram_3008", R(iram, 8));
	kv("image_0", R(cp2, IMAGE_OFF));
	kv("image_8", R(cp2, IMAGE_OFF + 8));
	kv("ring_tx_rd", R(ring, TX_RD));
	kv("ring_tx_wr", R(ring, TX_WR));
	kv("ring_rx_rd", R(ring, RX_RD));
	kv("ring_rx_wr", R(ring, RX_WR));
	kv("gic_pend_ipi_spi73", (pend >> 9) & 1);
	kv("gic_pend_wdt_spi85", (pend >> 21) & 1);
	kv("tailsum_a0000", tailsum(0xa0000));
}

/* /proc/cpwcn/stop, sprd_cproc_native_cp2_stop: wait for cp2 to report
 * no sleep transition in progress, then reset, deep sleep, shutdown */
static void stop(void)
{
	int i;

	for (i = 0; i < 200 && (R(pmu, SLEEP_STATUS) & 0xf000); i++)
		msleep(1);
	if (i == 200)
		kv("stop: sleep status still busy", R(pmu, SLEEP_STATUS));
	set(pmu, CP_SOFT_RST, CP2_RST, 1);
	set(pmu, PD_CP2_SYS_CFG, CP2_DEEPSLEEP, 1);
	set(pmu, PD_CP2_SYS_CFG, CP2_SHUTDOWN, 1);
	say("cp2: stopped\n");
}

static u32 buf[1024];

/* /proc/cpwcn/modem: the partition, as is, at 0x8a808000. Refuses while
 * cp2 is powered: it would be running from what we overwrite */
static int load(const char *path)
{
	long fd, n;
	u32 off = IMAGE_OFF, i, bad = 0, total = 0;

	if (!(R(pmu, PD_CP2_SYS_CFG) & CP2_SHUTDOWN)) {
		say("cp2: powered, stop it first\n");
		return 1;
	}
	fd = sys_open(path, O_RDONLY);
	if (fd < 0) {
		say("cp2: cannot open ");
		say(path);
		say("\n");
		return 1;
	}
	while (off < RING_OFF && (n = sys_read(fd, buf, sizeof(buf))) > 0) {
		n = (n + 3) / 4;
		if (off + n * 4 > RING_OFF)
			n = (RING_OFF - off) / 4;
		for (i = 0; i < (u32)n; i++)
			cp2[off / 4 + i] = buf[i];
		dmb();
		for (i = 0; i < (u32)n; i++)
			bad += cp2[off / 4 + i] != buf[i];
		off += n * 4;
		total += n * 4;
	}
	sys_close(fd);
	say("cp2: loaded ");
	dec(total);
	say(" bytes at 0x8a808000, ");
	dec(bad);
	say(" words read back wrong\n");
	return bad != 0;
}

/* /proc/cpwcn/start, sprd_cproc_native_cp2_start's CONFIG_ARCH_SCX30G
 * branch, plus what the vendor kernel had set up by then: the IPI and MSPI
 * gates, and a clean smsg ring (cp2 should initialise it, but the page
 * still holds whatever the last Android session left) */
static void start(void)
{
	int i;

	set(aon, 0, IPI_EB | MSPI_EB, 1);
	for (i = 0; i < 0x1000 / 4; i++)
		ring[i] = 0;
	R(iram, 0) = 0xe59f0000;	/* ldr r0, [pc] */
	R(iram, 4) = 0xe12fff10;	/* bx r0 */
	R(iram, 8) = CP2MEM + IMAGE_OFF;
	dmb();
	set(pmu, PD_CP2_SYS_CFG, CP2_SHUTDOWN, 0);
	msleep(5);
	set(pmu, CP_SOFT_RST, CP2_RST, 1);
	msleep(5);
	set(pmu, PD_CP2_SYS_CFG, CP2_DEEPSLEEP, 0);
	msleep(5);
	set(pmu, CP_SOFT_RST, CP2_RST, 0);
	for (i = 0; i < 100 && (R(pmu, CP_SOFT_RST) & CP2_RST); i++)
		msleep(1);
	say(i == 100 ? "cp2: reset bit stuck\n" : "cp2: started\n");
}

/* --- smsg, polled --- */

static void ipi_kick(void)
{
	dmb();
	R(ipi, 0) = IPI_WCN;
	dmb();
}

static int send(u32 ch, u32 type, u32 flag, u32 value)
{
	u32 wr = R(ring, TX_WR), slot;

	if (wr - R(ring, TX_RD) >= SMSGS) {
		say("cp2: smsg tx ring full (cp2 not reading it)\n");
		return -1;
	}
	slot = TXBUF / 4 + (wr & (SMSGS - 1)) * 2;
	ring[slot] = ch | type << 8 | flag << 16;
	ring[slot + 1] = value;
	dmb();
	R(ring, TX_WR) = wr + 1;
	ipi_kick();
	say("cp2: tx ch=");
	dec(ch);
	say(" type=");
	dec(type);
	say(" flag=");
	hex(flag);
	say(" value=");
	hex(value);
	say("\n");
	return 0;
}

/* one message from cp2, or 0 if none; clears our IPI either way */
static int recv(u32 *ch, u32 *type, u32 *flag, u32 *value)
{
	u32 rd = R(ring, RX_RD), slot, w;

	R(ipi, 4) = IPI_WCN;
	if (rd == R(ring, RX_WR))
		return 0;
	slot = RXBUF / 4 + (rd & (SMSGS - 1)) * 2;
	w = ring[slot];
	*ch = w & 0xff;
	*type = (w >> 8) & 0xff;
	*flag = w >> 16;
	*value = ring[slot + 1];
	dmb();
	R(ring, RX_RD) = rd + 1;
	say("cp2: rx ch=");
	dec(*ch);
	say(" type=");
	dec(*type);
	say(" flag=");
	hex(*flag);
	say(" value=");
	hex(*value);
	say("\n");
	return 1;
}

/* --- sbuf on channel 4, laid out as drivers/sipc/sbuf.c does --- */

#define HSIZE		(4 + RINGS * 32)
#define RINGHDR(i)	(1 + (i) * 8)		/* in words */
#define TXADDR 0
#define TXSIZE 1
#define TXRD 2
#define TXWR 3
#define RXADDR 4
#define RXSIZE 5
#define RXRD 6
#define RXWR 7

static void sbuf_layout(void)
{
	u32 i, a;

	for (i = 0; i < (HSIZE + RINGS * 2 * RINGSIZE) / 4; i++)
		smem[i] = 0;
	smem[0] = RINGS;
	for (i = 0; i < RINGS; i++) {
		a = SMEM + HSIZE + 2 * RINGSIZE * i;
		smem[RINGHDR(i) + TXADDR] = a;
		smem[RINGHDR(i) + TXSIZE] = RINGSIZE;
		smem[RINGHDR(i) + RXADDR] = a + RINGSIZE;
		smem[RINGHDR(i) + RXSIZE] = RINGSIZE;
	}
	dmb();
}

static volatile u8 *ringbuf(u32 addr)
{
	return (volatile u8 *)smem + (addr - SMEM);
}

static volatile u32 *smemw(u32 phys)
{
	return smem + (phys - SMEM) / 4;
}

/* sblock pool layout, from drivers/sipc/sblock.c sblock_create(). smem
 * holds, in order per channel: two sblock_ring_header structs (12 u32 each:
 * txblk_addr, txblk_count, txblk_size, txblk_blks, txblk_rdptr, txblk_wrptr,
 * then the same for rx); the tx block area; the rx block area; the ring
 * tx+rx blks arrays; the pool tx+rx blks arrays. On start every block is
 * owned by the AP pool, so the pool.wrptr is advanced to count and the
 * ring is empty. cp2 moves blocks ring<->pool with EVT_SBLK_SEND / RELEASE
 * and we do the reverse */
#define SB_TXADDR 0
#define SB_TXCNT  1
#define SB_TXSZ   2
#define SB_TXBLKS 3
#define SB_TXRD   4
#define SB_TXWR   5
#define SB_RXADDR 6
#define SB_RXCNT  7
#define SB_RXSZ   8
#define SB_RXBLKS 9
#define SB_RXRD   10
#define SB_RXWR   11
#define SB_HDR_W  24		/* two ring_header structs, in u32 */
/* the pool header is the second of them: word 12, byte 48. wifi3 had it
 * at SB_HDR_W (the header's total size, i.e. inside the first tx block), so
 * cp2 read an empty pool and logged "sblock_get: channel is 7 is err" in
 * a loop -- it takes its zero-copy rx buffers from there */
#define SB_POOL   12

static void sblock_layout(u32 base, u32 txn, u32 txsz, u32 rxn, u32 rxsz)
{
	volatile u32 *h = smemw(base);
	u32 hsize = SB_HDR_W * 4;
	u32 tx = base + hsize;
	u32 rx = tx + txn * txsz;
	u32 r_tx_blks = rx + rxn * rxsz;
	u32 r_rx_blks = r_tx_blks + txn * 8;
	u32 p_tx_blks = r_rx_blks + rxn * 8;
	u32 p_rx_blks = p_tx_blks + txn * 8;
	u32 total = (p_rx_blks + rxn * 8) - base;
	u32 i;

	/* zero the whole region (header + buffers + blks arrays) */
	for (i = 0; i < total / 4; i++)
		h[i] = 0;

	/* ring: in-flight blocks. Both pointers 0 => empty */
	h[SB_TXADDR] = tx;		h[SB_TXCNT] = txn;
	h[SB_TXSZ]   = txsz;		h[SB_TXBLKS] = r_tx_blks;
	h[SB_RXADDR] = rx;		h[SB_RXCNT] = rxn;
	h[SB_RXSZ]   = rxsz;		h[SB_RXBLKS] = r_rx_blks;

	/* pool: free blocks. At start, the pool holds every block, so both
	 * .wrptr = count and the pool arrays are filled with each block's
	 * (addr, size) */
	h[SB_POOL + SB_TXADDR] = tx;	h[SB_POOL + SB_TXCNT] = txn;
	h[SB_POOL + SB_TXSZ]   = txsz;	h[SB_POOL + SB_TXBLKS] = p_tx_blks;
	h[SB_POOL + SB_TXWR]   = txn;
	h[SB_POOL + SB_RXADDR] = rx;	h[SB_POOL + SB_RXCNT] = rxn;
	h[SB_POOL + SB_RXSZ]   = rxsz;	h[SB_POOL + SB_RXBLKS] = p_rx_blks;
	h[SB_POOL + SB_RXWR]   = rxn;

	for (i = 0; i < txn; i++) {
		volatile u32 *b = smemw(p_tx_blks + i * 8);

		b[0] = tx + i * txsz;
		b[1] = txsz;
	}
	for (i = 0; i < rxn; i++) {
		volatile u32 *b = smemw(p_rx_blks + i * 8);

		b[0] = rx + i * rxsz;
		b[1] = rxsz;
	}
	dmb();
}

static void sbuf_write(u32 r, const char *s)
{
	volatile u32 *h = smem + RINGHDR(r);
	volatile u8 *tx = ringbuf(h[TXADDR]);
	u32 wr = h[TXWR];

	for (; *s; s++, wr++)
		tx[wr & (RINGSIZE - 1)] = *s;
	dmb();
	h[TXWR] = wr;
	send(CH_PIPE, T_EVENT, EVT_WRPTR, r);
}

/* print whatever cp2 has put in ring r's rx buffer, as text */
static void sbuf_drain(u32 r)
{
	volatile u32 *h = smem + RINGHDR(r);
	volatile u8 *rx = ringbuf(h[RXADDR]);
	u32 rd = h[RXRD], wr = h[RXWR], n = 0;
	char line[2];

	if (rd == wr)
		return;
	say("cp2: ring ");
	dec(r);
	say(" says: ");
	line[1] = 0;
	for (; rd != wr && n < RINGSIZE; rd++, n++) {
		u8 c = rx[rd & (RINGSIZE - 1)];

		line[0] = c == '\r' ? ' ' : c == '\n' ? '|' :
			  c >= ' ' && c < 127 ? c : '.';
		say(line);
	}
	say("\n");
	dmb();
	h[RXRD] = rd;
	send(CH_PIPE, T_EVENT, EVT_RDPTR, r);
}

/* --- sblock: pool handshake, receive, release ---
 *
 * sblock has the same handshake shape as sbuf: cp2 opens the channel,
 * sends CMD_SBLOCK_INIT, we answer DONE with the smem base of the pool
 * we laid out. After that cp2 moves blocks between the ring and the pool
 * by advancing ring.rxblk_wrptr and sending EVT_SBLK_SEND; we release
 * them with ring.rxblk_rdptr+1 and EVT_SBLK_RELEASE */

/* peek the ring 8 pool for a block from cp2; copy up to maxlen bytes of
 * its payload into out. Returns the block's length or -1. The block is
 * released with sblock_release() */
static u32 last_blk_addr, last_blk_len;

static int sblock_recv(u32 chan, u32 base, u8 *out, u32 maxlen)
{
	volatile u32 *h = smemw(base);		/* ring header */
	u32 rxpos, blkaddr, blklen, i, n;
	volatile u8 *src;

	if (h[SB_RXWR] == h[SB_RXRD])
		return -1;
	/* both counts we use (128 and 1) are powers of two; vendor's
	 * sblock_get_ringpos masks in that case to avoid __aeabi_uidivmod */
	rxpos = h[SB_RXRD] & (h[SB_RXCNT] - 1);
	{
		volatile u32 *b = smemw(h[SB_RXBLKS] + rxpos * 8);

		blkaddr = b[0];
		blklen = b[1];
	}
	h[SB_RXRD] = h[SB_RXRD] + 1;
	dmb();
	last_blk_addr = blkaddr;
	last_blk_len = blklen;

	src = ringbuf(blkaddr);
	n = blklen < maxlen ? blklen : maxlen;
	for (i = 0; i < n; i++)
		out[i] = src[i];
	say("cp2: sblock ");
	dec(chan);
	say(" recv ");
	dec(blklen);
	say(" bytes at ");
	hex(blkaddr);
	say("\n");
	return blklen;
}

static void sblock_release(u32 chan, u32 base)
{
	volatile u32 *h = smemw(base);		/* ring header */
	volatile u32 *p = h + SB_POOL;		/* pool header */
	u32 pos = p[SB_RXWR] & (p[SB_RXCNT] - 1);		/* power-of-two counts */
	volatile u32 *b = smemw(p[SB_RXBLKS] + pos * 8);

	b[0] = last_blk_addr;
	b[1] = p[SB_RXSZ];
	p[SB_RXWR] = p[SB_RXWR] + 1;
	dmb();
	send(chan, T_EVENT, EVT_SBLK_RELEASE, 0);
}

/* --- binary access to sbuf ring 7: sprdwl's command channel ---
 *
 * sbuf_write already did ASCII writes with a trailing EVT_WRPTR; these
 * mirror that for u8 bytes. sbuf_read_bytes polls until wrptr moves past
 * rdptr by at least `want` bytes or `tries` tenths of a second elapse */
static void sbuf_write_bytes(u32 r, const u8 *s, u32 n)
{
	volatile u32 *h = smem + RINGHDR(r);
	volatile u8 *tx = ringbuf(h[TXADDR]);
	u32 wr = h[TXWR], i;

	for (i = 0; i < n; i++, wr++)
		tx[wr & (RINGSIZE - 1)] = s[i];
	dmb();
	h[TXWR] = wr;
	send(CH_PIPE, T_EVENT, EVT_WRPTR, r);
}

/* cp2's own log, the vendor's slog_wcn: smsg channel 5, an sbuf with one
 * ring, 32 KiB AP -> cp2 and 256 KiB cp2 -> AP (vendor DT slog_cpwcn).
 * cp2 opens the channel itself at boot; once the AP opens its side and
 * answers SBUF_INIT, the firmware's trace -- "wifi_open_seth error",
 * "error mode=%d ..." and the rest -- lands in the rx ring as text. Laid
 * out after channel 8 in the smem window: 0x71000 .. ~0xb9030 of 0xc0000 */
#define CH_LOG		5
#define SLOG_OFF	0x71000
#define SLOG_TXSIZE	0x8000
#define SLOG_RXSIZE	0x40000

static u8 slog_init;

static void slog_layout(void)
{
	volatile u32 *h = smemw(SMEM + SLOG_OFF);
	u32 tx = SMEM + SLOG_OFF + 0x40;	/* after the 36-byte header */
	int i;

	for (i = 0; i < 0x40 / 4; i++)
		h[i] = 0;
	h[0] = 1;				/* ringnr */
	h[1 + TXADDR] = tx;
	h[1 + TXSIZE] = SLOG_TXSIZE;
	h[1 + RXADDR] = tx + SLOG_TXSIZE;
	h[1 + RXSIZE] = SLOG_RXSIZE;
	dmb();
}

/* print what cp2 has logged since the last call, line by line */
static void slog_drain(void)
{
	volatile u32 *h = smemw(SMEM + SLOG_OFF) + 1;
	volatile u8 *rx;
	u32 rd, wr, n = 0;
	char line[2];
	int bol = 1;

	if (!slog_init)
		return;
	rx = ringbuf(h[RXADDR]);
	rd = h[RXRD];
	wr = h[RXWR];
	if (rd == wr)
		return;
	line[1] = 0;
	for (; rd != wr && n < SLOG_RXSIZE; rd++, n++) {
		u8 c = rx[rd & (SLOG_RXSIZE - 1)];

		if (c == '\r' || c == 0)
			continue;
		if (bol) {
			say("cp2log: ");
			bol = 0;
		}
		line[0] = c == '\n' || (c >= ' ' && c < 127) ? c : '.';
		say(line);
		if (c == '\n')
			bol = 1;
	}
	if (!bol)
		say("\n");
	dmb();
	h[RXRD] = rd;
	send(CH_LOG, T_EVENT, EVT_RDPTR, 0);
}

/* flags set by pump() when cp2 completes its half of each handshake */
static u8 sblk7_open, sblk7_init, sblk8_open, sblk8_init;

/* one pass over whatever smsg cp2 has queued. Routes:
 *  - T_OPEN on ch 7/8 => record "cp2 opened its half"
 *  - T_CMD CMD_SBLOCK_INIT on ch 7/8 => answer DONE with the pool base
 *  - T_EVENT WRPTR on ch 4 (sbuf) => leave; caller consumes the ring */
static void pump(void)
{
	u32 ch, type, flag, value;

	while (recv(&ch, &type, &flag, &value)) {
		if (ch == CH_DATA && type == T_OPEN && flag == OPEN_MAGIC)
			sblk7_open = 1;
		else if (ch == CH_EVENT && type == T_OPEN && flag == OPEN_MAGIC)
			sblk8_open = 1;
		else if (ch == CH_DATA && type == T_CMD
			 && flag == CMD_SBLOCK_INIT) {
			send(CH_DATA, T_DONE, DONE_SBLOCK_INIT, SBLK7_OFF + SMEM);
			sblk7_init = 1;
		} else if (ch == CH_EVENT && type == T_CMD
			   && flag == CMD_SBLOCK_INIT) {
			send(CH_EVENT, T_DONE, DONE_SBLOCK_INIT, SBLK8_OFF + SMEM);
			sblk8_init = 1;
		} else if (ch == CH_LOG && type == T_CMD
			   && flag == CMD_SBUF_INIT) {
			send(CH_LOG, T_DONE, DONE_SBUF_INIT, SMEM + SLOG_OFF);
			slog_init = 1;
		} else if (ch == CH_LOG && type == T_EVENT
			   && flag == EVT_WRPTR) {
			slog_drain();
		}
		/* every other msg (sbuf WRPTR acks, asserts) is harmless to drop:
		 * sbuf_read_bytes watches the ring pointers directly */
	}
}

static int sbuf_read_bytes(u32 r, u8 *out, u32 want, int tries)
{
	volatile u32 *h = smem + RINGHDR(r);
	volatile u8 *rx = ringbuf(h[RXADDR]);
	int t;

	for (t = 0; t < tries; t++) {
		pump();
		if (h[RXWR] - h[RXRD] >= want) {
			u32 rd = h[RXRD], i;

			for (i = 0; i < want; i++, rd++)
				out[i] = rx[rd & (RINGSIZE - 1)];
			dmb();
			h[RXRD] = rd;
			send(CH_PIPE, T_EVENT, EVT_RDPTR, r);
			return 0;
		}
		msleep(10);
	}
	say("cp2: sbuf ring ");
	dec(r);
	say(" read timed out: have ");
	dec(h[RXWR] - h[RXRD]);
	say(" want ");
	dec(want);
	say("\n");
	return -1;
}

/* a sprdwl command: msg header (type|len<<16), cmd tag, seq, then payload.
 * The frame's sprdwl.ko clears seq_no in cmd_send, so we do too. Returns 0
 * if the response's cmd_id matches and status is 0 */
static int wifi_cmd(u8 id, const u8 *payload, u32 plen, u8 *resp, u32 *rlen)
{
	u8 msg[512];
	u32 len = CMD_HDR_SZ + plen;
	u32 msg_hdr = MSG_CMD | (len << 16);
	u32 cmd_tag = (CMD_TYPE_SET << 12) | (CMD_DIR_AP2CP << 11)
		    | (CMD_LEVEL_NORM << 8) | id;

	if (len > sizeof(msg)) {
		say("cp2: wifi_cmd payload too large\n");
		return -1;
	}
	msg[0] = msg_hdr; msg[1] = msg_hdr >> 8;
	msg[2] = msg_hdr >> 16; msg[3] = msg_hdr >> 24;
	msg[4] = cmd_tag; msg[5] = cmd_tag >> 8;
	msg[6] = 0; msg[7] = 0;		/* seq_no */
	if (plen)
		memcpy(msg + CMD_HDR_SZ, payload, plen);

	say("cp2: wifi_cmd id=");
	dec(id);
	say(" plen=");
	dec(plen);
	say("\n");
	sbuf_write_bytes(WL_SBUF_RING, msg, len);

	/* response header is 12 bytes: msg_hdr(4) + cmd_tag(2) + seq(2) +
	 * status(2) + resv(2). Read it first to learn the full length */
	if (sbuf_read_bytes(WL_SBUF_RING, msg, RSP_HDR_SZ, 300))
		return -1;
	hexdump("resp hdr:", msg, RSP_HDR_SZ);
	{
		u32 h = msg[0] | msg[1] << 8 | msg[2] << 16 | (u32)msg[3] << 24;
		u32 total = (h >> 16) & 0xffff;
		u32 type = h & 0xffff;
		u32 ctag = msg[4] | msg[5] << 8;
		u32 status = msg[8] | msg[9] << 8;
		u8 cid = ctag & 0xff;

		if (type != MSG_CMD_RSP) {
			say("cp2: resp msg_type ");
			dec(type);
			say(" (expected CMD_RSP=1)\n");
			return -1;
		}
		if (cid != id) {
			say("cp2: resp wrong id ");
			dec(cid);
			say("\n");
			return -1;
		}
		if (status) {
			say("cp2: resp status=");
			dec(status);
			say(" (0xfd=ap-zc-cp-not, 0xfe=cp-zc-ap-not)\n");
		}
		if (total > RSP_HDR_SZ) {
			u32 extra = total - RSP_HDR_SZ;

			if (extra > sizeof(msg) - RSP_HDR_SZ)
				extra = sizeof(msg) - RSP_HDR_SZ;
			if (sbuf_read_bytes(WL_SBUF_RING, msg + RSP_HDR_SZ,
					    extra, 100))
				return -1;
			if (resp && rlen) {
				u32 n = *rlen < extra ? *rlen : extra;

				memcpy(resp, msg + RSP_HDR_SZ, n);
				*rlen = n;
			}
		} else if (rlen) {
			*rlen = 0;
		}
		return status ? (int)status : 0;
	}
}

static void rings(void)
{
	u32 r;

	for (r = 0; r < RINGS; r++) {
		volatile u32 *h = smem + RINGHDR(r);

		say("cp2: ring ");
		dec(r);
		say(" tx rd/wr ");
		dec(h[TXRD]);
		say("/");
		dec(h[TXWR]);
		say(" rx rd/wr ");
		dec(h[RXRD]);
		say("/");
		dec(h[RXWR]);
		say("\n");
	}
}

/* hello: the sequence sbuf_thread + spipe + wcnd's AT interface would run.
 * Each step is printed, so a log that stops says where cp2 stopped */
static int hello(int secs)
{
	u32 ch, type, flag, value, r;
	int t, poll = 0, opened = 0, inited = 0, asked = 0;

	while (recv(&ch, &type, &flag, &value))
		if (ch == CH_PIPE && type == T_OPEN && flag == OPEN_MAGIC)
			opened = 1;
	sbuf_layout();
	if (send(CH_PIPE, T_OPEN, OPEN_MAGIC, 0))
		return 1;
	for (t = 0; t < secs * 100; t++) {
		while (recv(&ch, &type, &flag, &value)) {
			if (ch != CH_PIPE)
				continue;
			if (type == T_OPEN && flag == OPEN_MAGIC) {
				opened = 1;
				say("cp2: channel 4 open\n");
			} else if (type == T_CMD && flag == CMD_SBUF_INIT) {
				send(CH_PIPE, T_DONE, DONE_SBUF_INIT, SMEM);
				inited = 1;
				say("cp2: sbuf init answered\n");
			} else if (type == T_EVENT && flag == EVT_WRPTR &&
				   value < RINGS) {
				sbuf_drain(value);
			}
		}
		/* ring 0 at once, ring 5 three seconds later, then the
		 * pointers three seconds after that */
		if (inited && asked < 900)
			asked++;
		if (asked == 1)
			sbuf_write(0, AT_CMD);
		else if (asked == 300)
			sbuf_write(5, AT_CMD);
		else if (asked == 600)
			rings();
		/* in case an event got lost: cp2 also moves the pointers */
		if (inited && ++poll == 50) {
			for (r = 0; r < RINGS; r++)
				sbuf_drain(r);
			poll = 0;
		}
		msleep(10);
	}
	say(opened ? "cp2: summary: channel opened" :
		     "cp2: summary: no OPEN from cp2");
	say(inited ? ", sbuf init seen" : ", no sbuf init");
	say(asked ? ", AT sent\n" : "\n");
	return !(opened && inited);
}

static int watch(int secs)
{
	u32 ch, type, flag, value;
	int t;

	for (t = 0; t < secs * 100; t++) {
		while (recv(&ch, &type, &flag, &value))
			;
		msleep(10);
	}
	return 0;
}

static void sdec(int v)			/* signed decimal */
{
	if (v < 0) {
		say("-");
		v = -v;
	}
	dec((u32)v);
}

static u32 le16(const u8 *p)
{
	return p[0] | p[1] << 8;
}

/* --- WIFI_CMD_DEV_OPEN and WIFI_CMD_SCAN from user space, before any kernel driver (docs/wcn-protocol.md) ---
 *
 * open: wait for the sbuf layout to be in smem (hello already does that;
 * we read smem[0] = RINGS as a sanity check), then open sblock 7 and 8 and
 * respond to their SBLOCK_INIT CMDs with the pools we laid out. Then build
 * the DEV_OPEN payload: mode|FW_ZEROCOPY, mac[6], struct wifi_nvm_data[256],
 * ap_timestamp[8]. 271 bytes of payload + 8 cmd header = 279 bytes, which
 * matches the frame's sprdwl.ko exactly */
static u8 nvm[NVM_SIZE];
static u8 dev_open_mode = DEV_MODE_STA | FW_ZEROCOPY;
static const char *nvm_path = "/lib/firmware/wifi-nvm.bin";
/* scan's 5th argument "eng" skips at+cp2_enter_user=1, to compare */
static int enter_user = 1;

static int read_nvm(const char *path)
{
	long fd, n;

	fd = sys_open(path, O_RDONLY);
	if (fd < 0) {
		say("cp2: cannot open ");
		say(path);
		say("\n");
		return -1;
	}
	n = sys_read(fd, nvm, NVM_SIZE);
	sys_close(fd);
	if (n != NVM_SIZE) {
		say("cp2: nvm wrong size\n");
		return -1;
	}
	say("cp2: nvm loaded (256 bytes)\n");
	return 0;
}

static int dev_open(const u8 mac[6])
{
	u8 payload[1 + 6 + NVM_SIZE + 8];
	u8 resp[32];
	u32 rlen = sizeof(resp);
	int ret;

	payload[0] = dev_open_mode;
	memcpy(payload + 1, mac, 6);
	memcpy(payload + 7, nvm, NVM_SIZE);
	memset(payload + 7 + NVM_SIZE, 0, 8);		/* ap_timestamp */

	say("cp2: DEV_OPEN mode=sta|zc mac=");
	hexbyte(mac[0]); say(":"); hexbyte(mac[1]); say(":"); hexbyte(mac[2]);
	say(":"); hexbyte(mac[3]); say(":"); hexbyte(mac[4]); say(":"); hexbyte(mac[5]);
	say("\n");

	ret = wifi_cmd(WIFI_CMD_DEV_OPEN, payload, sizeof(payload),
		       resp, &rlen);
	if (ret == 0)
		say("cp2: DEV_OPEN ok\n");
	return ret;
}

static int wifi_scan(void)
{
	int ret = wifi_cmd(WIFI_CMD_SCAN, 0, 0, 0, 0);

	if (ret == 0)
		say("cp2: SCAN issued; waiting for channel-8 scandone\n");
	return ret;
}

/* Parse one scandone event from channel 8. The event payload is after the
 * sprdwl event header (10 bytes). Format, from sprdwl_event_scan_results:
 * repeated records of {u16 ch_len; u8 ch_num; u16 rssi_len; s16 rssi;
 * u16 mgmt_len; u8 mgmt[mgmt_len]} -- the mgmt bytes are the probe
 * response, from which we show bssid, channel and rssi */
static void parse_scandone(const u8 *buf, u32 len)
{
	u32 pos = EVT_HDR_SZ, n = 0;

	say("cp2: scandone, event bytes=");
	dec(len);
	say("\n");
	while (pos + 10 <= len) {
		u32 ch_len = le16(buf + pos);
		u16 ch_num;
		u32 rssi_len;
		short rssi;
		u32 mgmt_len;
		const u8 *mgmt;

		pos += 2;
		if (ch_len != 1 && ch_len != 2)
			break;
		ch_num = ch_len == 1 ? buf[pos] : le16(buf + pos);
		pos += ch_len;
		rssi_len = le16(buf + pos);
		pos += 2;
		rssi = (short)le16(buf + pos);
		pos += rssi_len;
		mgmt_len = le16(buf + pos);
		pos += 2;
		if (pos + mgmt_len > len)
			break;
		mgmt = buf + pos;
		pos += mgmt_len;
		if (mgmt_len >= 16) {
			/* bssid is at offset 10 of the 802.11 header */
			say("cp2:   bssid ");
			hexbyte(mgmt[10]); say(":"); hexbyte(mgmt[11]); say(":");
			hexbyte(mgmt[12]); say(":"); hexbyte(mgmt[13]); say(":");
			hexbyte(mgmt[14]); say(":"); hexbyte(mgmt[15]);
			say(" ch=");
			dec(ch_num);
			say(" rssi=");
			sdec(rssi);
			/* the IEs start after the 24-byte header and the probe
			 * response's timestamp (8), interval (2), capability (2);
			 * the SSID is IE 0, normally first */
			if (mgmt_len > 38 && mgmt[36] == 0 &&
			    38 + mgmt[37] <= mgmt_len) {
				char c[2];
				u32 k;

				say(" ssid=\"");
				c[1] = 0;
				for (k = 0; k < mgmt[37]; k++) {
					u8 b = mgmt[38 + k];

					c[0] = b >= ' ' && b < 127 ? b : '.';
					say(c);
				}
				say("\"");
			}
			say("\n");
			if (!n)
				hexdump("first record's head:", mgmt - 10,
					mgmt_len + 10 < 48 ? mgmt_len + 10 : 48);
			n++;
		}
	}
	say("cp2: scandone, ");
	dec(n);
	say(" BSSes\n");
}

/* --- bringing Wi-Fi up, then scan or connect (docs/wifi.md wifi4/wifi5) ---
 *
 * wifi_up is everything up to and including DEV_OPEN, and assumes cp2 is
 * running and hello has laid out sbuf:
 * - cp2's own log on channel 5, switched on with at+armlog=1;
 * - user mode, as the frame's wcnd does on a user build;
 * - the sblock 7 (data) and 8 (events) pools. cp2 sends its OPEN and CMD
 *   SBLOCK_INIT only once a command arrives on ring 7, and pump() answers
 *   them while DEV_OPEN waits for its reply;
 * - DEV_OPEN with the calibration block, in dev_open_mode (0x81 =
 *   station | zero-copy). */
static u8 evbuf[BLK8_SIZE];

/* enum wlan_sipc_event_id, sipc.h */
static const char *const evname[] = {
	"connect", "disconnect", "scandone", "tx_busy", "ready", "softap",
	"mgmt_deauth", "mgmt_disassoc", "roc_expired", "report_frame",
	"mlme_tx_status",
};
static u8 mac[6] = { 0x40, 0x45, 0xda, 0x33, 0x19, 0x35 };	/* wifimac.txt */

static void settle(int ticks)
{
	int t;

	for (t = 0; t < ticks; t++) {
		pump();
		msleep(10);
	}
}

static int wifi_up(void)
{
	if (smem[0] != RINGS) {
		say("cp2: sbuf layout not in smem; run 'cp2 hello' first\n");
		return -1;
	}
	if (read_nvm(nvm_path))
		return -1;

	slog_layout();
	send(CH_LOG, T_OPEN, OPEN_MAGIC, 0);
	settle(100);
	say(slog_init ? "cp2: log channel 5 up\n" :
			"cp2: log channel 5 never asked for SBUF_INIT\n");
	if (slog_init) {
		sbuf_write(5, "at+armlog=1\r");
		settle(100);
		sbuf_drain(5);
	}
	if (enter_user) {
		sbuf_write(5, "at+cp2_enter_user=1\r");
		settle(100);
		sbuf_drain(5);
	}
	slog_drain();

	say("cp2: laying out sblock pools\n");
	sblock_layout(SBLK7_OFF + SMEM, BLK7_TXN, BLK7_SIZE, BLK7_RXN, BLK7_SIZE);
	sblock_layout(SBLK8_OFF + SMEM, BLK8_N,   BLK8_SIZE, BLK8_N,   BLK8_SIZE);
	send(CH_DATA,  T_OPEN, OPEN_MAGIC, 0);
	send(CH_EVENT, T_OPEN, OPEN_MAGIC, 0);
	settle(100);

	if (dev_open(mac))
		return -1;
	say("cp2: sblock state: 7 open=");
	dec(sblk7_open);
	say(" init=");
	dec(sblk7_init);
	say("; 8 open=");
	dec(sblk8_open);
	say(" init=");
	dec(sblk8_init);
	say("\n");
	return 0;
}

static u32 rx_frames, rx_shown;

/* one data block from sblock 7. Zero-copy (main.c sprdwl_rx_handler): the
 * block's first byte is an offset, the frame's header struct starts at
 * 2 + offset -- is_encrypted, 13 reserved bytes, then the Ethernet header */
static void data_frame(u32 blen)
{
	u8 head[64];
	volatile u8 *src = ringbuf(last_blk_addr);
	u32 off, i, n;

	n = blen < sizeof(head) ? blen : sizeof(head);
	for (i = 0; i < n; i++)
		head[i] = src[i];
	rx_frames++;
	if (rx_frames <= 2)
		hexdump("rx block head:", head, n);
	off = 2 + head[0];
	if (rx_shown >= 8 || off + 14 + 14 > n)
		return;
	rx_shown++;
	say("cp2: rx frame ");
	dec(blen);
	say(" bytes, enc=");
	dec(head[off]);
	say(" dst=");
	for (i = 0; i < 6; i++) {
		hexbyte(head[off + 14 + i]);
		say(i < 5 ? ":" : "");
	}
	say(" src=");
	for (i = 0; i < 6; i++) {
		hexbyte(head[off + 20 + i]);
		say(i < 5 ? ":" : "");
	}
	say(" type=");
	hexbyte(head[off + 26]);
	hexbyte(head[off + 27]);
	say("\n");
}

/* pump smsg, drain the log, and take every event (sblock 8) and data frame
 * (sblock 7) cp2 has queued, for up to `ticks` x 10 ms or until event
 * `want` (-1: none) arrives. Returns 1 if it did */
static int poll_events(int ticks, int want)
{
	volatile u32 *h8 = smemw(SBLK8_OFF + SMEM);
	volatile u32 *h7 = smemw(SBLK7_OFF + SMEM);
	int t, got = 0;

	for (t = 0; t < ticks && !got; t++) {
		pump();
		if ((t & 63) == 0)
			slog_drain();	/* in case a WRPTR event was missed */
		while (h8[SB_RXWR] != h8[SB_RXRD]) {
			int blen = sblock_recv(CH_EVENT, SBLK8_OFF + SMEM,
					       evbuf, sizeof(evbuf));
			if (blen > 0) {
				u32 ev_id = le16(evbuf + 4) & 0xfff;

				say("cp2: ch8 event id=");
				dec(ev_id);
				say(ev_id < 11 ? " (" : "");
				say(ev_id < 11 ? evname[ev_id] : "");
				say(ev_id < 11 ? ")\n" : "\n");
				if (ev_id == WIFI_EV_SCANDONE)
					parse_scandone(evbuf, blen);
				else
					hexdump("event:", evbuf,
						blen < 64 ? blen : 64);
				if ((int)ev_id == want)
					got = 1;
			}
			sblock_release(CH_EVENT, SBLK8_OFF + SMEM);
		}
		while (h7[SB_RXWR] != h7[SB_RXRD]) {
			int blen = sblock_recv(CH_DATA, SBLK7_OFF + SMEM,
					       evbuf, 0);
			if (blen > 0)
				data_frame(blen);
			sblock_release(CH_DATA, SBLK7_OFF + SMEM);
		}
		msleep(10);
	}
	slog_drain();
	return got;
}

static int scan_step(int secs)
{
	if (wifi_up() || wifi_scan())
		return -1;
	return poll_events(secs * 100, WIFI_EV_SCANDONE) ? 0 : -1;
}

/* connect: what the frame's sprdwl_cfg80211_connect sends for a WPA2-PSK
 * network, in its order (ids and sizes from its sprdwl.ko): P2P_ENABLE 0,
 * WPA_VERSION 2, AUTH_TYPE open, PAIRWISE and GROUP CCMP, AKM PSK, the
 * 32-byte PMK as WIFI_CMD_PSK (cp2 runs the 4-way handshake itself), the
 * channel and BSSID from a scan, and the ESSID last, which starts the
 * association; no CONNECT command exists. The PMK comes from a file (made
 * on the frame from its wpa_supplicant.conf, never printed) */
#define WIFI_CMD_AUTH_TYPE	2
#define WIFI_CMD_WPA_VERSION	3
#define WIFI_CMD_PAIRWISE	4
#define WIFI_CMD_GROUP		5
#define WIFI_CMD_AKM		6
#define WIFI_CMD_CHANNEL	7
#define WIFI_CMD_BSSID		8
#define WIFI_CMD_ESSID		9
#define WIFI_CMD_DISCONNECT	13
#define WIFI_CMD_PSK		27
#define WIFI_CMD_P2P_ENABLE	43
#define CIPHER_CCMP		4
#define AKM_PSK			1

static int cmd_u32(u8 id, u32 v)
{
	u8 b[4];

	b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24;
	return wifi_cmd(id, b, 4, 0, 0);
}

static int cmd_u8(u8 id, u8 v)
{
	return wifi_cmd(id, &v, 1, 0, 0);
}

static int parse_mac(const char *s, u8 *m)
{
	int i;

	for (i = 0; i < 6; i++, s += 3) {
		char x[5];	/* num() reads hex as "0x.." */

		if (!s[0] || !s[1] || (i < 5 && s[2] != ':'))
			return -1;
		x[0] = '0'; x[1] = 'x'; x[2] = s[0]; x[3] = s[1]; x[4] = 0;
		m[i] = num(x);
	}
	return 0;
}

static int connect_step(int secs, const char *ssid, const char *bssid_s,
			u32 chan, const char *pmk_path)
{
	u8 bssid[6], pmk[32];
	long fd, n;
	u32 len;
	int ok;

	for (len = 0; ssid[len]; len++)
		;
	if (len == 0 || len > 32 || parse_mac(bssid_s, bssid)) {
		say("cp2: connect: bad ssid or bssid\n");
		return 2;
	}
	fd = sys_open(pmk_path, O_RDONLY);
	n = fd < 0 ? -1 : sys_read(fd, pmk, sizeof(pmk));
	if (fd >= 0)
		sys_close(fd);
	if (n != 32) {
		say("cp2: connect: no 32-byte PMK in the file\n");
		return 2;
	}
	if (wifi_up())
		return -1;
	if (cmd_u8(WIFI_CMD_P2P_ENABLE, 0) ||
	    cmd_u32(WIFI_CMD_WPA_VERSION, 2) ||
	    cmd_u32(WIFI_CMD_AUTH_TYPE, 0) ||
	    cmd_u32(WIFI_CMD_PAIRWISE, CIPHER_CCMP) ||
	    cmd_u32(WIFI_CMD_GROUP, CIPHER_CCMP) ||
	    cmd_u8(WIFI_CMD_AKM, AKM_PSK) ||
	    wifi_cmd(WIFI_CMD_PSK, pmk, 32, 0, 0) ||
	    cmd_u32(WIFI_CMD_CHANNEL, chan) ||
	    wifi_cmd(WIFI_CMD_BSSID, bssid, 6, 0, 0)) {
		memset(pmk, 0, sizeof(pmk));
		say("cp2: connect: a setup command failed\n");
		return -1;
	}
	memset(pmk, 0, sizeof(pmk));
	if (wifi_cmd(WIFI_CMD_ESSID, (const u8 *)ssid, len, 0, 0)) {
		say("cp2: connect: ESSID failed\n");
		return -1;
	}
	say("cp2: association started; waiting for the connect event\n");
	ok = poll_events(secs * 100, WIFI_EV_CONNECT);
	say(ok ? "cp2: connect event received; watching data for 15 s\n" :
		 "cp2: no connect event\n");
	if (ok)
		poll_events(1500, -1);
	say("cp2: data frames received: ");
	dec(rx_frames);
	say("\n");
	{
		u8 r[2] = { 3, 0 };	/* WLAN_REASON_DEAUTH_LEAVING */

		wifi_cmd(WIFI_CMD_DISCONNECT, r, 2, 0, 0);
		poll_events(200, WIFI_EV_DISCONNECT);
	}
	return ok ? 0 : -1;
}

/* adibits: one PMIC register read-modify-write, the way spi-sprd-adi
 * does it, for trying a rail before a kernel has it. The SC2713S's
 * registers are memory-mapped through the ADI controller at 0x40038000 (its
 * base 0x40030000 + the 10-bit slave offset 0x8000, = the vendor DT's
 * 0x4003881c etc.):
 * - read: the register's physical address into RD_CMD (base + 0x24 on the
 *   SC7731G's r1 controller), then RD_DATA (+ 0x28) until bit 31 (busy)
 *   clears, the value in bits 15:0;
 * - write: a store to the window, queued into the write FIFO, whose status
 *   is at + 0x2c (bit 11 full, bit 10 empty).
 * Only ever bits: these words are shared (0x810/0x814 also hold the eMMC
 * core and I/O rails, bits 8 and 7, which the restart handler power-cycles;
 * 0x81c holds vddsim2 at bit 4, the touch controller's pads), so a constant
 * written there would take the rootfs or the touchscreen with it. The kernel
 * uses the controller too, with no lock between us (sprd-adi has no hardware
 * spinlock on this board), so this is one short read and one write. */
#define ADI		0x40030000
#define ADI_RD_CMD	0x24
#define ADI_RD_DATA	0x28
#define  ADI_RD_BUSY	(1u << 31)
#define ADI_FIFO_STS	0x2c
#define  ADI_FIFO_FULL	(1u << 11)
#define  ADI_FIFO_EMPTY	(1u << 10)
#define ADI_SLAVE	0x40038000

static volatile u32 *adi, *pmic;

static int adi_rd(u32 reg, u32 *v)
{
	u32 d = ADI_RD_BUSY;
	int i;

	R(adi, ADI_RD_CMD) = ADI_SLAVE + reg;
	dmb();
	for (i = 0; i < 1000 && ((d = R(adi, ADI_RD_DATA)) & ADI_RD_BUSY); i++)
		;
	*v = d & 0xffff;
	return (d & ADI_RD_BUSY) ? -1 : 0;
}

static int adi_wr(u32 reg, u32 v)
{
	int i;

	for (i = 0; i < 100000 && (R(adi, ADI_FIFO_STS) & ADI_FIFO_FULL); i++)
		;
	R(pmic, reg) = v;
	dmb();
	for (i = 0; i < 100000 && !(R(adi, ADI_FIFO_STS) & ADI_FIFO_EMPTY); i++)
		;
	return i == 100000 ? -1 : 0;
}

static int adibits(u32 reg, u32 set_bits, u32 clr_bits)
{
	u32 before, after;

	if (reg >= 0x1000 || (reg & 3) || (set_bits & clr_bits)) {
		say("cp2: adibits: bad register or masks\n");
		return 1;
	}
	adi = map(ADI, 0x1000);
	pmic = map(ADI_SLAVE, 0x1000);
	if (adi_rd(reg, &before)) {
		say("cp2: adibits: read timed out, nothing written\n");
		return 1;
	}
	if (!set_bits && !clr_bits) {		/* 0 0: read only */
		say("cp2: pmic ");
		hex(reg);
		say(" ");
		hex(before);
		say("\n");
		return 0;
	}
	if (adi_wr(reg, (before & ~clr_bits) | set_bits) || adi_rd(reg, &after)) {
		say("cp2: adibits: write or readback timed out\n");
		return 1;
	}
	say("cp2: pmic ");
	hex(reg);
	say(" ");
	hex(before);
	say(" -> ");
	hex(after);
	say((after & ~(set_bits | clr_bits)) == (before & ~(set_bits | clr_bits)) ?
	    "\n" : " (OTHER BITS CHANGED)\n");
	return (after & (set_bits | clr_bits)) != set_bits;
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static int main(int argc, char **argv)
{
	int secs = argc > 2 ? (int)num(argv[2]) : 5;

	if (argc < 2) {
		say("usage: cp2 regs|stop|load FILE|start|hello [SECS]|rings|watch [SECS]|scan [SECS [MODE [NVM]]]|connect SECS SSID BSSID CHAN PMKFILE [NVM]|adibits REG SET CLR\n");
		return 2;
	}
	maps();
	if (streq(argv[1], "regs"))
		regs();
	else if (streq(argv[1], "stop"))
		stop();
	else if (streq(argv[1], "load") && argc > 2)
		return load(argv[2]);
	else if (streq(argv[1], "start"))
		start();
	else if (streq(argv[1], "hello"))
		return hello(secs);
	else if (streq(argv[1], "rings"))
		rings();
	else if (streq(argv[1], "watch"))
		return watch(secs);
	else if (streq(argv[1], "adibits") && argc > 4)
		return adibits(num(argv[2]), num(argv[3]), num(argv[4]));
	else if (streq(argv[1], "slogh")) {
		volatile u32 *h = smemw(SMEM + SLOG_OFF);
		int i;

		for (i = 0; i < 9; i++) {
			say("cp2: slog hdr[");
			dec(i);
			say("] = ");
			hex(h[i]);
			say("\n");
		}
	}
	else if (streq(argv[1], "sblk")) {
		static const char * const n[] = { "tx_addr", "tx_cnt",
			"tx_sz", "tx_blks", "tx_rd", "tx_wr", "rx_addr",
			"rx_cnt", "rx_sz", "rx_blks", "rx_rd", "rx_wr" };
		u32 ch;
		for (ch = 0; ch < 2; ch++) {
			u32 base = (ch ? SBLK8_OFF : SBLK7_OFF) + SMEM;
			volatile u32 *h = smemw(base);
			int i;

			say(ch ? "cp2: ch 8 (events) @ " : "cp2: ch 7 (data) @ ");
			hex(base);
			say("\n");
			for (i = 0; i < 12; i++) {
				say("cp2:   ring.");
				say(n[i]);
				say(" = ");
				hex(h[i]);
				say("\n");
			}
			for (i = 0; i < 12; i++) {
				say("cp2:   pool.");
				say(n[i]);
				say(" = ");
				hex(h[SB_POOL + i]);
				say("\n");
			}
		}
	}
	else if (streq(argv[1], "connect") && argc > 6) {
		if (argc > 7)
			nvm_path = argv[7];
		return connect_step(secs, argv[3], argv[4], num(argv[5]), argv[6]);
	}
	else if (streq(argv[1], "scan")) {
		if (argc > 3)
			dev_open_mode = (u8)num(argv[3]);
		if (argc > 4)
			nvm_path = argv[4];
		if (argc > 5 && streq(argv[5], "eng"))
			enter_user = 0;
		return scan_step(secs);
	}
	else {
		say("cp2: unknown command\n");
		return 2;
	}
	return 0;
}

/* the kernel's initial stack: argc, then argv[] */
void __attribute__((naked, noreturn)) _start(void)
{
	asm volatile("ldr r0, [sp]\n\t"
		     "add r1, sp, #4\n\t"
		     "bl start_c");
}

void __attribute__((noreturn, used)) start_c(int argc, char **argv)
{
	sys_exit(main(argc, argv));
	for (;;)
		;
}
