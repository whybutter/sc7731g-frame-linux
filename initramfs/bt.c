// SPDX-License-Identifier: GPL-2.0-only
/*
 * bt -- the SC7731G's built-in Bluetooth from user space: HCI to cp2 (the
 * WCN co-processor that also does the Wi-Fi) over SIPC, with nothing but
 * /dev/mem, on top of cp2.c's loader and SIPC helpers.
 *
 *   bt up PSKEY [SECS]   after `cp2 stop; cp2 load; cp2 start; cp2 hello`:
 *                        cp2's log (channel 5) and user mode as wifi_up does
 *                        them, then the PSKEY block, HCI_Reset, the local
 *                        version and BD_ADDR, and an LE scan for SECS seconds
 *                        printing each advertiser once (address, RSSI, name)
 *   bt hci HEX...        after `bt up`: one HCI command (opcode as 4 hex
 *                        digits, then parameter bytes), and its events
 *
 * What the vendor does, from the frame's own Android (docs/bluetooth.md):
 * /dev/sttybt0 is the stty4bt driver over sbuf channel 4 ring 10 (vendor DT
 * stty4bt_cpwcn, bufid 0x0a), raw bytes both ways, H4 framing (a packet type
 * byte, then the HCI packet). libbt-vendor.so opens it and, before the
 * stack's first HCI_Reset, writes the 144-byte sr2351_pskey struct, with no
 * H4 type in front: its first byte, pskey_cmd = 5, says what it is. cp2
 * answers with a byte 5. tools/bt-pskey.py builds that block offline from
 * connectivity_configure.ini, as tools/wcn-nvm.py does for the Wi-Fi.
 *
 * Built like cp2 (freestanding, no libc): this file includes cp2.c whole and
 * renames its main, so the two tools share one copy of the SIPC code. cp2's
 * _start branches to the start_c defined here.
 */

#define main cp2_main
#define start_c cp2_start_c
#include "cp2.c"
#undef main
#undef start_c

#define BT_RING		10		/* stty4bt_cpwcn: sbuf 4, bufid 0x0a */
#define PSKEY_SIZE	0x90		/* sizeof(sr2351_pskey), libbt-vendor */
#define PSKEY_CMD	5

/* H4 packet types */
#define H4_CMD		1
#define H4_ACL		2
#define H4_SCO		3
#define H4_EVT		4

#define EV_CMD_COMPLETE	0x0e
#define EV_CMD_STATUS	0x0f
#define EV_LE_META	0x3e
#define LE_ADV_REPORT	0x02

/* --- bytes from ring 10, kept across reads so packets can straddle them --- */

static u8 rxq[4096];
static u32 rxq_n;

/* move whatever cp2 has written to ring 10 into rxq; returns bytes moved */
static u32 bt_pull(void)
{
	volatile u32 *h = smem + RINGHDR(BT_RING);
	volatile u8 *rx = ringbuf(h[RXADDR]);
	u32 rd = h[RXRD], wr = h[RXWR], n = 0;

	pump();
	while (rd != wr && rxq_n < sizeof(rxq)) {
		rxq[rxq_n++] = rx[rd & (RINGSIZE - 1)];
		rd++;
		n++;
	}
	if (n) {
		dmb();
		h[RXRD] = rd;
		send(CH_PIPE, T_EVENT, EVT_RDPTR, BT_RING);
	}
	return n;
}

static void rxq_drop(u32 n)
{
	u32 i;

	for (i = n; i < rxq_n; i++)
		rxq[i - n] = rxq[i];
	rxq_n -= n;
}

/* how long the H4 packet at the head of rxq is: 0 while its header is still
 * incomplete; a byte that starts no H4 packet is dropped with everything
 * queued after it (cp2 never sent one in bt1, but a bridge must not wedge) */
static u32 h4_need(void)
{
	if (!rxq_n)
		return 0;
	switch (rxq[0]) {
	case H4_EVT:
		return rxq_n >= 3 ? 3 + rxq[2] : 0;
	case H4_ACL:
		return rxq_n >= 5 ? 5 + (rxq[3] | rxq[4] << 8) : 0;
	case H4_SCO:
		return rxq_n >= 4 ? 4 + rxq[3] : 0;
	}
	hexdump("bt: not H4, dropping:", rxq, rxq_n);
	rxq_n = 0;
	return 0;
}

