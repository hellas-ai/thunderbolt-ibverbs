// SPDX-License-Identifier: GPL-2.0
/*
 * apple_discover.c — passive observer for Apple's RDMA-over-Thunderbolt
 *                    xdomain service.
 *
 * Goal: when a macOS 26.2+ Mac connects via USB4/Thunderbolt, capture
 * everything we can observe about its RDMA-over-TB service:
 *
 *   1. xdomain peer metadata (route, link_speed, UUIDs)
 *   2. service properties read from the peer's tb_property dir
 *   3. raw bytes of any frames the peer sends us on the data ring
 *   4. raw bytes of any xdomain-protocol control messages addressed
 *      to Apple's RDMA service UUID
 *
 * What the module does NOT do:
 *   - Speak Apple's wire protocol back. We're a passive observer.
 *     Apple's stack may decline to send us much beyond initial
 *     advertisement / login if we never play along.
 *
 * Apple's RDMA service identity (observed from /sys/bus/thunderbolt
 * on a Mac peer):
 *
 *   key       = \xff\xff\xff\xff\xff\xff\x41\x44   (8 bytes; six 0xff + "AD")
 *   prtcid    = 0xFA57   ("FAST" in leet)
 *   prtcvers  = 1
 *   prtcrevs  = 0
 *   prtcstns  = 0x00000000
 *
 * See APPLE_RDMA_INTEROP.md at the repo root for the full picture.
 */

#define pr_fmt(fmt) "apple_discover: " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/uuid.h>
#include <linux/thunderbolt.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/spinlock.h>
#include <linux/string.h>

#define APPLE_RDMA_PRTCID	0xFA57
#define APPLE_RDMA_PRTCVERS	1
#define APPLE_RDMA_PRTCREVS	0

/* Apple's protocol_key is binary, not ASCII. The 8 bytes are
 * (six 0xff) + "AD". The kernel matches via strcmp(), so no internal
 * NULs are tolerated; ours has none. */
static const char apple_rdma_key[9] = {
	(char)0xff, (char)0xff, (char)0xff, (char)0xff,
	(char)0xff, (char)0xff, 'A', 'D', '\0',
};

/* Apple's RDMA service directory UUID, observed from a macOS 26.3.1 peer. */
static const uuid_t apple_disc_default_service_uuid =
	UUID_INIT(0x49bf223e, 0xd4aa, 0x44d7,
		  0x87, 0x91, 0x50, 0x44, 0x5a, 0xc5, 0x2d, 0x5e);
static uuid_t apple_disc_service_uuid =
	UUID_INIT(0x49bf223e, 0xd4aa, 0x44d7,
		  0x87, 0x91, 0x50, 0x44, 0x5a, 0xc5, 0x2d, 0x5e);

/* Apple also publishes a binary property key (six 0xff) + "CA" = 1. */
static const char apple_rdma_ca_key[9] = {
	(char)0xff, (char)0xff, (char)0xff, (char)0xff,
	(char)0xff, (char)0xff, 'C', 'A', '\0',
};

/* Capture limits — we only care about the FIRST few frames / messages
 * to dissect the format. After we have N captures we stop logging to
 * avoid drowning dmesg in steady-state traffic. */
#define APPLE_DISC_FRAME_SIZE		SZ_4K
#define APPLE_DISC_RING_DEPTH		32
#define APPLE_DISC_FRAMES_TO_POST	16
#define APPLE_DISC_MAX_LOG_FRAMES	32
#define APPLE_DISC_MAX_LOG_BYTES	256	/* per frame, hex dump */
#define APPLE_DISC_MAX_CTRL_MSGS	32
#define APPLE_DISC_MAX_CTRL_BYTES	256

/* SOF/EOF markers worth tolerating on RX. We don't know what Apple
 * uses; accept anything (mask 0xffff = all 16 possible values) so we
 * see whatever they send. */
#define APPLE_DISC_RX_SOF_MASK		0xffff
#define APPLE_DISC_RX_EOF_MASK		0xffff

static bool enable_rx;
module_param(enable_rx, bool, 0444);
MODULE_PARM_DESC(enable_rx,
		 "Allocate an RX ring and post buffers (default: metadata-only)");

