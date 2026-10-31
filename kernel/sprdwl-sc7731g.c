// SPDX-License-Identifier: GPL-2.0-only
/*
 * Spreadtrum WCN Wi-Fi over SIPC: a cfg80211 full-MAC STA driver, on top
 * of the WCN co-processor driver (wcn-sc7731g). cp2 does scanning,
 * authentication and association; the host runs the WPA 4-way handshake
 * through wpa_supplicant on the netdev (docs/wifi.md wifi5).
 *
 * Written against this frame's firmware (CP2_WCN_14D_W16.26.3), from the
 * frame's own sprdwl.ko (vendor boot ramdisk, not stripped) plus the user-
 * space work in initramfs/cp2.c.
 */

#include <linux/etherdevice.h>
#include <linux/firmware.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <net/cfg80211.h>

#include "wcn-sc7731g.h"

/* command and event ids on sbuf ring 7 and sblock channel 8 (vendor
 * sipc.h). Only what a station needs */
#define WIFI_CMD_SCAN		1
#define WIFI_CMD_AUTH_TYPE	2
#define WIFI_CMD_WPA_VERSION	3
#define WIFI_CMD_PAIRWISE	4
#define WIFI_CMD_GROUP		5
#define WIFI_CMD_AKM		6
#define WIFI_CMD_CHANNEL	7
#define WIFI_CMD_BSSID		8
#define WIFI_CMD_ESSID		9
#define WIFI_CMD_KEY		10	/* add, delete and default are all this
					 * id; the header's type says which */
#define WIFI_CMD_DISCONNECT	13
#define WIFI_CMD_GET_RSSI	16	/* type GET: s8 signal, then noise at byte 2 */
#define WIFI_CMD_GET_TXRATE	17	/* type GET: see rate_to_info */
#define WIFI_CMD_DEV_OPEN	20
#define WIFI_CMD_DEV_CLOSE	21
#define WIFI_CMD_PM_SUSPEND	32	/* vendor's early_suspend: no payload */
#define WIFI_CMD_PM_RESUME	33	/* vendor's later_resume: no payload */
#define WIFI_CMD_PSK		27	/* 32-byte PMK, not the 4-way handshake */
#define WIFI_CMD_P2P_ENABLE	43

#define WIFI_EV_CONNECT		0
#define WIFI_EV_DISCONNECT	1
#define WIFI_EV_SCANDONE	2
#define WIFI_EV_READY		4
#define WIFI_EV_CQM_RSSI_LOW	12
#define WIFI_EV_CQM_RSSI_HIGH	13
#define WIFI_EV_CQM_14		14	/* vendor: CQM plus a flag, meaning unknown */

/* sprdwl message layer: u32 msg_hdr (type | len<<16), u16 cmd_tag
 * (type<<12 | dir<<11 | level<<8 | id), u16 seq. The frame's firmware
 * takes station as mode 1 (ORed with 0x80 for zero-copy); the stock
 * Samsung source said 0, and we lost a day of wifi3 to that */
#define MSG_CMD			0
#define MSG_CMD_RSP		1
#define CMD_TYPE_GET		0
#define CMD_TYPE_SET		1
#define CMD_TYPE_ADD		2
#define CMD_TYPE_DEL		3
#define CMD_DIR_AP2CP		1
#define CMD_LEVEL_NORM		1
#define CMD_HDR_SZ		8
#define RSP_HDR_SZ		12
#define EVT_HDR_SZ		10
#define DEV_MODE_STA		1
#define FW_ZEROCOPY		0x80
#define NVM_SIZE		256

#define CIPHER_CCMP		4
#define AKM_PSK			1

struct sprdwl {
	struct device *dev;
	struct wcn *wcn;
	struct wiphy *wiphy;
	struct net_device *ndev;
	struct wireless_dev wdev;

	struct cfg80211_scan_request *scan_req;
	struct mutex cmd_lock;
	wait_queue_head_t cmd_wq;
	bool have_resp;
	u8 resp[128];
	u32 resp_len;
	u8 last_cmd_id;

	/* These only ever exit on kthread_should_stop(). There used to be a
	 * sw->stopping flag the loops also checked, set just before
	 * kthread_stop(): a thread that saw it returned on its own, its
	 * task_struct could be gone by the time kthread_stop() got there
	 * (ev_thread wakes every 20 ms; rx_thread sits in a 1 s read across
	 * the whole of DEV_CLOSE), and kthread_stop() on a freed task waits
	 * forever holding RTNL. That was `ip link set wcn0 down` hanging the
	 * frame for half an hour with no pstore: nothing panics, every
	 * netlink user just queues behind RTNL. */
	struct task_struct *rx_thread, *ev_thread, *data_thread;
	bool connecting;	/* ESSID sent, no CONNECT event yet */
	u8 bssid[ETH_ALEN];	/* valid while the carrier is on */
	bool ps;		/* what cp2 was last told; off after DEV_OPEN */
	wait_queue_head_t rx_wq, ev_wq;
	bool rx_kick, ev_kick;

	u8 nvm[NVM_SIZE];
	const char *nvm_name;
};

/* --- command send / response --- */

static int wifi_cmd_type(struct sprdwl *sw, u8 type, u8 id, const void *payload,
			 u32 plen, void *resp_out, u32 *rlen)
{
	u8 msg[512];
	u32 len = CMD_HDR_SZ + plen;
	u32 msg_hdr = MSG_CMD | (len << 16);
	u32 cmd_tag = ((u32)type << 12) | (CMD_DIR_AP2CP << 11)
		    | (CMD_LEVEL_NORM << 8) | id;
	int ret, got;

	if (len > sizeof(msg))
		return -EMSGSIZE;
	msg[0] = msg_hdr; msg[1] = msg_hdr >> 8;
	msg[2] = msg_hdr >> 16; msg[3] = msg_hdr >> 24;
	msg[4] = cmd_tag; msg[5] = cmd_tag >> 8;
	msg[6] = 0; msg[7] = 0;
	if (plen)
		memcpy(msg + CMD_HDR_SZ, payload, plen);

	mutex_lock(&sw->cmd_lock);
	sw->have_resp = false;
	sw->last_cmd_id = id;
	ret = wcn_sbuf_write(sw->wcn, WCN_RING_WIFI, msg, len, 1000);
	if (ret) {
		dev_info(sw->dev, "wifi_cmd id=%u sbuf_write ret=%d\n", id, ret);
		mutex_unlock(&sw->cmd_lock);
		return ret;
	}
	got = wait_event_interruptible_timeout(sw->cmd_wq, sw->have_resp,
					       msecs_to_jiffies(2000));
	mutex_unlock(&sw->cmd_lock);
	if (got < 0)
		return got;
	if (!got) {
		dev_info(sw->dev, "wifi_cmd id=%u response timeout\n", id);
		return -ETIMEDOUT;
	}
	if (sw->resp_len >= RSP_HDR_SZ) {
		u16 status = sw->resp[8] | sw->resp[9] << 8;

		if (status && status != 0xff01)	/* wifi5: 0xff01 is harmless */
			return -EIO;
		if (resp_out && rlen) {
			u32 n = min_t(u32, *rlen, sw->resp_len - RSP_HDR_SZ);

			memcpy(resp_out, sw->resp + RSP_HDR_SZ, n);
			*rlen = n;
		}
	}
	return 0;
}

