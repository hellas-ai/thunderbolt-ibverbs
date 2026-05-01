// SPDX-License-Identifier: GPL-2.0
/*
 * usb4_rdma — soft-RDMA over USB4 host-to-host xdomain (skeleton).
 *
 * Goal: provide an `ib_device` whose data path runs over `tb_xdomain`
 * rings, so any libibverbs userspace (RCCL, MPI, EXO/Jaccl, perftest)
 * works between two USB4-connected Linux machines without a hardware
 * RDMA NIC.
 *
 * This file is the bring-up skeleton:
 *   1. Register a `tb_service_driver` claiming a unique UUID.
 *   2. Advertise that UUID via `tb_register_property_dir` so peers
 *      discover us during xdomain property exchange.
 *   3. On probe(), log peer info and (TODO) allocate TX+RX rings.
 *   4. (TODO) Register the verbs surface as `ib_device`.
 *
 * The skeleton intentionally does *not* yet:
 *   - allocate or use `tb_ring`s
 *   - implement any wire protocol
 *   - register an `ib_device`
 *
 * Each of those is its own pull request.
 *
 * Tested against kernel 7.0.x (NixOS) on AMD Strix Halo USB4 host
 * routers (PCI 1022:158d / 1022:158e).
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/uuid.h>
#include <linux/thunderbolt.h>
#include <linux/debugfs.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>

#include "usb4_rdma.h"

#define DRV_NAME    "usb4_rdma"

/* Protocol key as advertised on the xdomain bus. Constrained to ≤ 8
 * chars by tb_register_property_dir(); both peers must match. */
#define USB4_RDMA_PROTO_KEY "usb4rdma"
#define DRV_VERSION "0.0.1"

/*
 * Service UUID: peers running this driver must agree on it. Generated
 * with `uuidgen -r` once and frozen — changing it breaks compatibility.
 *
 *     7c2c8f1e-5b4d-4a01-9f3a-2b8e6d4c1a07
 */
static const uuid_t usb4_rdma_uuid =
	UUID_INIT(0x7c2c8f1e, 0x5b4d, 0x4a01,
		  0x9f, 0x3a, 0x2b, 0x8e, 0x6d, 0x4c, 0x1a, 0x07);

/* Protocol revision advertised in the property directory. Bump when
 * the wire format changes. */
#define USB4_RDMA_PROTOCOL_REV 1

/*
 * Per-bound-service state. One of these per discovered peer.
 *
 * In a real driver this would also carry:
 *   - struct ib_device *ibdev
 *   - struct tb_ring *tx_ring, *rx_ring
 *   - QP table, MR table, CQ table
 *   - busy-poll worker
 */
struct usb4_rdma_dev {
	struct tb_service *service;
	struct dentry *debugfs_dir;
	atomic64_t probe_jiffies;
	atomic_t state;
};

enum {
	USB4_RDMA_STATE_INIT = 0,
	USB4_RDMA_STATE_PROBED,
	USB4_RDMA_STATE_RUNNING,
	USB4_RDMA_STATE_REMOVED,
};

static struct dentry *usb4_rdma_debugfs_root;

/* ----- debugfs ---------------------------------------------------- */

static int usb4_rdma_state_show(struct seq_file *m, void *v)
{
	struct usb4_rdma_dev *dev = m->private;
	const char *state_str;

	switch (atomic_read(&dev->state)) {
	case USB4_RDMA_STATE_INIT:    state_str = "init"; break;
	case USB4_RDMA_STATE_PROBED:  state_str = "probed"; break;
	case USB4_RDMA_STATE_RUNNING: state_str = "running"; break;
	case USB4_RDMA_STATE_REMOVED: state_str = "removed"; break;
	default: state_str = "unknown"; break;
	}

	seq_printf(m, "state:           %s\n", state_str);
	seq_printf(m, "probe_jiffies:   %lld\n",
		   (long long)atomic64_read(&dev->probe_jiffies));
	seq_printf(m, "service_key:     %s\n",
		   dev->service ? dev->service->key : "(none)");
	seq_printf(m, "protocol_rev:    %u\n", USB4_RDMA_PROTOCOL_REV);
	return 0;
}