static bool enable_paths;
module_param(enable_paths, bool, 0444);
MODULE_PARM_DESC(enable_paths,
		 "Call tb_xdomain_enable_paths() for RX capture (requires enable_rx=1; default: off)");

static bool advertise_service;
module_param(advertise_service, bool, 0444);
MODULE_PARM_DESC(advertise_service,
		 "Advertise a local Apple-compatible AD/FA57 service to the peer (default: off)");

static char *service_uuid;
module_param(service_uuid, charp, 0444);
MODULE_PARM_DESC(service_uuid,
		 "Override advertised AD/FA57 service UUID, e.g. the Mac local port UUID");

static bool apple_vendor_only = true;
module_param(apple_vendor_only, bool, 0444);
MODULE_PARM_DESC(apple_vendor_only,
		 "Bind only peers whose xdomain vendor_name is Apple Inc. (default: on)");

struct apple_disc_frame {
	struct ring_frame frame;
	struct apple_disc_dev *dev;
	void *data;
	dma_addr_t dma;
};

struct apple_disc_dev {
	struct tb_service *svc;
	struct tb_xdomain *xd;

	bool hops_allocated;
	int local_in_hop;
	int local_out_hop;
	struct tb_ring *rx_ring;
	struct apple_disc_frame *rx_frames;
	int rx_frames_count;

	atomic_t frames_logged;
	atomic_t frames_received;

	struct dentry *dir;

	/* Protect logged-frame buffer (debugfs reader vs RX callback). */
	spinlock_t log_lock;
	struct list_head log_list;	/* of apple_disc_log_entry */
};

struct apple_disc_log_entry {
	struct list_head list;
	u32 size;
	u32 flags;
	u8 sof;
	u8 eof;
	u32 dump_len;
	u8 dump[APPLE_DISC_MAX_LOG_BYTES];
};

struct apple_disc_ctrl_log_entry {
	struct list_head list;
	u32 size;
	u32 dump_len;
	u8 dump[APPLE_DISC_MAX_CTRL_BYTES];
};

static struct dentry *apple_disc_root;
static struct tb_property_dir *apple_disc_property_dir;
static struct tb_protocol_handler apple_disc_protocol_handler;

static atomic_t apple_disc_ctrl_logged;
static atomic_t apple_disc_ctrl_received;
static DEFINE_SPINLOCK(apple_disc_ctrl_lock);
static LIST_HEAD(apple_disc_ctrl_list);

static int apple_disc_select_service_uuid(void)
{
	apple_disc_service_uuid = apple_disc_default_service_uuid;

	if (service_uuid && *service_uuid) {
		int ret = uuid_parse(service_uuid, &apple_disc_service_uuid);

		if (ret) {
			pr_err("invalid service_uuid='%s': %d\n",
			       service_uuid, ret);
			return ret;
		}
	}

	return 0;
}

static int apple_disc_register_property_dir(void)
{
	struct tb_property_dir *dir;
	int ret = 0;

	if (!advertise_service)
		return 0;

	dir = tb_property_create_dir(&apple_disc_service_uuid);
	if (!dir)
		return -ENOMEM;

	ret = ret ?: tb_property_add_immediate(dir, "prtcid",
					       APPLE_RDMA_PRTCID);
	ret = ret ?: tb_property_add_immediate(dir, "prtcvers",
					       APPLE_RDMA_PRTCVERS);
	ret = ret ?: tb_property_add_immediate(dir, "prtcrevs",
					       APPLE_RDMA_PRTCREVS);
	ret = ret ?: tb_property_add_immediate(dir, "prtcstns", 0);
	ret = ret ?: tb_property_add_immediate(dir, apple_rdma_ca_key, 1);
	if (ret) {
		tb_property_free_dir(dir);
		return ret;
	}

	ret = tb_register_property_dir(apple_rdma_key, dir);
	if (ret) {
		tb_property_free_dir(dir);
		return ret;
	}

	apple_disc_property_dir = dir;
	pr_info("advertising local Apple-compatible service uuid %pUb\n",
		&apple_disc_service_uuid);
	return 0;
}