static int wifi_cmd(struct sprdwl *sw, u8 id, const void *payload, u32 plen,
		    void *resp_out, u32 *rlen)
{
	return wifi_cmd_type(sw, CMD_TYPE_SET, id, payload, plen, resp_out, rlen);
}

static int cmd_u32(struct sprdwl *sw, u8 id, u32 v)
{
	__le32 le = cpu_to_le32(v);

	return wifi_cmd(sw, id, &le, 4, NULL, NULL);
}

static int cmd_u8(struct sprdwl *sw, u8 id, u8 v)
{
	return wifi_cmd(sw, id, &v, 1, NULL, NULL);
}

/* --- sbuf ring 7 reader thread: assemble command responses --- */

static int rx_cmd_thread(void *data)
{
	struct sprdwl *sw = data;
	u8 buf[512];
	int got;

	while (!kthread_should_stop()) {
		got = wcn_sbuf_read(sw->wcn, WCN_RING_WIFI, buf, RSP_HDR_SZ,
				    1000);
		if (got <= 0)
			continue;
		if (got < RSP_HDR_SZ)
			continue;
		{
			u32 h = buf[0] | buf[1] << 8 | buf[2] << 16 |
				(u32)buf[3] << 24;
			u32 total = (h >> 16) & 0xffff;
			u32 extra = total > RSP_HDR_SZ ? total - RSP_HDR_SZ : 0;

			if (extra) {
				u32 n = min_t(u32, extra, sizeof(buf) - RSP_HDR_SZ);

				wcn_sbuf_read(sw->wcn, WCN_RING_WIFI,
					      buf + RSP_HDR_SZ, n, 500);
			}
			memcpy(sw->resp, buf,
			       min_t(u32, total, sizeof(sw->resp)));
			sw->resp_len = min_t(u32, total, sizeof(sw->resp));
			sw->have_resp = true;
			wake_up(&sw->cmd_wq);
		}
	}
	return 0;
}

/* --- platform driver (binds to the WCN parent's "wifi" child) --- */

static int sprdwl_bring_up(struct sprdwl *sw);
static void sprdwl_bring_down(struct sprdwl *sw);

static int sprdwl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sprdwl *sw;

	sw = devm_kzalloc(dev, sizeof(*sw), GFP_KERNEL);
	if (!sw)
		return -ENOMEM;
	sw->dev = dev;
	sw->wcn = wcn_get(dev);
	if (!sw->wcn)
		return dev_err_probe(dev, -ENODEV, "no WCN parent\n");
	mutex_init(&sw->cmd_lock);
	init_waitqueue_head(&sw->cmd_wq);
	init_waitqueue_head(&sw->rx_wq);
	init_waitqueue_head(&sw->ev_wq);
	if (of_property_read_string(dev->of_node, "nvm-firmware-name",
				    &sw->nvm_name))
		sw->nvm_name = "sprd/wifi-nvm.bin";
	platform_set_drvdata(pdev, sw);

	return sprdwl_bring_up(sw);
}

static void sprdwl_remove(struct platform_device *pdev)
{
	struct sprdwl *sw = platform_get_drvdata(pdev);

	sprdwl_bring_down(sw);
}

static const struct of_device_id sprdwl_of_match[] = {
	{ .compatible = "sprd,sc7731g-sprdwl" },
	{}
};
MODULE_DEVICE_TABLE(of, sprdwl_of_match);

static struct platform_driver sprdwl_driver = {
	.probe = sprdwl_probe,
	.remove = sprdwl_remove,
	.driver = {
		.name = "sprdwl-sc7731g",
		.of_match_table = sprdwl_of_match,
	},
};
module_platform_driver(sprdwl_driver);

/* --- wiphy + netdev --- */

/* IEEE 802.11b/g/n: 14 channels, the usual rate table. cp2 scans and
 * joins 2.4 GHz only (vendor driver's wiphy); 5 GHz lives in the firmware
 * for the SR2351 but was never exercised by Android here */
static struct ieee80211_channel sprdwl_chans[] = {
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2412, .hw_value = 1 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2417, .hw_value = 2 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2422, .hw_value = 3 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2427, .hw_value = 4 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2432, .hw_value = 5 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2437, .hw_value = 6 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2442, .hw_value = 7 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2447, .hw_value = 8 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2452, .hw_value = 9 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2457, .hw_value = 10 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2462, .hw_value = 11 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2467, .hw_value = 12 },
	{ .band = NL80211_BAND_2GHZ, .center_freq = 2472, .hw_value = 13 },
};
static struct ieee80211_rate sprdwl_rates[] = {
	{ .bitrate = 10, .hw_value = 0 }, { .bitrate = 20, .hw_value = 1 },
	{ .bitrate = 55, .hw_value = 2 }, { .bitrate = 110, .hw_value = 3 },
	{ .bitrate = 60, .hw_value = 4 }, { .bitrate = 90, .hw_value = 5 },
	{ .bitrate = 120, .hw_value = 6 }, { .bitrate = 180, .hw_value = 7 },
	{ .bitrate = 240, .hw_value = 8 }, { .bitrate = 360, .hw_value = 9 },
	{ .bitrate = 480, .hw_value = 10 }, { .bitrate = 540, .hw_value = 11 },
};
static struct ieee80211_supported_band sprdwl_band_2ghz = {
	.band = NL80211_BAND_2GHZ,
	.channels = sprdwl_chans,
	.n_channels = ARRAY_SIZE(sprdwl_chans),
	.bitrates = sprdwl_rates,
	.n_bitrates = ARRAY_SIZE(sprdwl_rates),
	.ht_cap = { .ht_supported = true,
		    .cap = IEEE80211_HT_CAP_SGI_20,
		    .ampdu_factor = IEEE80211_HT_MAX_AMPDU_32K,
		    .ampdu_density = IEEE80211_HT_MPDU_DENSITY_8,
		    .mcs = { .rx_mask = { 0xff } } },
};
static const u32 sprdwl_ciphers[] = {
	WLAN_CIPHER_SUITE_WEP40,
	WLAN_CIPHER_SUITE_WEP104,
	WLAN_CIPHER_SUITE_TKIP,
	WLAN_CIPHER_SUITE_CCMP,
};

