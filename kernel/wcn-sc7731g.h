/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The SC7731G's WCN co-processor ("cp2": Wi-Fi, BT, FM on one ARM core with
 * Spreadtrum's firmware) and its SIPC transport, as one core driver
 * (wcn-sc7731g.c) with the radios on top as its DT children
 * (sprdwl-sc7731g.c for Wi-Fi, bt-sc7731g.c for Bluetooth).
 *
 * The protocol is what docs/wifi.md wifi1-wifi5 proved from user space
 * with initramfs/cp2.c, against this frame's firmware
 * (CP2_WCN_14D_W16.26.3):
 * - smsg: 8-byte messages in two 128-entry rings in cp2's last page;
 * - sbuf: byte-stream rings, channel 4 (13 rings: 5 AT, 7 Wi-Fi
 *   commands, 10 BT HCI) and channel 5 (cp2's own log);
 * - sblock: fixed-size block pools, channel 7 (Wi-Fi data) and 8
 *   (Wi-Fi events).
 *
 * The API is deliberately small:
 * - a client powers cp2 up and down (refcounted);
 * - it writes and reads sbuf rings;
 * - it gets, sends, receives and releases sblocks;
 * - and it registers one notify callback per channel.
 */
#ifndef WCN_SC7731G_H
#define WCN_SC7731G_H

#include <linux/types.h>

struct wcn;
struct device;

/* sbuf channel 4's rings */
#define WCN_RING_AT		5
#define WCN_RING_WIFI		7
#define WCN_RING_BT		10

/* sblock channels */
#define WCN_CH_DATA		7
#define WCN_CH_EVENT		8

/* what a notify callback is told */
enum wcn_notify {
	WCN_SBUF_RX,		/* sbuf: cp2 wrote into the ring's rx side */
	WCN_SBLOCK_RX,		/* sblock: a filled block is waiting */
	WCN_SBLOCK_TX_FREE,	/* sblock: cp2 returned tx blocks to the pool */
	WCN_CP2_DOWN,		/* cp2 stopped, crashed or its watchdog fired */
};

struct wcn_block {
	void __iomem *addr;	/* the block's start in the AP's mapping */
	u32 length;
};

/* from a child device: its parent's wcn */
struct wcn *wcn_get(struct device *child);

/* refcounted: the first up loads the firmware, starts cp2 and opens the
 * channels (it sleeps; process context); the last down stops it */
int wcn_power_up(struct wcn *wcn);
void wcn_power_down(struct wcn *wcn);
/* how many times cp2 has been started: a client that gives cp2 state of
 * its own once per start (BT's PSKEY) compares it with the last one it saw */
u32 wcn_boot_count(struct wcn *wcn);

int wcn_register_notify(struct wcn *wcn, int key,
			void (*cb)(void *data, enum wcn_notify what),
			void *data);
/* keys: the sbuf ring number for channel 4 rings, or 0x100 + the sblock
 * channel. Called from cp2's interrupt thread: no sleeping */
#define WCN_KEY_RING(r)		(r)
#define WCN_KEY_SBLOCK(ch)	(0x100 + (ch))

/* sbuf channel 4: whole messages or nothing; timeout_ms 0 = don't wait */
int wcn_sbuf_write(struct wcn *wcn, int ring, const void *buf, u32 len,
		   int timeout_ms);
int wcn_sbuf_read(struct wcn *wcn, int ring, void *buf, u32 len,
		  int timeout_ms);
u32 wcn_sbuf_avail(struct wcn *wcn, int ring);
u32 wcn_sbuf_tx_consumed(struct wcn *wcn, int ring);

/* sblock: get a free tx block, fill it, send it; receive a filled rx
 * block, use it, release it. Non-blocking: -EAGAIN when there is none */
int wcn_sblock_get(struct wcn *wcn, int ch, struct wcn_block *blk);
int wcn_sblock_send(struct wcn *wcn, int ch, struct wcn_block *blk);
void wcn_sblock_put(struct wcn *wcn, int ch, struct wcn_block *blk);
int wcn_sblock_receive(struct wcn *wcn, int ch, struct wcn_block *blk);
void wcn_sblock_release(struct wcn *wcn, int ch, struct wcn_block *blk);

#endif