static void apple_disc_unregister_property_dir(void)
{
	if (!apple_disc_property_dir)
		return;

	tb_unregister_property_dir(apple_rdma_key, apple_disc_property_dir);
	tb_property_free_dir(apple_disc_property_dir);
	apple_disc_property_dir = NULL;
}

/* ----- one-shot peer metadata dump ------------------------------- */

static void apple_disc_dump_peer(struct tb_service *svc, struct tb_xdomain *xd)
{
	struct tb_property *p;

	pr_info("======== PEER DETECTED (svc=%s) ========\n",
		dev_name(&svc->dev));
	pr_info("  route=0x%llx  link_speed=%u Gb/s\n", xd->route, xd->link_speed);
	pr_info("  remote_uuid=%pUb\n", xd->remote_uuid);
	pr_info("  service prtcid=0x%x  prtcvers=%u  prtcrevs=%u  prtcstns=0x%x\n",
		svc->prtcid, svc->prtcvers, svc->prtcrevs, svc->prtcstns);
	pr_info("  service key (raw 8 bytes): %02x %02x %02x %02x %02x %02x %02x %02x\n",
		(u8)svc->key[0], (u8)svc->key[1], (u8)svc->key[2], (u8)svc->key[3],
		(u8)svc->key[4], (u8)svc->key[5], (u8)svc->key[6], (u8)svc->key[7]);
	pr_info("  device_name='%s'  vendor_name='%s'\n",
		xd->device_name ? xd->device_name : "(null)",
		xd->vendor_name ? xd->vendor_name : "(null)");

	mutex_lock(&xd->lock);
	p = tb_property_find(xd->remote_properties, svc->key,
			     TB_PROPERTY_TYPE_DIRECTORY);
	if (p && p->value.dir) {
		struct tb_property_dir *dir = p->value.dir;
		struct tb_property *child;

		if (dir->uuid)
			pr_info("  service dir uuid=%pUb\n", dir->uuid);
		else
			pr_info("  service dir uuid=(null)\n");

		tb_property_for_each(dir, child) {
			if (child->type == TB_PROPERTY_TYPE_VALUE) {
				pr_info("  prop %-8s = 0x%x\n", child->key,
					child->value.immediate);
			} else {
				pr_info("  prop %-8s type=0x%x len=%zu\n",
					child->key, child->type, child->length);
			}
		}
	} else {
		pr_info("  service property directory not found\n");
	}
	mutex_unlock(&xd->lock);
}

/* ----- RX log readers ------------------------------------------- */

static int apple_disc_log_show(struct seq_file *m, void *unused)
{
	struct apple_disc_dev *dev = m->private;
	struct apple_disc_log_entry *e;
	unsigned long flags;
	int i = 0;

	seq_printf(m, "captures: %d (received total: %d)\n",
		   atomic_read(&dev->frames_logged),
		   atomic_read(&dev->frames_received));
	seq_puts(m, "\n");

	spin_lock_irqsave(&dev->log_lock, flags);
	list_for_each_entry(e, &dev->log_list, list) {
		seq_printf(m, "==== frame[%d] size=%u flags=0x%03x sof=%u eof=%u  (showing first %u bytes) ====\n",
			   i++, e->size, e->flags, e->sof, e->eof, e->dump_len);
		seq_hex_dump(m, "  ", DUMP_PREFIX_OFFSET, 16, 1,
			     e->dump, e->dump_len, true);
		seq_puts(m, "\n");
	}
	spin_unlock_irqrestore(&dev->log_lock, flags);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apple_disc_log);

static int apple_disc_ctrl_log_show(struct seq_file *m, void *unused)
{
	struct apple_disc_ctrl_log_entry *e;
	unsigned long flags;
	int i = 0;

	seq_printf(m, "control messages logged: %d (received total: %d)\n",
		   atomic_read(&apple_disc_ctrl_logged),
		   atomic_read(&apple_disc_ctrl_received));
	seq_puts(m, "\n");

	spin_lock_irqsave(&apple_disc_ctrl_lock, flags);
	list_for_each_entry(e, &apple_disc_ctrl_list, list) {
		seq_printf(m, "==== ctrl[%d] size=%u (showing first %u bytes) ====\n",
			   i++, e->size, e->dump_len);
		seq_hex_dump(m, "  ", DUMP_PREFIX_OFFSET, 16, 1,
			     e->dump, e->dump_len, true);
		seq_puts(m, "\n");
	}
	spin_unlock_irqrestore(&apple_disc_ctrl_lock, flags);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apple_disc_ctrl_log);