/* DEV_OPEN carries mode | 0x80 (zero-copy) + mac + 256-byte nvm_data +
 * 8-byte ap_timestamp -- 271 bytes of payload. The frame's firmware
 * takes station as mode 1, not 0 (docs/wifi.md wifi4); 0 is an error */
static int sprdwl_dev_open(struct sprdwl *sw)
{
	u8 payload[1 + 6 + NVM_SIZE + 8];
	int ret;

	payload[0] = DEV_MODE_STA | FW_ZEROCOPY;
	ether_addr_copy(payload + 1, sw->ndev->dev_addr);
	memcpy(payload + 7, sw->nvm, NVM_SIZE);
	memset(payload + 7 + NVM_SIZE, 0, 8);
	dev_info(sw->dev, "DEV_OPEN send, mac %pM\n", sw->ndev->dev_addr);
	ret = wifi_cmd(sw, WIFI_CMD_DEV_OPEN, payload, sizeof(payload),
		       NULL, NULL);
	dev_info(sw->dev, "DEV_OPEN ret=%d, ring7 unread=%u\n",
		 ret, wcn_sbuf_tx_consumed(sw->wcn, WCN_RING_WIFI));
	return ret;
}

static int sprdwl_dev_close(struct sprdwl *sw)
{
	u8 mode = DEV_MODE_STA | FW_ZEROCOPY;

	return wifi_cmd(sw, WIFI_CMD_DEV_CLOSE, &mode, 1, NULL, NULL);
}

/* The calibration block comes as its own firmware file at install time
 * (tools/wcn-nvm.py built it from the frame's two .ini files). */
static int load_nvm(struct sprdwl *sw)
{
	const struct firmware *fw;
	int ret;

	ret = request_firmware(&fw, sw->nvm_name, sw->dev);
	if (ret)
		return dev_err_probe(sw->dev, ret, "nvm %s\n", sw->nvm_name);
	if (fw->size != NVM_SIZE) {
		release_firmware(fw);
		return dev_err_probe(sw->dev, -EINVAL, "nvm bad size\n");
	}
	memcpy(sw->nvm, fw->data, NVM_SIZE);
	release_firmware(fw);
	return 0;
}

/* --- sblock-7 RX: data frames from cp2 --- */

/* The vendor's zero-copy rx format (sprdwl_handler on the frame + user-
 * space cp2's data_frame): first byte of the block is an offset; after
 * 2 + offset bytes you reach struct wlan_sblock_recv_data -- u8
 * is_encrypted, 13 reserved bytes, then the Ethernet header and payload.
 * For an unencrypted frame cp2 writes the Ethernet dst/src/type at
 * +2+offset+14..+2+offset+28 (so dst is at the standard 14-byte-header
 * boundary); length cp2 reports includes everything from byte 0. */
static int rx_data_thread(void *data)
{
	struct sprdwl *sw = data;
	struct wcn_block blk;
	struct sk_buff *skb;
	u8 head[64];
	u32 off, len;

	while (!kthread_should_stop()) {
		if (wcn_sblock_receive(sw->wcn, WCN_CH_DATA, &blk)) {
			/* A kick landing after the receive above is still seen
			 * here, as the flag stays set. The timeout is only a
			 * net under a lost doorbell */
			wait_event_interruptible_timeout(sw->rx_wq,
				READ_ONCE(sw->rx_kick) || kthread_should_stop(),
				HZ / 10);
			WRITE_ONCE(sw->rx_kick, false);
			continue;
		}
		if (blk.length < 16)
			goto release;
		memcpy_fromio(head, blk.addr, min_t(u32, sizeof(head), blk.length));
		off = 2 + head[0];
		if (off + 14 + 14 > blk.length)	/* 14 reserved + 14 eth hdr */
			goto release;
		len = blk.length - (off + 14);
		skb = netdev_alloc_skb(sw->ndev, len);
		if (!skb) {
			sw->ndev->stats.rx_dropped++;
			goto release;
		}
		memcpy_fromio(skb_put(skb, len), blk.addr + off + 14, len);
		skb->protocol = eth_type_trans(skb, sw->ndev);
		sw->ndev->stats.rx_packets++;
		sw->ndev->stats.rx_bytes += len;
		netif_rx(skb);
release:
		wcn_sblock_release(sw->wcn, WCN_CH_DATA, &blk);
	}
	return 0;
}

/* From the SIPC irq thread. The data channel is both directions: cp2 hands
 * us a frame, or gives tx blocks back to the pool */
static void data_notify(void *data, enum wcn_notify what)
{
	struct sprdwl *sw = data;

	if (what == WCN_SBLOCK_RX) {
		WRITE_ONCE(sw->rx_kick, true);
		wake_up(&sw->rx_wq);
	} else if (what == WCN_SBLOCK_TX_FREE && netif_running(sw->ndev)) {
		netif_wake_queue(sw->ndev);
	}
}

static void event_notify(void *data, enum wcn_notify what)
{
	struct sprdwl *sw = data;

	if (what == WCN_SBLOCK_RX) {
		WRITE_ONCE(sw->ev_kick, true);
		wake_up(&sw->ev_wq);
	}
}

/* --- event thread: sblock 8 (WIFI_EV_SCANDONE / CONNECT / DISCONNECT) --- */

/* Each scandone record from cp2 (docs/wifi.md wifi4 parse_scandone):
 *  u16 ch_len, u8|u16 ch_num (ch_len is 1 or 2)
 *  u16 rssi_len, s16 rssi (rssi_len is 2)
 *  u16 mgmt_len, u8 mgmt[mgmt_len]  (a probe response or beacon)
 * mgmt starts with the 24-byte 802.11 header (bssid at +10), then the
 * probe response's timestamp, beacon_int, capab_info before the IEs */
