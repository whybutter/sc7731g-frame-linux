// SPDX-License-Identifier: GPL-2.0-only
/*
 * The SC7731G's built-in Bluetooth as an HCI device, on the WCN core
 * (wcn-sc7731g.c) next to the Wi-Fi (sprdwl-sc7731g.c): both are radios in
 * the same co-processor, cp2, and share it through wcn_power_up/down.
 *
 * What it is (docs/bluetooth.md bt1-bt7): a BR/EDR 2.1 controller,
 * manufacturer 0x01ec, no LE at all in this firmware. The transport is the
 * vendor's stty4bt: sbuf channel 4 ring 10, raw H4 both ways (a packet type
 * byte, then the HCI packet). Before the stack's first command cp2 wants
 * the 144-byte PSKEY block (sr2351_pskey, first byte 5), with no H4 type in
 * front, and answers it with a byte 5 (05 08 08 08 on this firmware).
 *
 * The PSKEY comes from user space. It is built from the frame's Android
 * connectivity_configure.ini, which stays on the eMMC and out of git and
 * the image, and the BD_ADDR in it from the board's serial
 * (rootfs/usr/lib/sc7731g-bt/pskey.sh). sc7731g-bt writes it to the
 * `pskey` attribute of this device; the first valid write registers the
 * hci device, so no hciN exists that would fail its first power-on. cp2
 * takes it once per start: every open compares wcn_boot_count() with the
 * start the PSKEY last went to, so an hci0 closed and reopened while the
 * Wi-Fi kept cp2 running doesn't send it again, and one reopened on a
 * fresh cp2 does. A new PSKEY written later is used from the next start.
 *
 * Until then this replaces the earlier user-space bridge (initramfs/bt.c) (ring 10 <->
 * /dev/vhci), which stood down once this kernel's WCN driver owned cp2.
 * The framing and the PSKEY handling are the bridge's, ported.
 *
 * cp2 notifies ring 10's writes from its interrupt thread, which may not
 * sleep: the notify only queues rx work. tx is queued too, because
 * wcn_sbuf_write may wait for room in the 4 KiB ring.
 */
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/skbuff.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

#include "wcn-sc7731g.h"

#define PSKEY_SIZE	0x90		/* sizeof(sr2351_pskey), libbt-vendor */
#define PSKEY_CMD	5

/* H4 packet types */
#define H4_CMD		1
#define H4_ACL		2
#define H4_SCO		3
#define H4_EVT		4

/* the largest packet cp2 sends is an ACL one of ~1 KiB (Read_Buffer_Size),
 * and the ring is 4 KiB: a whole ring of it always fits */
#define RXBUF		(4096 + 1100)

struct btwcn {
	struct device *dev;
	struct wcn *wcn;
	struct hci_dev *hdev;
	bool registered;
	struct mutex lock;		/* pskey, registration, open/close */

	u8 pskey[PSKEY_SIZE];
	bool have_pskey;
	u32 pskey_boot;			/* wcn_boot_count() it went to; 0 none */

	bool up;			/* open and past the PSKEY: rx is H4 */
	struct workqueue_struct *wq;	/* ordered: rx and tx never overlap */
	struct work_struct rx_work, tx_work;
	struct sk_buff_head txq;

	u8 rx[RXBUF];
	u32 rxn;
};

static void btwcn_notify(void *data, enum wcn_notify what)
{
	struct btwcn *bt = data;

	if (what == WCN_SBUF_RX && READ_ONCE(bt->up))
		queue_work(bt->wq, &bt->rx_work);
}

/* how long the H4 packet at the head of rx is: 0 while its header is
 * still incomplete, -1 when the first byte starts no H4 packet */
static int h4_len(const u8 *p, u32 n)
{
	if (!n)
		return 0;
	switch (p[0]) {
	case H4_EVT:
		return n >= 3 ? 3 + p[2] : 0;
	case H4_ACL:
		return n >= 5 ? 5 + (p[3] | p[4] << 8) : 0;
	case H4_SCO:
		return n >= 4 ? 4 + p[3] : 0;
	}
	return -1;
}

static void btwcn_rx_work(struct work_struct *work)
{
	struct btwcn *bt = container_of(work, struct btwcn, rx_work);
	struct hci_dev *hdev = bt->hdev;
	int got, len;

	while (READ_ONCE(bt->up)) {
		got = wcn_sbuf_read(bt->wcn, WCN_RING_BT, bt->rx + bt->rxn,
				    RXBUF - bt->rxn, 0);
		if (got <= 0)
			break;
		bt->rxn += got;
		hdev->stat.byte_rx += got;

		for (;;) {
			struct sk_buff *skb;

			len = h4_len(bt->rx, bt->rxn);
			if (len < 0 || len > RXBUF) {
				/* cp2 never sent one in bt1-bt7; a driver
				 * that keeps it would never resync */
				bt_dev_err_ratelimited(hdev,
					"not H4 (%02x), dropping %u bytes",
					bt->rx[0], bt->rxn);
				hdev->stat.err_rx++;
				bt->rxn = 0;
				break;
			}
			if (!len || bt->rxn < len)
				break;
			skb = bt_skb_alloc(len - 1, GFP_KERNEL);
			if (!skb) {
				hdev->stat.err_rx++;
			} else {
				hci_skb_pkt_type(skb) = bt->rx[0];
				skb_put_data(skb, bt->rx + 1, len - 1);
				switch (bt->rx[0]) {
				case H4_EVT:
					hdev->stat.evt_rx++;
					break;
				case H4_ACL:
					hdev->stat.acl_rx++;
					break;
				case H4_SCO:
					hdev->stat.sco_rx++;
					break;
				}
				hci_recv_frame(hdev, skb);
			}
			bt->rxn -= len;
			memmove(bt->rx, bt->rx + len, bt->rxn);
		}
	}
}