/* one whole H4 packet into pkt (type byte first), waiting up to ticks*10 ms;
 * returns its length, 0 on timeout. ticks 0 looks once and never sleeps */
static u32 bt_packet(u8 *pkt, u32 max, int ticks)
{
	int t;

	for (t = 0; t <= ticks; t++) {
		u32 need;

		bt_pull();
		need = h4_need();
		if (need && rxq_n >= need) {
			u32 n = need < max ? need : max;

			memcpy(pkt, rxq, n);
			rxq_drop(need);
			return n;
		}
		if (t < ticks)
			msleep(10);
	}
	return 0;
}

/* to cp2 on ring 10, waiting for room: the ring is 4 KiB and an ACL packet
 * up to ~1 KiB, and sbuf_write_bytes overwrites whatever cp2 hasn't read */
static int bt_send(const u8 *p, u32 n)
{
	volatile u32 *h = smem + RINGHDR(BT_RING);
	int t;

	for (t = 0; RINGSIZE - (h[TXWR] - h[TXRD]) < n; t++) {
		if (t == 1000)
			return -1;		/* cp2 stopped reading for 1 s */
		pump();
		msleep(1);
	}
	sbuf_write_bytes(BT_RING, p, n);
	return 0;
}

/* --- pskey --- */

static int pskey(const char *path)
{
	u8 blk[PSKEY_SIZE], ack[16];
	long fd, n;
	int t;

	fd = sys_open(path, O_RDONLY);
	if (fd < 0) {
		say("bt: cannot open ");
		say(path);
		say("\n");
		return -1;
	}
	n = sys_read(fd, blk, sizeof(blk));
	sys_close(fd);
	if (n != PSKEY_SIZE || blk[0] != PSKEY_CMD) {
		say("bt: pskey file is not a 144-byte block starting with 5\n");
		return -1;
	}
	say("bt: pskey, 144 bytes\n");
	bt_send(blk, sizeof(blk));
	/* libbt-vendor waits 5 s and reads up to 10 bytes */
	for (t = 0; t < 500; t++) {
		bt_pull();
		if (rxq_n)
			break;
		msleep(10);
	}
	if (!rxq_n) {
		say("bt: no pskey answer in 5 s\n");
		return -1;
	}
	msleep(50);
	bt_pull();
	n = rxq_n < sizeof(ack) ? rxq_n : sizeof(ack);
	memcpy(ack, rxq, n);
	hexdump("bt: pskey answer:", ack, n);
	/* the answer is 4 bytes, 05 08 08 08 on this firmware; libbt-vendor
	 * reads up to 10 and looks only at the first, so all of it goes */
	rxq_n = 0;
	if (ack[0] != PSKEY_CMD) {
		say("bt: pskey answer is not 5\n");
		return -1;
	}
	say("bt: pskey ok\n");
	return 0;
}

/* --- HCI --- */

static void addr(const u8 *a)		/* BD_ADDR, little-endian on the wire */
{
	int i;

	for (i = 5; i >= 0; i--) {
		hexbyte(a[i]);
		if (i)
			say(":");
	}
}

/* advertisers seen so far, so a scan prints each once */
static u8 seen[64][6];
static u32 nseen;

static void adv_report(const u8 *p, u32 len)
{
	u32 nrep = p[0], pos = 1, r;

	for (r = 0; r < nrep && pos + 9 <= len; r++) {
		u8 evtype = p[pos], atype = p[pos + 1];
		const u8 *a = p + pos + 2;
		u32 dlen = p[pos + 8], i, k;
		const u8 *d = p + pos + 9;
		signed char rssi;

		if (pos + 9 + dlen + 1 > len)
			break;
		rssi = (signed char)d[dlen];
		pos += 9 + dlen + 1;
		for (i = 0; i < nseen; i++) {
			for (k = 0; k < 6 && seen[i][k] == a[k]; k++)
				;
			if (k == 6)
				break;
		}
		if (i < nseen)
			continue;
		if (nseen < 64)
			memcpy(seen[nseen++], a, 6);
		say("bt: adv ");
		addr(a);
		say(atype ? " random" : " public");
		say(" type=");
		dec(evtype);
		say(" rssi=");
		sdec(rssi);
		/* the AD structures: {len, type, data}; 0x08/0x09 are the
		 * shortened/complete local name */
		for (i = 0; i + 1 < dlen && d[i]; i += d[i] + 1) {
			if ((d[i + 1] == 0x08 || d[i + 1] == 0x09) &&
			    i + 1 + d[i] <= dlen) {
				char c[2];

				say(" name=\"");
				c[1] = 0;
				for (k = 2; k <= d[i]; k++) {
					c[0] = d[i + k] >= ' ' && d[i + k] < 127 ?
					       d[i + k] : '.';
					say(c);
				}
				say("\"");
			}
		}
		say("\n");
	}
}