static int apple_disc_ctrl_callback(const void *buf, size_t size, void *data)
{
	int logged;

	atomic_inc(&apple_disc_ctrl_received);

	logged = atomic_read(&apple_disc_ctrl_logged);
	if (logged < APPLE_DISC_MAX_CTRL_MSGS) {
		struct apple_disc_ctrl_log_entry *e;

		e = kmalloc(sizeof(*e), GFP_ATOMIC);
		if (e) {
			unsigned long flags;

			e->size = size;
			e->dump_len = min_t(u32, size, APPLE_DISC_MAX_CTRL_BYTES);
			memcpy(e->dump, buf, e->dump_len);

			spin_lock_irqsave(&apple_disc_ctrl_lock, flags);
			list_add_tail(&e->list, &apple_disc_ctrl_list);
			spin_unlock_irqrestore(&apple_disc_ctrl_lock, flags);

			atomic_inc(&apple_disc_ctrl_logged);
			pr_info("CTRL[%d]: size=%zu for local Apple service UUID\n",
				logged, size);
		}
	}

	/* This UUID is ours when advertise_service=1, so consume the packet.
	 * We intentionally do not respond yet; Phase 1b will add the minimal
	 * fake-peer responses once the request format is known.
	 */
	return 1;
}

/* ----- RX path -------------------------------------------------- */

static void apple_disc_rx_callback(struct tb_ring *ring,
				   struct ring_frame *frame, bool canceled)
{
	struct apple_disc_frame *df = container_of(frame, typeof(*df), frame);
	struct apple_disc_dev *dev = df->dev;
	int logged;
	int ret;

	atomic_inc(&dev->frames_received);

	if (canceled) {
		pr_info_ratelimited("RX canceled\n");
		return;
	}

	logged = atomic_read(&dev->frames_logged);
	if (logged < APPLE_DISC_MAX_LOG_FRAMES) {
		struct apple_disc_log_entry *e =
			kmalloc(sizeof(*e), GFP_ATOMIC);

		if (e) {
			u32 size = frame->size ? frame->size :
						 APPLE_DISC_FRAME_SIZE;
			u32 dump_len = min_t(u32, size, APPLE_DISC_MAX_LOG_BYTES);
			unsigned long flags;

			e->size = size;
			e->flags = frame->flags;
			e->sof = frame->sof;
			e->eof = frame->eof;
			e->dump_len = dump_len;
			memcpy(e->dump, df->data, dump_len);

			spin_lock_irqsave(&dev->log_lock, flags);
			list_add_tail(&e->list, &dev->log_list);
			spin_unlock_irqrestore(&dev->log_lock, flags);

			atomic_inc(&dev->frames_logged);
			pr_info("RX[%d]: size=%u flags=0x%03x sof=%u eof=%u\n",
				logged, size, frame->flags, frame->sof, frame->eof);
		}
	}

	/* Re-post the buffer so the controller has somewhere to write
	 * the next frame. We're a passive observer — keep listening. */
	ret = tb_ring_rx(ring, frame);
	if (ret)
		pr_warn_ratelimited("tb_ring_rx repost failed: %d\n", ret);
}

/* ----- ring lifecycle ------------------------------------------- */