static void parse_scandone(struct sprdwl *sw, const u8 *buf, u32 len)
{
	struct wiphy *wiphy = sw->wiphy;
	u32 pos = EVT_HDR_SZ, found = 0;

	while (pos + 10 <= len) {
		u32 ch_len = buf[pos] | buf[pos + 1] << 8;
		u16 ch_num, mgmt_len;
		s16 rssi;
		const struct ieee80211_mgmt *mgmt;
		struct ieee80211_channel *ch;
		struct cfg80211_bss *bss;
		u16 cap, beacon_int;
		u64 tsf;
		size_t ielen;

		pos += 2;
		if (ch_len != 1 && ch_len != 2)
			break;
		ch_num = ch_len == 1 ? buf[pos]
				     : (buf[pos] | buf[pos + 1] << 8);
		pos += ch_len;
		if (pos + 2 > len)
			break;
		pos += 2;		/* rssi_len, always 2 */
		rssi = (s16)(buf[pos] | buf[pos + 1] << 8);
		pos += 2;
		if (pos + 2 > len)
			break;
		mgmt_len = buf[pos] | buf[pos + 1] << 8;
		pos += 2;
		if (pos + mgmt_len > len || mgmt_len < 36)
			break;
		mgmt = (const struct ieee80211_mgmt *)(buf + pos);
		ch = ieee80211_get_channel(wiphy,
			ieee80211_channel_to_frequency(ch_num,
						       NL80211_BAND_2GHZ));
		if (ch) {
			ielen = mgmt_len - offsetof(struct ieee80211_mgmt,
						    u.probe_resp.variable);
			tsf = le64_to_cpu(mgmt->u.probe_resp.timestamp);
			beacon_int = le16_to_cpu(mgmt->u.probe_resp.beacon_int);
			cap = le16_to_cpu(mgmt->u.probe_resp.capab_info);
			bss = cfg80211_inform_bss(wiphy, ch,
					CFG80211_BSS_FTYPE_PRESP,
					mgmt->bssid, tsf, cap, beacon_int,
					mgmt->u.probe_resp.variable, ielen,
					rssi * 100, GFP_KERNEL);
			cfg80211_put_bss(wiphy, bss);
			found++;
		}
		pos += mgmt_len;
	}
	dev_dbg(sw->dev, "scandone, %u BSSes\n", found);
}

static void handle_event(struct sprdwl *sw, const u8 *buf, u32 len)
{
	u16 tag;
	u32 ev_id;

	if (len < EVT_HDR_SZ)
		return;
	tag = buf[4] | buf[5] << 8;
	ev_id = tag & 0xfff;
	switch (ev_id) {
	case WIFI_EV_SCANDONE: {
		struct cfg80211_scan_info info = {};

		parse_scandone(sw, buf, len);
		if (sw->scan_req) {
			cfg80211_scan_done(sw->scan_req, &info);
			sw->scan_req = NULL;
		}
		break;
	}
	case WIFI_EV_CONNECT: {
		/* sprdwl_event_connect_result, read properly this time:
		 *   u16 status_len (1), u8 status
		 *   u16 bssid_len (6), u8 bssid[]
		 *   u8 req_ie_len, u8 req_ie[]	the AssocReq IEs cp2 really sent
		 *   u8 resp_ie_len, 3 bytes we skip like the vendor does,
		 *   u8 resp_ie[]
		 * An earlier version of this comment called p[0] a subtype,
		 * p[13] an ssid_len and p[14] the SSID. That "SSID" was the
		 * first element of req_ie -- id 0 at p[12] -- and dropping
		 * req_ie is what kept the 4-way from finishing: the driver
		 * can't hand cp2 sme->ie (the vendor forwards only a WPS IE),
		 * so cp2 builds its own RSN element for the AssocReq, and
		 * wpa_supplicant only learns what it said from req_ie. Without
		 * it msg 2/4 carries wpa_supplicant's own idea of the element
		 * and the AP, which compares it with the AssocReq, ignores
		 * msg 2 and retransmits msg 1. */
		const u8 *p = buf + EVT_HDR_SZ;
		u32 left = len - EVT_HDR_SZ;
		struct cfg80211_connect_resp_params r = { 0 };
		u32 blen, pos;

		if (left < 5)
			break;
		blen = p[3] | p[4] << 8;
		pos = 5 + blen;
		if (blen != ETH_ALEN || pos >= left)
			break;
		r.status = p[2];
		r.links[0].bssid = p + 5;
		r.req_ie_len = p[pos++];
		if (pos + r.req_ie_len <= left) {
			r.req_ie = p + pos;
			pos += r.req_ie_len;
		} else {
			r.req_ie_len = 0;
			pos = left;
		}
		if (pos + 4 <= left && pos + 4 + p[pos] <= left) {
			r.resp_ie_len = p[pos];
			r.resp_ie = p + pos + 4;
		}
		sw->connecting = false;
		if (!r.status) {
			ether_addr_copy(sw->bssid, p + 5);
			netif_carrier_on(sw->ndev);
		}
		cfg80211_connect_done(sw->ndev, &r, GFP_KERNEL);
		dev_info(sw->dev, "connect status=%u to %pM\n", p[2], p + 5);
		break;
	}
	case WIFI_EV_DISCONNECT:
		/* The vendor's event_disconnect splits on its state too: before
		 * CONNECT this is a failed join, which cfg80211 only accepts as
		 * a connect result */
		if (sw->connecting) {
			struct cfg80211_connect_resp_params r = {
				.status = WLAN_STATUS_UNSPECIFIED_FAILURE,
			};

			sw->connecting = false;
			cfg80211_connect_done(sw->ndev, &r, GFP_KERNEL);
			break;
		}
		netif_carrier_off(sw->ndev);
		cfg80211_disconnected(sw->ndev,
				      len > EVT_HDR_SZ ? buf[EVT_HDR_SZ] : 0,
				      NULL, 0, true, GFP_KERNEL);
		break;
	case WIFI_EV_READY:
		dev_info(sw->dev, "cp2 ready\n");
		break;
	case WIFI_EV_CQM_RSSI_LOW:
	case WIFI_EV_CQM_RSSI_HIGH:
	case WIFI_EV_CQM_14:
		/* The vendor routes all three to cfg80211_cqm_rssi_notify (14
		 * also sets a flag of its own) and answers nothing. cp2 sends 14
		 * about every 100 ms while associated with Bluetooth inquiring,
		 * though we never armed CQM (cmd 44), 2026-10-30. Not
		 * reported: nothing asked for thresholds, and a line each was 10
		 * a second in the log */
		dev_dbg(sw->dev, "cqm event %u\n", ev_id);
		break;
	default:
		dev_info(sw->dev, "unknown event id=%u\n", ev_id);
		break;
	}
}

