// SPDX-License-Identifier: GPL-2.0
/*
 * tbnetmq — multi-queue thunderbolt-net (skeleton).
 *
 * Status: this file currently registers a tb_service under key "netmq"
 * so it shows up on the xdomain bus alongside the stock "network"
 * binding, but does NOT yet allocate rings or expose a netdev. See
 * README.md for the design and remaining work.
 *
 * Build: this is intentionally NOT yet linked into the main usb4_rdma
 * module. Once it has actual functionality it will get its own
 * standalone Makefile target.
 */

#define pr_fmt(fmt) "tbnetmq: " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/uuid.h>
#include <linux/thunderbolt.h>

#define TBNETMQ_PROTO_KEY  "netmq"
#define TBNETMQ_PROTO_ID   1

/* Distinct UUID from stock thunderbolt-net (which uses
 * c66189ca-1cce-4195-bdb8-49592e5f5a4f). uuidgen -r once, frozen. */
static const uuid_t tbnetmq_uuid =
	UUID_INIT(0xc8b3a4d0, 0x9f6e, 0x4cb2,
		  0x8d, 0x5a, 0x71, 0xe0, 0x4f, 0x2c, 0x95, 0x3a);

static struct tb_property_dir *tbnetmq_dir;

/* TODO(everything below):
 *   - tbnetmq_probe: alloc rings, alloc skb pools, register netdev
 *   - tbnetmq_remove: tear down
 *   - tbnetmq_open / _stop: ring lifecycle
 *   - tbnetmq_xmit: per-queue dispatch
 *   - per-queue NAPI poll
 *   - login extension for queue count / hop negotiation
 */

static int tbnetmq_probe(struct tb_service *svc,
			 const struct tb_service_id *id)
{
	struct tb_xdomain *xd = tb_service_parent(svc);

	dev_info(&svc->dev,
		 "tbnetmq: probe — peer route 0x%llx (skeleton, no data path yet)\n",
		 xd ? xd->route : 0ULL);
	return 0;
}

static void tbnetmq_remove(struct tb_service *svc)
{
	dev_info(&svc->dev, "tbnetmq: remove\n");
}

static const struct tb_service_id tbnetmq_ids[] = {
	{ TB_SERVICE(TBNETMQ_PROTO_KEY, TBNETMQ_PROTO_ID) },
	{ },
};
MODULE_DEVICE_TABLE(tbsvc, tbnetmq_ids);

static struct tb_service_driver tbnetmq_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name  = "tbnetmq",
	},
	.probe   = tbnetmq_probe,
	.remove  = tbnetmq_remove,
	.id_table = tbnetmq_ids,
};

static int __init tbnetmq_init(void)
{
	int err;

	tbnetmq_dir = tb_property_create_dir(&tbnetmq_uuid);
	if (!tbnetmq_dir)
		return -ENOMEM;
	tb_property_add_immediate(tbnetmq_dir, "prtcid", TBNETMQ_PROTO_ID);
	tb_property_add_immediate(tbnetmq_dir, "prtcvers", 1);
	tb_property_add_immediate(tbnetmq_dir, "prtcrevs", 1);
	tb_property_add_immediate(tbnetmq_dir, "prtcstns", 0);

	err = tb_register_property_dir(TBNETMQ_PROTO_KEY, tbnetmq_dir);
	if (err) {
		tb_property_free_dir(tbnetmq_dir);
		return err;
	}

	err = tb_register_service_driver(&tbnetmq_driver);
	if (err) {
		tb_unregister_property_dir(TBNETMQ_PROTO_KEY, tbnetmq_dir);
		tb_property_free_dir(tbnetmq_dir);
		return err;
	}

	pr_info("registered, key=%s (skeleton)\n", TBNETMQ_PROTO_KEY);
	return 0;
}

static void __exit tbnetmq_exit(void)
{
	tb_unregister_service_driver(&tbnetmq_driver);
	tb_unregister_property_dir(TBNETMQ_PROTO_KEY, tbnetmq_dir);
	tb_property_free_dir(tbnetmq_dir);
}

module_init(tbnetmq_init);
module_exit(tbnetmq_exit);

MODULE_AUTHOR("usb4-rdma project");
MODULE_DESCRIPTION("Multi-queue thunderbolt-net (skeleton)");
MODULE_LICENSE("GPL v2");