static void btwcn_tx_work(struct work_struct *work)
{
	struct btwcn *bt = container_of(work, struct btwcn, tx_work);
	struct hci_dev *hdev = bt->hdev;
	struct sk_buff *skb;
	int ret;

	while ((skb = skb_dequeue(&bt->txq))) {
		if (!READ_ONCE(bt->up)) {
			kfree_skb(skb);
			continue;
		}
		memcpy(skb_push(skb, 1), &hci_skb_pkt_type(skb), 1);
		/* the ring is 4 KiB and cp2 drains it at air speed: a second
		 * without room means it stopped reading */
		ret = wcn_sbuf_write(bt->wcn, WCN_RING_BT, skb->data, skb->len,
				     1000);
		if (ret) {
			bt_dev_err_ratelimited(hdev, "ring 10 write: %d", ret);
			hdev->stat.err_tx++;
		} else {
			hdev->stat.byte_tx += skb->len;
		}
		kfree_skb(skb);
	}
}

/* everything cp2 left in ring 10, gone: a reply from before this open, or
 * the rest of a PSKEY answer */
static void btwcn_drain(struct btwcn *bt)
{
	u8 junk[64];

	while (wcn_sbuf_read(bt->wcn, WCN_RING_BT, junk, sizeof(junk), 0) > 0)
		;
}

/* The PSKEY, the way libbt-vendor gives it: write the block, wait up to 5 s
 * for the answer, read what came (4 bytes on this firmware) and look only
 * at the first. Called with the ring's rx not yet H4 (bt->up false) */
static int btwcn_send_pskey(struct btwcn *bt)
{
	u8 ack[16];
	int ret, n;

	btwcn_drain(bt);
	ret = wcn_sbuf_write(bt->wcn, WCN_RING_BT, bt->pskey, PSKEY_SIZE, 1000);
	if (ret) {
		dev_err(bt->dev, "PSKEY write: %d\n", ret);
		return ret;
	}
	n = wcn_sbuf_read(bt->wcn, WCN_RING_BT, ack, sizeof(ack), 5000);
	if (n < 0) {
		dev_err(bt->dev, "no PSKEY answer: %d\n", n);
		return n == -ETIMEDOUT ? -EIO : n;
	}
	msleep(50);
	btwcn_drain(bt);
	if (ack[0] != PSKEY_CMD) {
		dev_err(bt->dev, "PSKEY answer %*ph, not 05\n", n, ack);
		return -EIO;
	}
	dev_info(bt->dev, "PSKEY taken (%*ph), cp2 start %u\n", n, ack,
		 wcn_boot_count(bt->wcn));
	return 0;
}

static int btwcn_open(struct hci_dev *hdev)
{
	struct btwcn *bt = hci_get_drvdata(hdev);
	int ret;

	mutex_lock(&bt->lock);
	ret = wcn_power_up(bt->wcn);
	if (ret) {
		bt_dev_err(hdev, "cp2 power up: %d", ret);
		goto out;
	}
	if (bt->pskey_boot != wcn_boot_count(bt->wcn)) {
		ret = btwcn_send_pskey(bt);
		if (ret) {
			wcn_power_down(bt->wcn);
			goto out;
		}
		bt->pskey_boot = wcn_boot_count(bt->wcn);
	} else {
		btwcn_drain(bt);
	}
	bt->rxn = 0;
	WRITE_ONCE(bt->up, true);
	/* anything cp2 wrote between the drain and here has no notify left */
	queue_work(bt->wq, &bt->rx_work);
out:
	mutex_unlock(&bt->lock);
	return ret;
}

static int btwcn_close(struct hci_dev *hdev)
{
	struct btwcn *bt = hci_get_drvdata(hdev);

	mutex_lock(&bt->lock);
	if (bt->up) {
		WRITE_ONCE(bt->up, false);
		cancel_work_sync(&bt->rx_work);
		cancel_work_sync(&bt->tx_work);
		skb_queue_purge(&bt->txq);
		wcn_power_down(bt->wcn);
	}
	mutex_unlock(&bt->lock);
	return 0;
}