static int ev_thread_fn(void *data)
{
	struct sprdwl *sw = data;
	struct wcn_block blk;

	while (!kthread_should_stop()) {
		if (wcn_sblock_receive(sw->wcn, WCN_CH_EVENT, &blk)) {
			wait_event_interruptible_timeout(sw->ev_wq,
				READ_ONCE(sw->ev_kick) || kthread_should_stop(),
				HZ / 10);
			WRITE_ONCE(sw->ev_kick, false);
			continue;
		}
		{
			u8 *buf = kmalloc(blk.length, GFP_KERNEL);

			if (buf) {
				memcpy_fromio(buf, blk.addr, blk.length);
				handle_event(sw, buf, blk.length);
				kfree(buf);
			}
		}
		wcn_sblock_release(sw->wcn, WCN_CH_EVENT, &blk);
	}
	return 0;
}

/* --- cfg80211 ops --- */

static struct sprdwl *sprdwl_from_wiphy(struct wiphy *wiphy)
{
	return *(struct sprdwl **)wiphy_priv(wiphy);
}

static int sprdwl_cfg80211_scan(struct wiphy *wiphy,
				struct cfg80211_scan_request *req)
{
	struct sprdwl *sw = sprdwl_from_wiphy(wiphy);
	u8 buf[256];
	u32 plen = 0;
	unsigned int i;
	int ret;

	if (sw->scan_req)
		return -EBUSY;
	for (i = 0; i < req->n_ssids; i++) {
		if (!req->ssids[i].ssid_len)
			continue;
		if (plen + 1 + req->ssids[i].ssid_len > sizeof(buf))
			return -E2BIG;
		buf[plen++] = req->ssids[i].ssid_len;
		memcpy(buf + plen, req->ssids[i].ssid,
		       req->ssids[i].ssid_len);
		plen += req->ssids[i].ssid_len;
	}
	sw->scan_req = req;
	ret = wifi_cmd(sw, WIFI_CMD_SCAN, buf, plen, NULL, NULL);
	if (ret) {
		sw->scan_req = NULL;
		return ret;
	}
	return 0;
}

/* Map NL80211's cipher suite to the firmware's byte: vendor sipc.h */
static int cipher_to_fw(u32 c)
{
	switch (c) {
	case WLAN_CIPHER_SUITE_WEP40:	return 1;
	case WLAN_CIPHER_SUITE_WEP104:	return 2;
	case WLAN_CIPHER_SUITE_TKIP:	return 3;
	case WLAN_CIPHER_SUITE_CCMP:	return 4;
	default:			return -1;
	}
}

/* The frame's connect sequence (sprdwl_cfg80211_connect disassembly, with
 * ids from vendor sipc.h): WPA_VERSION, AUTH_TYPE, PAIRWISE, GROUP, AKM,
 * CHANNEL, BSSID, ESSID. ESSID starts the join -- there is no CONNECT
 * command. cp2 does auth/assoc; wpa_supplicant does the 4-way handshake
 * over the netdev (docs/wifi.md wifi5) */
static int sprdwl_cfg80211_connect(struct wiphy *wiphy, struct net_device *ndev,
				   struct cfg80211_connect_params *sme)
{
	struct sprdwl *sw = sprdwl_from_wiphy(wiphy);
	int ret, pw, gr, akm = 1 /* PSK */;
	u32 chan;

	if (!sme->bssid || !sme->ssid_len)
		return -EINVAL;
	pw = sme->crypto.n_ciphers_pairwise ?
	     cipher_to_fw(sme->crypto.ciphers_pairwise[0]) :
	     cipher_to_fw(WLAN_CIPHER_SUITE_CCMP);
	gr = cipher_to_fw(sme->crypto.cipher_group);
	if (sme->crypto.n_akm_suites) {
		if (sme->crypto.akm_suites[0] == WLAN_AKM_SUITE_8021X)
			akm = 2;
	}
	chan = sme->channel ?
	       ieee80211_frequency_to_channel(sme->channel->center_freq) : 0;

	ret = cmd_u8(sw, WIFI_CMD_P2P_ENABLE, 0);
	if (!ret) ret = cmd_u32(sw, WIFI_CMD_WPA_VERSION,
			       sme->crypto.wpa_versions ?: 2);
	if (!ret) ret = cmd_u32(sw, WIFI_CMD_AUTH_TYPE,
			       sme->auth_type == NL80211_AUTHTYPE_SHARED_KEY);
	if (!ret && pw >= 0) ret = cmd_u32(sw, WIFI_CMD_PAIRWISE, pw);
	if (!ret && gr >= 0) ret = cmd_u32(sw, WIFI_CMD_GROUP, gr);
	if (!ret) ret = cmd_u8(sw, WIFI_CMD_AKM, akm);
	if (!ret && chan) ret = cmd_u32(sw, WIFI_CMD_CHANNEL, chan);
	if (!ret) ret = wifi_cmd(sw, WIFI_CMD_BSSID, sme->bssid, 6, NULL, NULL);
	sw->connecting = !ret;	/* before ESSID: the event can beat the response */
	if (!ret) ret = wifi_cmd(sw, WIFI_CMD_ESSID, sme->ssid,
				sme->ssid_len, NULL, NULL);
	if (ret)
		sw->connecting = false;
	return ret;
}

static int sprdwl_cfg80211_disconnect(struct wiphy *wiphy,
				      struct net_device *ndev,
				      u16 reason)
{
	struct sprdwl *sw = sprdwl_from_wiphy(wiphy);
	__le16 le = cpu_to_le16(reason);

	return wifi_cmd(sw, WIFI_CMD_DISCONNECT, &le, 2, NULL, NULL);
}

/* Vendor's add_key payload (struct wlan_sipc_add_key): u8 mac[6], u8
 * keyseq[8], u8 pairwise, u8 cypher_type, u8 key_index, u8 key_len,
 * then the key bytes. Sent as type ADD: this was going out as SET, which
 * cp2 takes as "set default key" with the index in the first byte --
 * its log said "set key idx = 0x52" (mac[0] of the AP) and 0xff (of the
 * broadcast address), no key was ever installed, and it kept filtering
 * every data frame with filter_host_rx_11i_supp_sta after a 4-way
 * wpa_supplicant called COMPLETED. del_key and set_default_key had made-up
 * ids on top of that (11 and 12); sprdwl_{add,del,set}_key_cmd all use 10 */