/* print an event; returns the status of a Command Complete/Status for
 * opcode op, -1 for anything else */
static int event(const u8 *pkt, u32 n, u32 op)
{
	u8 code = pkt[1];
	const u8 *p = pkt + 3;
	u32 plen = n - 3;

	if (code == EV_LE_META && plen && p[0] == LE_ADV_REPORT) {
		adv_report(p + 1, plen - 1);
		return -1;
	}
	hexdump("bt: event", pkt, n);
	if (code == EV_CMD_COMPLETE && plen >= 4 &&
	    (u32)(p[1] | p[2] << 8) == op)
		return p[3];
	if (code == EV_CMD_STATUS && plen >= 4 &&
	    (u32)(p[2] | p[3] << 8) == op)
		return p[0];
	return -1;
}

static u8 last[260];
static u32 last_n;

/* one HCI command, and its Command Complete/Status within 2 s; the return
 * parameters stay in last[] */
static int hci(u32 op, const u8 *par, u32 plen)
{
	u8 cmd[260];
	int t, st;

	cmd[0] = H4_CMD;
	cmd[1] = op;
	cmd[2] = op >> 8;
	cmd[3] = plen;
	if (plen)
		memcpy(cmd + 4, par, plen);
	hexdump("bt: cmd", cmd, 4 + plen);
	bt_send(cmd, 4 + plen);
	for (t = 0; t < 20; t++) {
		last_n = bt_packet(last, sizeof(last), 10);
		if (!last_n)
			continue;
		if (last[0] != H4_EVT) {
			hexdump("bt: acl", last, last_n);
			continue;
		}
		st = event(last, last_n, op);
		if (st >= 0) {
			if (st) {
				say("bt: status ");
				hex(st);
				say("\n");
			}
			return st;
		}
	}
	say("bt: no answer to ");
	hex(op);
	say("\n");
	return -1;
}

/* channel 5 and user mode: the first half of cp2.c's wifi_up. The log is
 * only for tests: armlog=1 fills channel 5 with cp2's whole trace, which
 * the bridge would have to drain for as long as it runs */
static void wcn_common(int armlog)
{
	if (armlog) {
		slog_layout();
		send(CH_LOG, T_OPEN, OPEN_MAGIC, 0);
		settle(100);
	}
	if (slog_init) {
		sbuf_write(5, "at+armlog=1\r");
		settle(100);
		sbuf_drain(5);
	}
	sbuf_write(5, "at+cp2_enter_user=1\r");
	settle(100);
	sbuf_drain(5);
	slog_drain();
}