static int apple_disc_alloc_rx_frames(struct apple_disc_dev *dev)
{
	struct device *dma_dev = tb_ring_dma_device(dev->rx_ring);
	int i;

	dev->rx_frames = kcalloc(APPLE_DISC_FRAMES_TO_POST,
				 sizeof(*dev->rx_frames), GFP_KERNEL);
	if (!dev->rx_frames)
		return -ENOMEM;

	for (i = 0; i < APPLE_DISC_FRAMES_TO_POST; i++) {
		struct apple_disc_frame *df = &dev->rx_frames[i];

		df->dev = dev;
		df->data = kmalloc(APPLE_DISC_FRAME_SIZE, GFP_KERNEL);
		if (!df->data)
			goto err;
		df->dma = dma_map_single(dma_dev, df->data,
					 APPLE_DISC_FRAME_SIZE,
					 DMA_FROM_DEVICE);
		if (dma_mapping_error(dma_dev, df->dma)) {
			kfree(df->data);
			df->data = NULL;
			goto err;
		}
		df->frame.buffer_phy = df->dma;
		/* ring_frame.size is 12 bits; size 0 encodes the ring's full
		 * 4 KiB frame size in FRAME mode.
		 */
		df->frame.size = 0;
		df->frame.callback = apple_disc_rx_callback;
		INIT_LIST_HEAD(&df->frame.list);
	}
	dev->rx_frames_count = APPLE_DISC_FRAMES_TO_POST;
	return 0;

err:
	while (--i >= 0) {
		struct apple_disc_frame *df = &dev->rx_frames[i];

		if (df->data) {
			dma_unmap_single(dma_dev, df->dma,
					 APPLE_DISC_FRAME_SIZE, DMA_FROM_DEVICE);
			kfree(df->data);
		}
	}
	kfree(dev->rx_frames);
	dev->rx_frames = NULL;
	return -ENOMEM;
}

static void apple_disc_free_rx_frames(struct apple_disc_dev *dev)
{
	struct device *dma_dev;
	int i;

	if (!dev->rx_frames)
		return;
	dma_dev = tb_ring_dma_device(dev->rx_ring);
	for (i = 0; i < dev->rx_frames_count; i++) {
		struct apple_disc_frame *df = &dev->rx_frames[i];

		if (df->data) {
			dma_unmap_single(dma_dev, df->dma,
					 APPLE_DISC_FRAME_SIZE, DMA_FROM_DEVICE);
			kfree(df->data);
		}
	}
	kfree(dev->rx_frames);
	dev->rx_frames = NULL;
	dev->rx_frames_count = 0;
}

static int apple_disc_setup_ring(struct apple_disc_dev *dev)
{
	struct tb_xdomain *xd = dev->xd;
	int in_hop, out_hop;
	int ret, i;

	in_hop = tb_xdomain_alloc_in_hopid(xd, -1);
	if (in_hop < 0) {
		pr_warn("alloc_in_hopid failed: %d\n", in_hop);
		return in_hop;
	}
	out_hop = tb_xdomain_alloc_out_hopid(xd, -1);
	if (out_hop < 0) {
		tb_xdomain_release_in_hopid(xd, in_hop);
		pr_warn("alloc_out_hopid failed: %d\n", out_hop);
		return out_hop;
	}
	dev->local_in_hop = in_hop;
	dev->local_out_hop = out_hop;
	dev->hops_allocated = true;
	pr_info("allocated hops: in=%d out=%d\n", in_hop, out_hop);

	dev->rx_ring = tb_ring_alloc_rx(xd->tb->nhi, -1, APPLE_DISC_RING_DEPTH,
					RING_FLAG_FRAME,
					0,	/* no e2e tx hop */
					APPLE_DISC_RX_SOF_MASK,
					APPLE_DISC_RX_EOF_MASK,
					NULL, NULL);
	if (!dev->rx_ring) {
		ret = -ENOMEM;
		goto err_hopid;
	}
	pr_info("rx_ring->hop=%d\n", dev->rx_ring->hop);

	ret = apple_disc_alloc_rx_frames(dev);
	if (ret)
		goto err_ring;

	tb_ring_start(dev->rx_ring);

	for (i = 0; i < dev->rx_frames_count; i++) {
		struct apple_disc_frame *df = &dev->rx_frames[i];

		ret = tb_ring_rx(dev->rx_ring, &df->frame);
		if (ret) {
			pr_warn("tb_ring_rx slot %d: %d\n", i, ret);
			goto err_started;
		}
	}

	if (enable_paths) {
		/* Enable paths so the controller starts forwarding frames
		 * addressed to our in_hop. This is experimental because we do
		 * not know Apple's TX hop yet.
		 */
		ret = tb_xdomain_enable_paths(xd, dev->local_out_hop,
					      dev->rx_ring->hop,
					      dev->local_in_hop,
					      dev->rx_ring->hop);
		if (ret) {
			pr_warn("enable_paths failed: %d (Apple peer may not have agreed on hop assignment yet)\n",
				ret);
			/* Continue anyway — we'll still see anything that
			 * ARRIVES. Without enable_paths the controller may not
			 * route to us, but this is the safer first capture mode.
			 */
		}
	} else {
		pr_info("enable_paths=0: RX ring posted, but no xdomain paths were programmed\n");
	}

	return 0;

err_started:
	tb_ring_stop(dev->rx_ring);
	apple_disc_free_rx_frames(dev);
err_ring:
	tb_ring_free(dev->rx_ring);
	dev->rx_ring = NULL;
err_hopid:
	tb_xdomain_release_out_hopid(xd, out_hop);
	tb_xdomain_release_in_hopid(xd, in_hop);
	dev->hops_allocated = false;
	return ret;
}