static int sprdwl_cfg80211_add_key(struct wiphy *wiphy, struct net_device *ndev,
				   int link_id, u8 key_index, bool pairwise,
				   const u8 *mac_addr, struct key_params *params)
{
	struct sprdwl *sw = sprdwl_from_wiphy(wiphy);
	u8 payload[6 + 8 + 4 + 32];
	int cy;

	if (params->key_len > 32)
		return -EINVAL;
	cy = cipher_to_fw(params->cipher);
	if (cy < 0)
		return -EOPNOTSUPP;
	if (mac_addr)
		ether_addr_copy(payload, mac_addr);
	else
		eth_broadcast_addr(payload);
	memset(payload + 6, 0, 8);
	if (params->seq_len)
		memcpy(payload + 6, params->seq,
		       min_t(u32, params->seq_len, 8));
	payload[14] = pairwise;
	payload[15] = cy;
	payload[16] = key_index;
	payload[17] = params->key_len;
	memcpy(payload + 18, params->key, params->key_len);
	return wifi_cmd_type(sw, CMD_TYPE_ADD, WIFI_CMD_KEY, payload,
			     18 + params->key_len, NULL, NULL);
}

static int sprdwl_cfg80211_del_key(struct wiphy *wiphy, struct net_device *ndev,
				   int link_id, u8 key_index, bool pairwise,
				   const u8 *mac_addr)
{
	struct sprdwl *sw = sprdwl_from_wiphy(wiphy);
	u8 payload[8];

	/* cfg80211 clears the keys when a connection ends, which on the way
	 * down crosses DEV_CLOSE: cp2 doesn't answer, and that was `ip link
	 * set wcn0 down` taking 2 s. The radio forgets them anyway */
	if (!netif_running(ndev))
		return 0;
	/* u8 key_index, a byte the vendor never sets, u8 mac[6] */
	payload[0] = key_index;
	payload[1] = 0;
	if (mac_addr)
		ether_addr_copy(payload + 2, mac_addr);
	else
		eth_broadcast_addr(payload + 2);
	return wifi_cmd_type(sw, CMD_TYPE_DEL, WIFI_CMD_KEY, payload, 8,
			     NULL, NULL);
}

static int sprdwl_cfg80211_set_default_key(struct wiphy *wiphy,
					   struct net_device *ndev,
					   int link_id, u8 key_index,
					   bool unicast, bool multicast)
{
	struct sprdwl *sw = sprdwl_from_wiphy(wiphy);

	return cmd_u8(sw, WIFI_CMD_KEY, key_index);
}

/* cp2's rate word, from the table sprdwl_cfg80211_get_station searches: legacy
 * rates in whole Mb/s (5 for 5.5), HT as 0x80 | MCS, 20 MHz and long GI. The
 * vendor reports 1 Mb/s for anything it doesn't know; we report nothing */
static bool rate_to_info(u32 r, struct rate_info *ri)
{
	r &= 0xff;
	if (r & 0x80) {
		if ((r & 0x7f) > 15)
			return false;
		ri->flags = RATE_INFO_FLAGS_MCS;
		ri->mcs = r & 0x7f;
		ri->bw = RATE_INFO_BW_20;
		return true;
	}
	switch (r) {
	case 1: case 2: case 11: case 6: case 9: case 12:
	case 18: case 24: case 36: case 48: case 54:
		ri->legacy = r * 10;
		return true;
	case 5:
		ri->legacy = 55;
		return true;
	}
	return false;
}

/* Signal goes in as signal_avg too: cp2 has one number, and a signal
 * sensor may read `signal avg` from `iw dev wlan0 station dump`. The vendor set
 * neither that nor dump_station; its Android only ever asked by BSSID */
static int sprdwl_fill_station(struct sprdwl *sw, struct station_info *sinfo)
{
	__le32 v;
	u32 n = sizeof(v);
	int ret;

	ret = wifi_cmd_type(sw, CMD_TYPE_GET, WIFI_CMD_GET_RSSI, NULL, 0, &v, &n);
	if (ret)
		return ret;
	if (n != sizeof(v))
		return -EIO;
	sinfo->signal = (s8)le32_to_cpu(v);
	sinfo->signal_avg = sinfo->signal;
	sinfo->filled |= BIT_ULL(NL80211_STA_INFO_SIGNAL) |
			 BIT_ULL(NL80211_STA_INFO_SIGNAL_AVG);

	n = sizeof(v);
	if (!wifi_cmd_type(sw, CMD_TYPE_GET, WIFI_CMD_GET_TXRATE, NULL, 0, &v, &n) &&
	    n == sizeof(v) && rate_to_info(le32_to_cpu(v), &sinfo->txrate))
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_BITRATE);

	sinfo->rx_packets = sw->ndev->stats.rx_packets;
	sinfo->tx_packets = sw->ndev->stats.tx_packets;
	sinfo->rx_bytes = sw->ndev->stats.rx_bytes;
	sinfo->tx_bytes = sw->ndev->stats.tx_bytes;
	sinfo->filled |= BIT_ULL(NL80211_STA_INFO_RX_PACKETS) |
			 BIT_ULL(NL80211_STA_INFO_TX_PACKETS) |
			 BIT_ULL(NL80211_STA_INFO_RX_BYTES64) |
			 BIT_ULL(NL80211_STA_INFO_TX_BYTES64);
	return 0;
}

static int sprdwl_cfg80211_get_station(struct wiphy *wiphy,
				       struct net_device *ndev, const u8 *mac,
				       struct station_info *sinfo)
{
	struct sprdwl *sw = sprdwl_from_wiphy(wiphy);

	if (!netif_carrier_ok(ndev) || !ether_addr_equal(mac, sw->bssid))
		return -ENOENT;
	return sprdwl_fill_station(sw, sinfo);
}

static int sprdwl_cfg80211_dump_station(struct wiphy *wiphy,
					struct net_device *ndev, int idx,
					u8 *mac, struct station_info *sinfo)
{
	struct sprdwl *sw = sprdwl_from_wiphy(wiphy);

	if (idx || !netif_carrier_ok(ndev))
		return -ENOENT;
	ether_addr_copy(mac, sw->bssid);
	return sprdwl_fill_station(sw, sinfo);
}

/* Power save as `iw dev wlan0 set power_save on|off`. The vendor had no such
 * op: its only power commands are early_suspend (32) and later_resume (33),
 * sent from the screen-off and screen-on hooks, so that's what on and off
 * mean here. Whether cp2 then dozes between beacons, which is what this is
 * for, isn't known yet: associated, the radio never leaves Wi-Fi long enough
 * for a Bluetooth inquiry to finish, disconnected it finishes in 6.4 s
 * (2026-10-30). cfg80211 calls this on every up with its own idea
 * of the state, off unless asked, which cp2 already is after DEV_OPEN, so
 * only a change goes to cp2. Down, cp2 is off and DEV_OPEN starts it awake;
 * the state is kept, and sent again by that same call at up */