static int btwcn_flush(struct hci_dev *hdev)
{
	struct btwcn *bt = hci_get_drvdata(hdev);

	skb_queue_purge(&bt->txq);
	return 0;
}

static int btwcn_send(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct btwcn *bt = hci_get_drvdata(hdev);

	switch (hci_skb_pkt_type(skb)) {
	case HCI_COMMAND_PKT:
		hdev->stat.cmd_tx++;
		break;
	case HCI_ACLDATA_PKT:
		hdev->stat.acl_tx++;
		break;
	case HCI_SCODATA_PKT:
		hdev->stat.sco_tx++;
		break;
	default:
		kfree_skb(skb);
		return -EILSEQ;
	}
	skb_queue_tail(&bt->txq, skb);
	queue_work(bt->wq, &bt->tx_work);
	return 0;
}

static ssize_t pskey_write(struct file *f, struct kobject *kobj,
			   const struct bin_attribute *attr, char *buf,
			   loff_t off, size_t count)
{
	struct btwcn *bt = dev_get_drvdata(kobj_to_dev(kobj));
	int ret;

	if (off || count != PSKEY_SIZE || buf[0] != PSKEY_CMD)
		return -EINVAL;
	mutex_lock(&bt->lock);
	memcpy(bt->pskey, buf, PSKEY_SIZE);
	bt->have_pskey = true;
	ret = 0;
	if (!bt->registered) {
		ret = hci_register_dev(bt->hdev);
		if (ret < 0)
			dev_err(bt->dev, "hci_register_dev: %d\n", ret);
		else
			bt->registered = true;
	}
	mutex_unlock(&bt->lock);
	return ret < 0 ? ret : count;
}
static BIN_ATTR_WO(pskey, PSKEY_SIZE);

static const struct bin_attribute *const btwcn_bin_attrs[] = {
	&bin_attr_pskey,
	NULL
};

static const struct attribute_group btwcn_group = {
	.bin_attrs = btwcn_bin_attrs,
};
__ATTRIBUTE_GROUPS(btwcn);

static int btwcn_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hci_dev *hdev;
	struct btwcn *bt;
	int ret;

	bt = devm_kzalloc(dev, sizeof(*bt), GFP_KERNEL);
	if (!bt)
		return -ENOMEM;
	bt->dev = dev;
	bt->wcn = wcn_get(dev);
	if (!bt->wcn)
		return dev_err_probe(dev, -ENODEV, "no WCN parent\n");
	mutex_init(&bt->lock);
	skb_queue_head_init(&bt->txq);
	INIT_WORK(&bt->rx_work, btwcn_rx_work);
	INIT_WORK(&bt->tx_work, btwcn_tx_work);
	bt->wq = alloc_ordered_workqueue("btwcn", WQ_MEM_RECLAIM);
	if (!bt->wq)
		return -ENOMEM;

	hdev = hci_alloc_dev();
	if (!hdev) {
		ret = -ENOMEM;
		goto err_wq;
	}
	hdev->bus = HCI_SDIO;	/* nearest there is: on-chip, not UART/USB */
	hci_set_drvdata(hdev, bt);
	SET_HCIDEV_DEV(hdev, dev);
	hdev->manufacturer = 0x01ec;
	hdev->open = btwcn_open;
	hdev->close = btwcn_close;
	hdev->flush = btwcn_flush;
	hdev->send = btwcn_send;
	bt->hdev = hdev;

	ret = wcn_register_notify(bt->wcn, WCN_KEY_RING(WCN_RING_BT),
				  btwcn_notify, bt);
	if (ret)
		goto err_hdev;
	platform_set_drvdata(pdev, bt);
	dev_info(dev, "waiting for the PSKEY (%s/pskey)\n", dev_name(dev));
	return 0;

err_hdev:
	hci_free_dev(hdev);
err_wq:
	destroy_workqueue(bt->wq);
	return ret;
}

static void btwcn_remove(struct platform_device *pdev)
{
	struct btwcn *bt = platform_get_drvdata(pdev);

	if (bt->registered)
		hci_unregister_dev(bt->hdev);	/* closes it if it was up */
	wcn_register_notify(bt->wcn, WCN_KEY_RING(WCN_RING_BT), NULL, NULL);
	hci_free_dev(bt->hdev);
	destroy_workqueue(bt->wq);
}

static const struct of_device_id btwcn_of_match[] = {
	{ .compatible = "sprd,sc7731g-bt" },
	{}
};
MODULE_DEVICE_TABLE(of, btwcn_of_match);

static struct platform_driver btwcn_driver = {
	.probe = btwcn_probe,
	.remove = btwcn_remove,
	.driver = {
		.name = "bt-sc7731g",
		.of_match_table = btwcn_of_match,
		.dev_groups = btwcn_groups,
	},
};
module_platform_driver(btwcn_driver);

MODULE_DESCRIPTION("Spreadtrum SC7731G WCN Bluetooth (HCI over SIPC sbuf ring 10)");
MODULE_LICENSE("GPL");