static void apple_disc_teardown_ring(struct apple_disc_dev *dev)
{
	if (dev->rx_ring) {
		if (enable_paths)
			tb_xdomain_disable_paths(dev->xd, dev->local_out_hop,
						 dev->rx_ring->hop,
						 dev->local_in_hop,
						 dev->rx_ring->hop);
		tb_ring_stop(dev->rx_ring);
		apple_disc_free_rx_frames(dev);
		tb_ring_free(dev->rx_ring);
		dev->rx_ring = NULL;
	}
	if (dev->hops_allocated) {
		tb_xdomain_release_in_hopid(dev->xd, dev->local_in_hop);
		tb_xdomain_release_out_hopid(dev->xd, dev->local_out_hop);
		dev->hops_allocated = false;
	}
}

/* ----- service driver probe / remove ---------------------------- */

static int apple_disc_probe(struct tb_service *svc,
			    const struct tb_service_id *id)
{
	struct tb_xdomain *xd = tb_service_parent(svc);
	struct apple_disc_dev *dev;
	int ret;

	if (!xd)
		return -ENODEV;

	if (apple_vendor_only &&
	    (!xd->vendor_name || strcmp(xd->vendor_name, "Apple Inc."))) {
		dev_info(&svc->dev,
			 "skipping non-Apple AD/FA57 peer: device_name='%s' vendor_name='%s'\n",
			 xd->device_name ? xd->device_name : "(null)",
			 xd->vendor_name ? xd->vendor_name : "(null)");
		return -ENODEV;
	}

	dev = devm_kzalloc(&svc->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->svc = svc;
	dev->xd = xd;
	atomic_set(&dev->frames_logged, 0);
	atomic_set(&dev->frames_received, 0);
	spin_lock_init(&dev->log_lock);
	INIT_LIST_HEAD(&dev->log_list);

	apple_disc_dump_peer(svc, xd);

	if (enable_rx) {
		ret = apple_disc_setup_ring(dev);
		if (ret) {
			pr_warn("ring setup failed: %d (peer is still bound; control-plane only)\n",
				ret);
			/* Even without a ring we want to keep the binding so
			 * we get the dmesg dump above. */
		}
	} else {
		pr_info("enable_rx=0: metadata-only bind; not allocating rings or programming paths\n");
	}

	dev->dir = debugfs_create_dir(dev_name(&svc->dev), apple_disc_root);
	if (!IS_ERR_OR_NULL(dev->dir))
		debugfs_create_file("rx_log", 0444, dev->dir, dev,
				    &apple_disc_log_fops);

	tb_service_set_drvdata(svc, dev);
	dev_info(&svc->dev,
		 "Apple RDMA-over-TB peer bound. Watch dmesg + debugfs %s/rx_log for capture.\n",
		 dev->dir ? dev_name(&svc->dev) : "(no debugfs)");
	return 0;
}

static void apple_disc_remove(struct tb_service *svc)
{
	struct apple_disc_dev *dev = tb_service_get_drvdata(svc);
	struct apple_disc_log_entry *e, *tmp;
	unsigned long flags;

	dev_info(&svc->dev,
		 "Apple peer detached: received=%d logged=%d\n",
		 dev ? atomic_read(&dev->frames_received) : 0,
		 dev ? atomic_read(&dev->frames_logged) : 0);
	if (!dev)
		return;

	apple_disc_teardown_ring(dev);
	debugfs_remove_recursive(dev->dir);

	spin_lock_irqsave(&dev->log_lock, flags);
	list_for_each_entry_safe(e, tmp, &dev->log_list, list) {
		list_del(&e->list);
		kfree(e);
	}
	spin_unlock_irqrestore(&dev->log_lock, flags);

	tb_service_set_drvdata(svc, NULL);
}

/* ----- module init / exit --------------------------------------- */

static const struct tb_service_id apple_disc_ids[] = {
	{
		.match_flags = TBSVC_MATCH_PROTOCOL_KEY |
			       TBSVC_MATCH_PROTOCOL_ID |
			       TBSVC_MATCH_PROTOCOL_VERSION |
			       TBSVC_MATCH_PROTOCOL_REVISION,
		.protocol_key = {
			(char)0xff, (char)0xff, (char)0xff, (char)0xff,
			(char)0xff, (char)0xff, 'A', 'D', '\0',
		},
		.protocol_id       = APPLE_RDMA_PRTCID,
		.protocol_version  = APPLE_RDMA_PRTCVERS,
		.protocol_revision = APPLE_RDMA_PRTCREVS,
	},
	{ },
};
MODULE_DEVICE_TABLE(tbsvc, apple_disc_ids);

static struct tb_service_driver apple_disc_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name  = "apple_rdma_discover",
	},
	.probe   = apple_disc_probe,
	.remove  = apple_disc_remove,
	.id_table = apple_disc_ids,
};