static int sprdwl_cfg80211_set_power_mgmt(struct wiphy *wiphy,
					  struct net_device *ndev,
					  bool enabled, int timeout)
{
	struct sprdwl *sw = sprdwl_from_wiphy(wiphy);
	int ret;

	if (!netif_running(ndev) || enabled == sw->ps)
		return 0;
	ret = wifi_cmd(sw, enabled ? WIFI_CMD_PM_SUSPEND : WIFI_CMD_PM_RESUME,
		       NULL, 0, NULL, NULL);
	if (!ret)
		sw->ps = enabled;
	return ret;
}

static const struct cfg80211_ops sprdwl_cfg80211_ops = {
	.scan = sprdwl_cfg80211_scan,
	.connect = sprdwl_cfg80211_connect,
	.disconnect = sprdwl_cfg80211_disconnect,
	.add_key = sprdwl_cfg80211_add_key,
	.del_key = sprdwl_cfg80211_del_key,
	.set_default_key = sprdwl_cfg80211_set_default_key,
	.get_station = sprdwl_cfg80211_get_station,
	.dump_station = sprdwl_cfg80211_dump_station,
	.set_power_mgmt = sprdwl_cfg80211_set_power_mgmt,
};

static int sprdwl_ndo_open(struct net_device *ndev)
{
	struct sprdwl *sw = container_of(ndev->ieee80211_ptr,
					 struct sprdwl, wdev);
	int ret;

	/* Load the NVM here, not at probe, from when /lib/firmware was the only
	 * place for it and the rootfs wasn't mounted yet. A normal build has it
	 * in the kernel image now (build/kernel.sh, EXTRA_FIRMWARE) */
	ret = load_nvm(sw);
	if (ret)
		return ret;
	wcn_register_notify(sw->wcn, WCN_KEY_SBLOCK(WCN_CH_DATA), data_notify, sw);
	wcn_register_notify(sw->wcn, WCN_KEY_SBLOCK(WCN_CH_EVENT), event_notify, sw);
	ret = wcn_power_up(sw->wcn);
	if (ret)
		return ret;
	sw->rx_thread = kthread_run(rx_cmd_thread, sw, "sprdwl-rx");
	if (IS_ERR(sw->rx_thread)) {
		ret = PTR_ERR(sw->rx_thread);
		sw->rx_thread = NULL;
		goto err_pdown;
	}
	sw->ps = false;		/* a fresh DEV_OPEN */
	ret = sprdwl_dev_open(sw);
	if (ret)
		goto err_rx;
	sw->ev_thread = kthread_run(ev_thread_fn, sw, "sprdwl-ev");
	if (IS_ERR(sw->ev_thread)) {
		ret = PTR_ERR(sw->ev_thread);
		sw->ev_thread = NULL;
		goto err_close;
	}
	sw->data_thread = kthread_run(rx_data_thread, sw, "sprdwl-rxd");
	if (IS_ERR(sw->data_thread)) {
		ret = PTR_ERR(sw->data_thread);
		sw->data_thread = NULL;
		goto err_ev;
	}
	netif_carrier_off(ndev);
	netif_start_queue(ndev);
	/* cp2 stops listening 10-20 s after DEV_OPEN if no scan followed: no
	 * response, no interrupt back, and only a restart cures it. A first
	 * scan at +5 or +10 s keeps it alive, and from then on it sleeps and
	 * wakes on demand (scanning fine after 300 s idle). Android never saw
	 * this because wpa_supplicant's first scan was already on its way, but
	 * `ip link set wcn0 up` and a supplicant later is a dead radio. Any
	 * other first command doesn't count: P2P_ENABLE, which is harmless and
	 * what connect starts with, was tried here and cp2 still went deaf at
	 * 75 s. So one scan of our own: scan_req stays NULL, and scandone just
	 * fills cfg80211's BSS table */
	ret = wifi_cmd(sw, WIFI_CMD_SCAN, NULL, 0, NULL, NULL);
	if (ret)
		dev_warn(sw->dev, "first scan: %d\n", ret);
	return 0;

err_ev:
	kthread_stop(sw->ev_thread);
	sw->ev_thread = NULL;
err_close:
	sprdwl_dev_close(sw);
err_rx:
	kthread_stop(sw->rx_thread);
	sw->rx_thread = NULL;
err_pdown:
	wcn_power_down(sw->wcn);
	return ret;
}

static int sprdwl_ndo_stop(struct net_device *ndev)
{
	struct sprdwl *sw = container_of(ndev->ieee80211_ptr,
					 struct sprdwl, wdev);

	netif_stop_queue(ndev);
	if (sw->data_thread) {
		kthread_stop(sw->data_thread);
		sw->data_thread = NULL;
	}
	if (sw->ev_thread) {
		kthread_stop(sw->ev_thread);
		sw->ev_thread = NULL;
	}
	sprdwl_dev_close(sw);
	if (sw->rx_thread) {
		kthread_stop(sw->rx_thread);
		sw->rx_thread = NULL;
	}
	wcn_power_down(sw->wcn);
	/* The event thread is gone, so whatever cp2 owed us never arrives:
	 * finish it here or cfg80211 keeps a scan and a connection that
	 * outlive the radio (and the next scan is -EBUSY for good) */
	if (sw->scan_req) {
		struct cfg80211_scan_info info = { .aborted = true };

		cfg80211_scan_done(sw->scan_req, &info);
		sw->scan_req = NULL;
	}
	sw->connecting = false;
	if (netif_carrier_ok(ndev)) {
		netif_carrier_off(ndev);
		cfg80211_disconnected(ndev, 0, NULL, 0, true, GFP_KERNEL);
	}
	return 0;
}

/* sblock-7 TX, as the frame's sprdwl_start_xmit does it: get a free
 * block from the pool, memcpy_toio the frame at offset 50 (vendor's
 * SIPC_TRANS_OFFSET for the zero-copy build) and send the block with
 * length = skb->len. cp2 reads the frame at blk + 50 */
#define SPRDWL_TX_OFF		50

static netdev_tx_t sprdwl_ndo_start_xmit(struct sk_buff *skb,
					 struct net_device *ndev)
{
	struct sprdwl *sw = container_of(ndev->ieee80211_ptr,
					 struct sprdwl, wdev);
	struct wcn_block blk;
	int ret;