static int usb4_rdma_state_open(struct inode *inode, struct file *file)
{
	return single_open(file, usb4_rdma_state_show, inode->i_private);
}

static const struct file_operations usb4_rdma_state_fops = {
	.owner   = THIS_MODULE,
	.open    = usb4_rdma_state_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* ----- service driver --------------------------------------------- */

static int usb4_rdma_probe(struct tb_service *svc, const struct tb_service_id *id)
{
	struct usb4_rdma_dev *dev;
	struct tb_xdomain *xd = tb_service_parent(svc);

	dev_info(&svc->dev,
		 "usb4_rdma: probe — service %s, route 0x%llx, link_speed=%u Gb/s%s\n",
		 svc->key, xd ? xd->route : 0ULL,
		 xd ? xd->link_speed : 0,
		 (xd && xd->link_usb4) ? " (USB4)" : "");

	dev = devm_kzalloc(&svc->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->service = svc;
	atomic_set(&dev->state, USB4_RDMA_STATE_PROBED);
	atomic64_set(&dev->probe_jiffies, jiffies);

	dev->debugfs_dir = debugfs_create_dir(dev_name(&svc->dev),
					      usb4_rdma_debugfs_root);
	if (!IS_ERR_OR_NULL(dev->debugfs_dir)) {
		debugfs_create_file("state", 0444, dev->debugfs_dir, dev,
				    &usb4_rdma_state_fops);
	}

	tb_service_set_drvdata(svc, dev);

	/*
	 * TODO(tier-1):
	 *   - tb_ring_alloc_tx(svc->parent->parent, hop=?, RING_SIZE, RING_FLAG_E2E)
	 *   - tb_ring_alloc_rx(... callback for incoming frames ...)
	 *   - tb_ring_start() on both
	 *   - register an ib_device with verbs callbacks (alloc_pd, reg_user_mr,
	 *     create_qp, post_send, post_recv, poll_cq, req_notify_cq, ...)
	 *   - hook post_send → wire-frame builder → ring_tx
	 *   - hook ring_rx callback → completion event
	 */
	atomic_set(&dev->state, USB4_RDMA_STATE_RUNNING);
	return 0;
}

static void usb4_rdma_remove(struct tb_service *svc)
{
	struct usb4_rdma_dev *dev = tb_service_get_drvdata(svc);

	dev_info(&svc->dev, "usb4_rdma: remove\n");

	if (!dev)
		return;

	atomic_set(&dev->state, USB4_RDMA_STATE_REMOVED);

	/*
	 * TODO(tier-1):
	 *   - ib_unregister_device()
	 *   - tb_ring_stop() + tb_ring_free() for both rings
	 *   - drain any pending CQEs / async events
	 */

	debugfs_remove_recursive(dev->debugfs_dir);
	tb_service_set_drvdata(svc, NULL);
}

static const struct tb_service_id usb4_rdma_ids[] = {
	{ TB_SERVICE(USB4_RDMA_PROTO_KEY, 1) },
	{ },
};
MODULE_DEVICE_TABLE(tbsvc, usb4_rdma_ids);

static struct tb_service_driver usb4_rdma_driver = {
	.driver = {
		.owner   = THIS_MODULE,
		.name    = DRV_NAME,
	},
	.probe   = usb4_rdma_probe,
	.remove  = usb4_rdma_remove,
	.id_table = usb4_rdma_ids,
};

/* ----- property directory advertisement --------------------------- */
/*
 * Both peers must register a property dir under the same key+UUID for
 * xdomain discovery to bind them. We use:
 *   key    = "usb4rdma"
 *   prtcid = 1
 *   prtcvers = USB4_RDMA_PROTOCOL_REV
 *   prtcrevs = 1
 *   prtcstns = 0  (no special connection states yet)
 */
static struct tb_property_dir *usb4_rdma_xdomain_property_dir;

static int usb4_rdma_build_property_dir(void)
{
	struct tb_property_dir *dir;
	int err = 0;

	dir = tb_property_create_dir(&usb4_rdma_uuid);
	if (!dir)
		return -ENOMEM;

	err = err ?: tb_property_add_immediate(dir, "prtcid", 1);
	err = err ?: tb_property_add_immediate(dir, "prtcvers", USB4_RDMA_PROTOCOL_REV);
	err = err ?: tb_property_add_immediate(dir, "prtcrevs", 1);
	err = err ?: tb_property_add_immediate(dir, "prtcstns", 0);
	err = err ?: tb_property_add_text(dir, "deviceid", DRV_NAME);
	if (err) {
		tb_property_free_dir(dir);
		return err;
	}

	usb4_rdma_xdomain_property_dir = dir;
	return tb_register_property_dir(USB4_RDMA_PROTO_KEY, dir);
}

static void usb4_rdma_destroy_property_dir(void)
{
	if (usb4_rdma_xdomain_property_dir) {
		tb_unregister_property_dir(USB4_RDMA_PROTO_KEY,
					   usb4_rdma_xdomain_property_dir);
		tb_property_free_dir(usb4_rdma_xdomain_property_dir);
		usb4_rdma_xdomain_property_dir = NULL;
	}
}

/* ----- module init / exit ----------------------------------------- */

static int __init usb4_rdma_init(void)
{
	int err;

	pr_info("%s %s loading\n", DRV_NAME, DRV_VERSION);

	usb4_rdma_debugfs_root = debugfs_create_dir(DRV_NAME, NULL);
	if (IS_ERR(usb4_rdma_debugfs_root)) {
		pr_warn("debugfs unavailable (%ld), continuing without\n",
			PTR_ERR(usb4_rdma_debugfs_root));
		usb4_rdma_debugfs_root = NULL;
	}

	err = usb4_rdma_build_property_dir();
	if (err) {
		pr_err("failed to register property dir: %d\n", err);
		goto err_debugfs;
	}

	err = tb_register_service_driver(&usb4_rdma_driver);
	if (err) {
		pr_err("failed to register service driver: %d\n", err);
		goto err_propdir;
	}

	/* PCI BAR explorer — best-effort, doesn't fail module load. */
	if (usb4_rdma_pci_init(usb4_rdma_debugfs_root))
		pr_warn("PCI BAR explorer init failed; continuing without it\n");

	/* Multi-ring xdomain loadtest — best-effort. */
	if (usb4_rdma_loadtest_init(usb4_rdma_debugfs_root))
		pr_warn("loadtest init failed; continuing without it\n");

	/* RDMA ib_device skeleton — best-effort. */
	if (usb4_rdma_ibdev_init())
		pr_warn("ib_device init failed; continuing without it\n");

	pr_info("%s ready, advertising service uuid %pUb\n",
		DRV_NAME, &usb4_rdma_uuid);
	return 0;

err_propdir:
	usb4_rdma_destroy_property_dir();
err_debugfs:
	debugfs_remove_recursive(usb4_rdma_debugfs_root);
	return err;
}

static void __exit usb4_rdma_exit(void)
{
	pr_info("%s unloading\n", DRV_NAME);
	usb4_rdma_ibdev_exit();
	usb4_rdma_loadtest_exit();
	usb4_rdma_pci_exit();
	tb_unregister_service_driver(&usb4_rdma_driver);
	usb4_rdma_destroy_property_dir();
	debugfs_remove_recursive(usb4_rdma_debugfs_root);
}

module_init(usb4_rdma_init);
module_exit(usb4_rdma_exit);

MODULE_AUTHOR("usb4-rdma project");
MODULE_DESCRIPTION("Soft-RDMA over USB4 host-to-host xdomain (skeleton)");
MODULE_VERSION(DRV_VERSION);
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("tbsvc:kusb4rdmap00000001");