static int __init apple_disc_init(void)
{
	int err;

	apple_disc_root = debugfs_create_dir("apple_rdma_discover", NULL);
	if (IS_ERR(apple_disc_root))
		apple_disc_root = NULL;
	if (apple_disc_root)
		debugfs_create_file("ctrl_log", 0444, apple_disc_root, NULL,
				    &apple_disc_ctrl_log_fops);

	err = apple_disc_select_service_uuid();
	if (err) {
		debugfs_remove_recursive(apple_disc_root);
		return err;
	}

	err = apple_disc_register_property_dir();
	if (err) {
		debugfs_remove_recursive(apple_disc_root);
		return err;
	}

	apple_disc_protocol_handler.uuid = &apple_disc_service_uuid;
	apple_disc_protocol_handler.callback = apple_disc_ctrl_callback;
	INIT_LIST_HEAD(&apple_disc_protocol_handler.list);
	err = tb_register_protocol_handler(&apple_disc_protocol_handler);
	if (err) {
		apple_disc_unregister_property_dir();
		debugfs_remove_recursive(apple_disc_root);
		return err;
	}

	err = tb_register_service_driver(&apple_disc_driver);
	if (err) {
		tb_unregister_protocol_handler(&apple_disc_protocol_handler);
		apple_disc_unregister_property_dir();
		debugfs_remove_recursive(apple_disc_root);
		return err;
	}

	pr_info("registered: matching prtcid=0x%x prtcvers=%u prtcrevs=%u key='\\xff*6 + AD'\n",
		APPLE_RDMA_PRTCID, APPLE_RDMA_PRTCVERS, APPLE_RDMA_PRTCREVS);
	pr_info("connect a macOS 26.2+ Mac with TB5 to see what it sends.\n");
	return 0;
}

static void __exit apple_disc_exit(void)
{
	struct apple_disc_ctrl_log_entry *e, *tmp;
	unsigned long flags;

	tb_unregister_service_driver(&apple_disc_driver);
	tb_unregister_protocol_handler(&apple_disc_protocol_handler);
	apple_disc_unregister_property_dir();
	debugfs_remove_recursive(apple_disc_root);

	spin_lock_irqsave(&apple_disc_ctrl_lock, flags);
	list_for_each_entry_safe(e, tmp, &apple_disc_ctrl_list, list) {
		list_del(&e->list);
		kfree(e);
	}
	spin_unlock_irqrestore(&apple_disc_ctrl_lock, flags);

	pr_info("unloaded\n");
}

module_init(apple_disc_init);
module_exit(apple_disc_exit);

MODULE_AUTHOR("usb4-rdma project");
MODULE_DESCRIPTION("Passive observer for Apple's RDMA-over-Thunderbolt service");
MODULE_LICENSE("GPL v2");