static int bt_up(const char *path, int secs)
{
	/* LE scan: active, 10 ms interval and window, public own address,
	 * no filter -- the scale and the phones answer scan requests with
	 * their names */
	static const u8 evmask[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0x1f, 0x00, 0x20 };
	static const u8 scanpar[7] = { 0x01, 0x10, 0x00, 0x10, 0x00, 0x00, 0x00 };
	static const u8 scan_on[2] = { 0x01, 0x00 };
	static const u8 scan_off[2] = { 0x00, 0x00 };
	u8 pkt[260];
	int t;

	if (smem[0] != RINGS) {
		say("bt: sbuf layout not in smem; run 'cp2 hello' first\n");
		return 1;
	}
	wcn_common(1);
	if (pskey(path)) {
		slog_drain();
		return 1;
	}
	if (hci(0x0c03, 0, 0)) {			/* HCI_Reset */
		slog_drain();
		return 1;
	}
	if (!hci(0x1001, 0, 0) && last_n >= 15) {	/* Read_Local_Version */
		const u8 *r = last + 7;

		say("bt: hci version ");
		dec(r[0]);
		say(" rev ");
		hex(r[1] | r[2] << 8);
		say(" lmp ");
		dec(r[3]);
		say(" manufacturer ");
		hex(r[4] | r[5] << 8);
		say(" subver ");
		hex(r[6] | r[7] << 8);
		say("\n");
	}
	if (!hci(0x1009, 0, 0) && last_n >= 13) {	/* Read_BD_ADDR */
		say("bt: bdaddr ");
		addr(last + 7);
		say("\n");
	}
	hci(0x1003, 0, 0);				/* Read_Local_Supported_Features */
	hci(0x0c01, evmask, 8);				/* Set_Event_Mask, + LE meta */
	hci(0x2002, 0, 0);				/* LE_Read_Buffer_Size */
	if (hci(0x200b, scanpar, 7) || hci(0x200c, scan_on, 2)) {
		slog_drain();
		return 1;
	}
	say("bt: scanning\n");
	for (t = 0; t < secs * 10; t++) {
		u32 n = bt_packet(pkt, sizeof(pkt), 10);

		if (n && pkt[0] == H4_EVT)
			event(pkt, n, 0);
		if ((t & 31) == 0)
			slog_drain();
	}
	hci(0x200c, scan_off, 2);
	say("bt: ");
	dec(nseen);
	say(" advertisers\n");
	slog_drain();
	return nseen ? 0 : 1;
}

/* --- the bridge: ring 10 <-> /dev/vhci, so the kernel has an hci0 ---
 *
 * hci_vhci registers an HCI device for whoever opens /dev/vhci and writes
 * the vendor packet ff 00 (00: a primary controller, no quirks); it answers
 * ff 00 <index, le16>. From then on each read() is one H4 packet the stack
 * sends (commands, ACL), and each write() must be one whole H4 packet from
 * the controller. Closing the file unregisters hci0, so the bridge is the
 * adapter's lifetime: the service restarts it, and with it cp2.
 *
 * Without a kernel SIPC driver nothing takes cp2's interrupt (SPI 73), so
 * the bridge polls: poll() on vhci with a 2 ms timeout, and ring 10 and the
 * smsg ring on every pass. A packet from cp2 waits at most ~2 ms for the
 * stack, one from the stack none (poll wakes on it). */

#define O_NONBLOCK	04000
#define POLLIN		1
#define sys_poll(f, n, ms)	sys(168, (long)(f), n, ms, 0, 0, 0)
#define sys_dup2(a, b)		sys(63, a, b, 0, 0, 0, 0)

struct pollfd {
	int fd;
	short events, revents;
};

/* the bridge's own log, on stderr: cp2.c's say() is stdout, which the
 * bridge points at /dev/null once it runs (every smsg it sends and gets is
 * a line there) */
static void err(const char *s)
{
	int n = 0;

	while (s[n])
		n++;
	sys_write(2, s, n);
}

/* cp2's watchdog has fired (SPI 85 pending): it is dead and won't answer */
static int cp2_dead(void)
{
	static volatile u32 *gicd;

	if (!gicd)
		gicd = map(0x12001000, 0x1000);
	return (R(gicd, 0x200 + 3 * 4) >> 21) & 1;
}