	ret = wcn_sblock_get(sw->wcn, WCN_CH_DATA, &blk);
	if (ret) {
		/* Pool empty. cp2 returns blocks with an EVT_SBLK_RELEASE and
		 * data_notify restarts the queue; stop first and look again,
		 * or a release between the get and the stop leaves the queue
		 * stopped for good. Nothing used to wake it at all: the first
		 * 64 frames faster than the air killed the link with
		 * wpa_supplicant still saying COMPLETED */
		netif_stop_queue(ndev);
		smp_mb();
		ret = wcn_sblock_get(sw->wcn, WCN_CH_DATA, &blk);
		if (ret)
			return NETDEV_TX_BUSY;
		netif_start_queue(ndev);
	}
	if (blk.length < skb->len + SPRDWL_TX_OFF) {
		wcn_sblock_put(sw->wcn, WCN_CH_DATA, &blk);
		goto drop;
	}
	memcpy_toio(blk.addr + SPRDWL_TX_OFF, skb->data, skb->len);
	blk.length = skb->len;
	ret = wcn_sblock_send(sw->wcn, WCN_CH_DATA, &blk);
	if (ret) {
		wcn_sblock_put(sw->wcn, WCN_CH_DATA, &blk);
		goto drop;
	}
	ndev->stats.tx_packets++;
	ndev->stats.tx_bytes += skb->len;
	dev_consume_skb_any(skb);
	return NETDEV_TX_OK;

drop:
	ndev->stats.tx_dropped++;
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static const struct net_device_ops sprdwl_netdev_ops = {
	.ndo_open = sprdwl_ndo_open,
	.ndo_stop = sprdwl_ndo_stop,
	.ndo_start_xmit = sprdwl_ndo_start_xmit,
	.ndo_set_mac_address = eth_mac_addr,
};

static void sprdwl_ndev_setup(struct net_device *ndev)
{
	ether_setup(ndev);
	ndev->netdev_ops = &sprdwl_netdev_ops;
	ndev->needs_free_netdev = true;
}

/* This radio has no MAC of its own: the vendor's wcnd made one up and kept it
 * in /data/misc/wifi/wifimac.txt. Ours comes from the board's serial, which
 * cboot puts on the command line, so it is stable across boots, slots and
 * reinstalls and different on the next board; locally administered, because
 * nobody assigned it. Random only if there is no serial. A unit that needs a
 * particular one has FRAMEBOOT/wifi-mac, which frameboot applies before
 * anything brings the interface up (ndo_set_mac_address, as DEV_OPEN reads
 * dev_addr). This used to read of_root's "serial-number", a property this
 * DT doesn't have, so it was always random, and then a hardcoded MAC took over
 * while the 4-way was being debugged; the MAC never was that bug. */
static void sprdwl_derive_mac(struct sprdwl *sw, u8 mac[6])
{
	static const char key[] = "androidboot.serialno=";
	const char *s = NULL;
	u32 h = 0x811c9dc5;	/* FNV-1a of the serial */

	eth_random_addr(mac);
	if (of_chosen)
		s = of_get_property(of_chosen, "bootargs", NULL);
	s = s ? strstr(s, key) : NULL;
	if (!s) {
		dev_warn(sw->dev, "no serial on the command line, random MAC\n");
		return;
	}
	for (s += sizeof(key) - 1; *s && *s != ' '; s++)
		h = (h ^ (u8)*s) * 0x01000193;
	mac[0] = 0x02;		/* locally administered, unicast */
	mac[1] = 0xfa;
	mac[2] = (h >> 24) & 0xff;
	mac[3] = (h >> 16) & 0xff;
	mac[4] = (h >> 8) & 0xff;
	mac[5] = h & 0xff;
}

static int sprdwl_bring_up(struct sprdwl *sw)
{
	struct wiphy *wiphy;
	struct net_device *ndev;
	u8 mac[6];
	int ret;

	wiphy = wiphy_new(&sprdwl_cfg80211_ops, sizeof(struct sprdwl *));
	if (!wiphy)
		return -ENOMEM;
	*(struct sprdwl **)wiphy_priv(wiphy) = sw;
	wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	wiphy->bands[NL80211_BAND_2GHZ] = &sprdwl_band_2ghz;
	wiphy->cipher_suites = sprdwl_ciphers;
	wiphy->n_cipher_suites = ARRAY_SIZE(sprdwl_ciphers);
	wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	wiphy->max_scan_ssids = 1;
	wiphy->max_scan_ie_len = 256;
	set_wiphy_dev(wiphy, sw->dev);
	ret = wiphy_register(wiphy);
	if (ret) {
		wiphy_free(wiphy);
		return ret;
	}
	sw->wiphy = wiphy;

	/* wlan%d: this is the frame's Wi-Fi now, what the userland's networking,
	 * wpa_supplicant and DHCP drive. During bring-up it was wcn%d to keep
	 * those agents off it while the dongle was wlan0; it probes long before
	 * USB host mode, so a dongle still plugged in would be wlan1 and left
	 * alone */
	ndev = alloc_netdev(0, "wlan%d", NET_NAME_UNKNOWN, sprdwl_ndev_setup);
	if (!ndev) {
		wiphy_unregister(wiphy);
		wiphy_free(wiphy);
		return -ENOMEM;
	}
	sprdwl_derive_mac(sw, mac);
	eth_hw_addr_set(ndev, mac);
	SET_NETDEV_DEV(ndev, &wiphy->dev);
	ndev->ieee80211_ptr = &sw->wdev;
	sw->wdev.wiphy = wiphy;
	sw->wdev.iftype = NL80211_IFTYPE_STATION;
	sw->wdev.netdev = ndev;
	ret = register_netdev(ndev);
	if (ret) {
		free_netdev(ndev);
		wiphy_unregister(wiphy);
		wiphy_free(wiphy);
		return ret;
	}
	/* wdev_priv() must return our sprdwl; we embedded sw->wdev in
	 * sprdwl so it works out */
	sw->ndev = ndev;
	return 0;
}

static void sprdwl_bring_down(struct sprdwl *sw)
{
	if (sw->ndev)
		unregister_netdev(sw->ndev);
	if (sw->wiphy) {
		wiphy_unregister(sw->wiphy);
		wiphy_free(sw->wiphy);
	}
}

MODULE_DESCRIPTION("Spreadtrum SC7731G WCN Wi-Fi (sprdwl over SIPC)");
MODULE_LICENSE("GPL");