static int bridge(const char *fw, const char *pskey_path)
{
	struct pollfd pfd;
	u8 pkt[1100];
	long vfd, n;
	u32 ticks = 0;
	int wdt0;
	int t;

	/* cp2 from scratch, as the tests do: whatever ran before (a test, a
	 * previous bridge) leaves it in a state we don't know */
	stop();
	if (load(fw)) {
		err("btbridge: firmware did not load\n");
		return 1;
	}
	start();
	msleep(2000);
	if (hello(3)) {
		err("btbridge: cp2 never opened sbuf channel 4\n");
		return 1;
	}
	wcn_common(0);
	if (pskey(pskey_path)) {
		err("btbridge: cp2 did not take the PSKEY\n");
		return 1;
	}

	vfd = sys_open("/dev/vhci", O_RDWR | O_NONBLOCK);
	if (vfd < 0) {
		err("btbridge: cannot open /dev/vhci (CONFIG_BT_HCIVHCI)\n");
		return 1;
	}
	pkt[0] = 0xff;
	pkt[1] = 0x00;
	if (sys_write(vfd, pkt, 2) != 2) {
		err("btbridge: vhci refused the create packet\n");
		return 1;
	}
	for (t = 0; t < 100 && (n = sys_read(vfd, pkt, sizeof(pkt))) < 0; t++)
		msleep(10);
	if (n != 4 || pkt[0] != 0xff) {
		err("btbridge: no answer from vhci\n");
		return 1;
	}
	/* say() still reaches the log here: the index in decimal */
	say("btbridge: up, hci");
	dec(pkt[2] | pkt[3] << 8);
	say("\n");
	/* SPI 85 as it is now: only a change to pending means cp2 died (no
	 * driver owns the line, so an old one could still be latched) */
	wdt0 = cp2_dead();

	/* from here on cp2.c's chatter goes nowhere */
	n = sys_open("/dev/null", O_RDWR);
	if (n >= 0)
		sys_dup2(n, 1);

	for (;;) {
		pfd.fd = vfd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		sys_poll(&pfd, 1, 2);
		/* the stack to cp2: drain everything queued */
		while ((n = sys_read(vfd, pkt, sizeof(pkt))) > 0) {
			if (pkt[0] == 0xff)
				continue;		/* vhci's own, none expected */
			if (bt_send(pkt, n)) {
				err("btbridge: cp2 stopped reading ring 10\n");
				return 1;
			}
		}
		/* cp2 to the stack: every whole packet in the ring */
		while ((n = bt_packet(pkt, sizeof(pkt), 0)) > 0) {
			if (sys_write(vfd, pkt, n) != n)
				err("btbridge: vhci refused a packet\n");
		}
		if (++ticks == 500) {			/* about a second */
			ticks = 0;
			if (!wdt0 && cp2_dead()) {
				err("btbridge: cp2's watchdog fired\n");
				return 1;
			}
		}
	}
}

static u32 hexnum(const char *s)
{
	u32 v = 0;

	for (; *s; s++) {
		int d = *s >= '0' && *s <= '9' ? *s - '0' :
			*s >= 'a' && *s <= 'f' ? *s - 'a' + 10 :
			*s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
		if (d < 0)
			break;
		v = v << 4 | d;
	}
	return v;
}

static int bt_main(int argc, char **argv)
{
	if (argc > 2 && streq(argv[1], "up")) {
		maps();
		return bt_up(argv[2], argc > 3 ? (int)num(argv[3]) : 10);
	}
	if (argc > 2 && streq(argv[1], "hci")) {
		u8 par[255];
		int i;

		maps();
		for (i = 3; i < argc && i - 3 < 255; i++)
			par[i - 3] = hexnum(argv[i]);
		return hci(hexnum(argv[2]), par, argc - 3) != 0;
	}
	if (argc > 2 && streq(argv[1], "listen")) {
		u8 pkt[260];
		int t, secs = (int)num(argv[2]);

		maps();
		for (t = 0; t < secs * 10; t++) {
			u32 n = bt_packet(pkt, sizeof(pkt), 10);

			if (n && pkt[0] == H4_EVT)
				event(pkt, n, 0);
			else if (n)
				hexdump("bt: acl", pkt, n);
		}
		return 0;
	}
	if (argc > 3 && streq(argv[1], "bridge")) {
		maps();
		return bridge(argv[2], argv[3]);
	}
	if (argc > 1 && streq(argv[1], "stop")) {
		maps();
		stop();
		return 0;
	}
	say("usage: bt up PSKEY [SECS] | bt hci OPCODE [BYTE...] | bt listen SECS (hex)\n"
	    "       bt bridge FIRMWARE PSKEY | bt stop\n");
	return 2;
}

void __attribute__((noreturn, used)) start_c(int argc, char **argv)
{
	sys_exit(bt_main(argc, argv));
	for (;;)
		;
}
