// SPDX-License-Identifier: GPL-2.0
/*
 * loadtest.c — multi-ring xdomain throughput probe.
 *
 * The question: when we open N parallel `tb_ring` pairs on the same
 * USB4 host router, does aggregate throughput scale linearly with N,
 * or is there a shared internal bottleneck (DMA engine, memory port,
 * scheduler) that caps the controller below N × per-ring rate?
 *
 * Answering this gates the bandwidth thesis behind the soft-RDMA
 * project. If aggregate scales to ~5 × 10 Gbps = ~50 Gbps single-cable
 * before hitting the 40 Gbps PHY ceiling, multi-ring is the right
 * lever to pull. If it plateaus at the ~10 Gbps single-ring rate seen
 * by `thunderbolt_net` regardless of N, the strategy needs a rethink.
 *
 * Approach:
 *   1. Register a `tb_service` under a separate key ("u4lt") so we
 *      don't conflict with thunderbolt_net's "network".
 *   2. On probe (peer discovered), allocate N TX+RX ring pairs at
 *      fixed hops (LOADTEST_HOP_BASE..LOADTEST_HOP_BASE+N-1). Stays
 *      out of thunderbolt_net's hops 0/1.
 *   3. Use tb_xdomain_enable_paths() to wire the routing.
 *   4. Pre-post RX frames; spawn one TX kthread per ring.
 *   5. Frames carry no protocol — just 4 KB of zeros, SOF/EOF
 *      markers per `dma_test`.
 *   6. RX callback re-posts the frame; TX callback marks slot free.
 *   7. Aggregate counters in debugfs:
 *        /sys/kernel/debug/usb4_rdma/loadtest/<peer>/stats
 *        /sys/kernel/debug/usb4_rdma/loadtest/<peer>/start
 *        /sys/kernel/debug/usb4_rdma/loadtest/<peer>/stop
 *
 * Both peers must run this module. Hop allocation is symmetric — both
 * sides claim hops [BASE..BASE+N-1] and both call enable_paths with
 * the same arguments.
 */

#define pr_fmt(fmt) "usb4_rdma/loadtest: " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/uuid.h>
#include <linux/thunderbolt.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/kthread.h>
#include <linux/atomic.h>
#include <linux/ktime.h>
#include <linux/delay.h>

#include "usb4_rdma.h"

#define LOADTEST_PROTO_KEY        "u4lt"
#define LOADTEST_PROTO_ID         1

/* Hops are auto-allocated by the xdomain layer (must be >= TB_PATH_MIN_HOPID
 * which is 8 for non-NHI paths). The "hops 0..6" we observed in BAR0 register
 * slots are controller ring indices, not xdomain hop IDs — different concept. */
#define LOADTEST_MAX_RINGS        5

/* SOF/EOF markers (PDFs) — borrowed from dma_test layout. */
#define LOADTEST_PDF_FRAME_START  1
#define LOADTEST_PDF_FRAME_END    2

#define LOADTEST_FRAME_SIZE       SZ_4K
#define LOADTEST_RING_DEPTH       64
#define LOADTEST_FRAMES_PER_RING  32   /* outstanding frames */

static int nr_rings = 4;
module_param(nr_rings, int, 0644);
MODULE_PARM_DESC(nr_rings, "Number of ring pairs to open (1..5; default 4)");

static int frame_size = LOADTEST_FRAME_SIZE;
module_param(frame_size, int, 0644);
MODULE_PARM_DESC(frame_size, "Frame size in bytes (default 4096)");

/* Service UUID (uuidgen -r): 96f4dde0-3e3a-4f4b-9b81-2c5d4f1c8a99 */
static const uuid_t loadtest_uuid =
	UUID_INIT(0x96f4dde0, 0x3e3a, 0x4f4b,
		  0x9b, 0x81, 0x2c, 0x5d, 0x4f, 0x1c, 0x8a, 0x99);

static struct tb_property_dir *loadtest_property_dir;
static struct dentry *loadtest_root;

/* ----- per-ring + per-device state -------------------------------- */

struct loadtest_frame {
	struct ring_frame frame;
	struct loadtest_ring *lr;
	void *data;
	dma_addr_t dma;
	bool is_tx;
};

struct loadtest_ring {
	struct loadtest_dev *dev;
	int idx;
	int hop;
	int out_hop;	/* xdomain TX path hop (we send TO this on the peer) */
	int in_hop;	/* xdomain RX path hop (peer sends TO this on us) */

	struct tb_ring *tx_ring;
	struct tb_ring *rx_ring;

	struct loadtest_frame *tx_frames;
	struct loadtest_frame *rx_frames;
	int frames_per_side;

	atomic_t tx_inflight;	/* number of frames currently in flight on TX */
	atomic64_t tx_bytes;
	atomic64_t rx_bytes;
	atomic64_t tx_packets;
	atomic64_t rx_packets;

	struct task_struct *tx_thread;
};

struct loadtest_dev {
	struct tb_service *svc;
	struct tb_xdomain *xd;

	int nr_rings;
	struct loadtest_ring rings[LOADTEST_MAX_RINGS];
	int rings_setup;	/* how many got fully set up before any error */

	struct dentry *dir;
	bool running;
	ktime_t start_time;
};

static inline struct device *ring_dev(struct loadtest_ring *lr)
{
	return tb_ring_dma_device(lr->tx_ring ?: lr->rx_ring);
}

/* ----- frame pool helpers ----------------------------------------- */

static int alloc_frames(struct loadtest_ring *lr,
			struct loadtest_frame **out, int count, bool tx)
{
	struct loadtest_frame *frames;
	int i;

	frames = kcalloc(count, sizeof(*frames), GFP_KERNEL);
	if (!frames)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		struct loadtest_frame *lf = &frames[i];

		lf->lr = lr;
		lf->is_tx = tx;
		lf->data = kmalloc(frame_size, GFP_KERNEL);
		if (!lf->data)
			goto err;
		if (tx) {
			/* Fill with a recognizable pattern so the receiver
			 * could in principle verify it. */
			memset(lf->data, 0xa5, frame_size);
		}
		lf->dma = dma_map_single(ring_dev(lr), lf->data,
					 frame_size,
					 tx ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
		if (dma_mapping_error(ring_dev(lr), lf->dma)) {
			kfree(lf->data);
			lf->data = NULL;
			goto err;
		}
		lf->frame.buffer_phy = lf->dma;
		lf->frame.size = frame_size;
		INIT_LIST_HEAD(&lf->frame.list);
	}
	*out = frames;
	return 0;

err:
	while (--i >= 0) {
		struct loadtest_frame *lf = &frames[i];
		dma_unmap_single(ring_dev(lr), lf->dma, frame_size,
				 tx ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
		kfree(lf->data);
	}
	kfree(frames);
	return -ENOMEM;
}

static void free_frames(struct loadtest_ring *lr,
			struct loadtest_frame *frames, int count, bool tx)
{
	int i;

	if (!frames)
		return;
	for (i = 0; i < count; i++) {
		struct loadtest_frame *lf = &frames[i];
		if (lf->data) {
			dma_unmap_single(ring_dev(lr), lf->dma,
					 frame_size,
					 tx ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
			kfree(lf->data);
		}
	}
	kfree(frames);
}

/* ----- callbacks --------------------------------------------------- */

static void tx_callback(struct tb_ring *ring, struct ring_frame *frame,
			bool canceled)
{
	struct loadtest_frame *lf = container_of(frame, typeof(*lf), frame);
	struct loadtest_ring *lr = lf->lr;

	if (!canceled) {
		atomic64_add(frame->size, &lr->tx_bytes);
		atomic64_inc(&lr->tx_packets);
	}
	atomic_dec(&lr->tx_inflight);
}

static void rx_callback(struct tb_ring *ring, struct ring_frame *frame,
			bool canceled)
{
	struct loadtest_frame *lf = container_of(frame, typeof(*lf), frame);
	struct loadtest_ring *lr = lf->lr;

	if (canceled)
		return;

	atomic64_add(frame->size, &lr->rx_bytes);
	atomic64_inc(&lr->rx_packets);

	/* Re-post the frame for another receive. */
	tb_ring_rx(lr->rx_ring, &lf->frame);
}

/* ----- TX kthread -------------------------------------------------- */

static int tx_thread_fn(void *data)
{
	struct loadtest_ring *lr = data;
	int slot = 0;

	while (!kthread_should_stop()) {
		if (atomic_read(&lr->tx_inflight) >= lr->frames_per_side) {
			/* Saturated — nothing to do until completions free
			 * a slot. */
			cond_resched();
			continue;
		}

		{
			struct loadtest_frame *lf = &lr->tx_frames[slot];

			lf->frame.callback = tx_callback;
			lf->frame.sof = LOADTEST_PDF_FRAME_START;
			lf->frame.eof = LOADTEST_PDF_FRAME_END;
			lf->frame.size = frame_size;

			atomic_inc(&lr->tx_inflight);
			if (tb_ring_tx(lr->tx_ring, &lf->frame) < 0) {
				atomic_dec(&lr->tx_inflight);
				usleep_range(100, 200);
				continue;
			}
			slot = (slot + 1) % lr->frames_per_side;
		}
	}

	return 0;
}

/* ----- ring setup / teardown -------------------------------------- */

static int loadtest_ring_setup(struct loadtest_dev *dev, int idx)
{
	struct loadtest_ring *lr = &dev->rings[idx];
	struct tb_xdomain *xd = dev->xd;
	int out_hop, in_hop;
	u16 sof_mask = BIT(LOADTEST_PDF_FRAME_START);
	u16 eof_mask = BIT(LOADTEST_PDF_FRAME_END);
	int ret, i;

	lr->dev = dev;
	lr->idx = idx;
	lr->frames_per_side = LOADTEST_FRAMES_PER_RING;
	atomic_set(&lr->tx_inflight, 0);
	atomic64_set(&lr->tx_bytes, 0);
	atomic64_set(&lr->rx_bytes, 0);

	/* Auto-allocate hop IDs (>= TB_PATH_MIN_HOPID = 8). */
	out_hop = tb_xdomain_alloc_out_hopid(xd, -1);
	if (out_hop < 0) {
		pr_warn("ring %d: out hopid alloc failed (%d)\n", idx, out_hop);
		return out_hop;
	}
	in_hop = tb_xdomain_alloc_in_hopid(xd, -1);
	if (in_hop < 0) {
		tb_xdomain_release_out_hopid(xd, out_hop);
		return in_hop;
	}
	lr->out_hop = out_hop;
	lr->in_hop = in_hop;
	lr->hop = out_hop;
	pr_info("ring %d: allocated out_hop=%d in_hop=%d\n",
		idx, out_hop, in_hop);

	lr->tx_ring = tb_ring_alloc_tx(xd->tb->nhi, -1, LOADTEST_RING_DEPTH,
				       RING_FLAG_FRAME);
	if (!lr->tx_ring) {
		ret = -ENOMEM;
		goto err_hopid;
	}

	lr->rx_ring = tb_ring_alloc_rx(xd->tb->nhi, -1, LOADTEST_RING_DEPTH,
				       RING_FLAG_FRAME, 0,
				       sof_mask, eof_mask, NULL, NULL);
	if (!lr->rx_ring) {
		ret = -ENOMEM;
		goto err_tx_ring;
	}

	pr_info("ring %d: tx_ring->hop=%d rx_ring->hop=%d\n",
		idx, lr->tx_ring->hop, lr->rx_ring->hop);

	ret = alloc_frames(lr, &lr->tx_frames, lr->frames_per_side, true);
	if (ret)
		goto err_rx_ring;
	ret = alloc_frames(lr, &lr->rx_frames, lr->frames_per_side, false);
	if (ret)
		goto err_tx_frames;

	/* Wire the actual data path between the peers. The signature is
	 *   enable_paths(xd, transmit_path, transmit_ring,
	 *                receive_path, receive_ring)
	 * where transmit_* is the local TX side, receive_* is the local
	 * RX side. Both peers call this; kernel reconciles. */
	ret = tb_xdomain_enable_paths(xd, lr->out_hop, lr->tx_ring->hop,
				      lr->in_hop, lr->rx_ring->hop);
	if (ret) {
		pr_warn("ring %d: enable_paths failed: %d\n", idx, ret);
		goto err_rx_frames;
	}

	/* Pre-post all RX frames. */
	for (i = 0; i < lr->frames_per_side; i++) {
		struct loadtest_frame *lf = &lr->rx_frames[i];
		lf->frame.callback = rx_callback;
		tb_ring_rx(lr->rx_ring, &lf->frame);
	}

	tb_ring_start(lr->tx_ring);
	tb_ring_start(lr->rx_ring);

	return 0;

err_rx_frames:
	free_frames(lr, lr->rx_frames, lr->frames_per_side, false);
	lr->rx_frames = NULL;
err_tx_frames:
	free_frames(lr, lr->tx_frames, lr->frames_per_side, true);
	lr->tx_frames = NULL;
err_rx_ring:
	tb_ring_free(lr->rx_ring);
	lr->rx_ring = NULL;
err_tx_ring:
	tb_ring_free(lr->tx_ring);
	lr->tx_ring = NULL;
err_hopid:
	tb_xdomain_release_in_hopid(xd, in_hop);
	tb_xdomain_release_out_hopid(xd, out_hop);
	return ret;
}

static void loadtest_ring_teardown(struct loadtest_dev *dev, int idx)
{
	struct loadtest_ring *lr = &dev->rings[idx];
	struct tb_xdomain *xd = dev->xd;

	if (lr->tx_thread) {
		kthread_stop(lr->tx_thread);
		lr->tx_thread = NULL;
	}

	if (lr->tx_ring) {
		tb_ring_stop(lr->tx_ring);
		tb_xdomain_disable_paths(xd, lr->out_hop, lr->tx_ring->hop,
					 lr->in_hop, lr->rx_ring->hop);
	}
	if (lr->rx_ring)
		tb_ring_stop(lr->rx_ring);

	free_frames(lr, lr->tx_frames, lr->frames_per_side, true);
	free_frames(lr, lr->rx_frames, lr->frames_per_side, false);
	lr->tx_frames = NULL;
	lr->rx_frames = NULL;

	if (lr->tx_ring) {
		tb_ring_free(lr->tx_ring);
		lr->tx_ring = NULL;
	}
	if (lr->rx_ring) {
		tb_ring_free(lr->rx_ring);
		lr->rx_ring = NULL;
	}

	tb_xdomain_release_in_hopid(xd, lr->in_hop);
	tb_xdomain_release_out_hopid(xd, lr->out_hop);
}

/* ----- start / stop the test -------------------------------------- */

static int loadtest_start(struct loadtest_dev *dev)
{
	int i;

	if (dev->running)
		return -EBUSY;

	for (i = 0; i < dev->nr_rings; i++) {
		struct loadtest_ring *lr = &dev->rings[i];
		atomic64_set(&lr->tx_bytes, 0);
		atomic64_set(&lr->rx_bytes, 0);
		atomic64_set(&lr->tx_packets, 0);
		atomic64_set(&lr->rx_packets, 0);
	}

	dev->start_time = ktime_get();

	for (i = 0; i < dev->nr_rings; i++) {
		struct loadtest_ring *lr = &dev->rings[i];
		lr->tx_thread = kthread_run(tx_thread_fn, lr,
					    "u4lt-tx-%d", i);
		if (IS_ERR(lr->tx_thread)) {
			int err = PTR_ERR(lr->tx_thread);
			lr->tx_thread = NULL;
			while (--i >= 0) {
				kthread_stop(dev->rings[i].tx_thread);
				dev->rings[i].tx_thread = NULL;
			}
			return err;
		}
	}

	dev->running = true;
	return 0;
}

static void loadtest_stop(struct loadtest_dev *dev)
{
	int i;

	if (!dev->running)
		return;

	for (i = 0; i < dev->nr_rings; i++) {
		if (dev->rings[i].tx_thread) {
			kthread_stop(dev->rings[i].tx_thread);
			dev->rings[i].tx_thread = NULL;
		}
	}

	dev->running = false;
}

/* ----- debugfs: stats / start / stop ------------------------------ */

static int stats_show(struct seq_file *m, void *unused)
{
	struct loadtest_dev *dev = m->private;
	u64 total_tx_bytes = 0, total_rx_bytes = 0;
	u64 total_tx_pkts = 0, total_rx_pkts = 0;
	u64 elapsed_ns;
	int i;

	if (!dev->running) {
		seq_puts(m, "(not running — write 1 to start to begin)\n");
		return 0;
	}

	elapsed_ns = ktime_to_ns(ktime_sub(ktime_get(), dev->start_time));
	if (elapsed_ns == 0)
		elapsed_ns = 1;

	seq_printf(m, "nr_rings:    %d\n", dev->nr_rings);
	seq_printf(m, "frame_size:  %d bytes\n", frame_size);
	seq_printf(m, "elapsed:     %llu ms\n", elapsed_ns / 1000000);
	seq_puts(m, "\n");

	seq_printf(m, "  %-4s %-10s %-12s %-12s %-12s %-12s %-12s\n",
		   "ring", "hop", "tx_bytes", "rx_bytes",
		   "tx_pkts", "rx_pkts", "rx_Gbps");
	seq_puts(m, "  --------------------------------------------------------------------------\n");

	for (i = 0; i < dev->nr_rings; i++) {
		struct loadtest_ring *lr = &dev->rings[i];
		u64 tb = atomic64_read(&lr->tx_bytes);
		u64 rb = atomic64_read(&lr->rx_bytes);
		u64 tp = atomic64_read(&lr->tx_packets);
		u64 rp = atomic64_read(&lr->rx_packets);
		u64 gbps_x100 = (rb * 8 * 100 * 1000000000ULL) /
				(elapsed_ns * 1000000000ULL);
		/* Simpler: rx Gbps = rx_bytes * 8 / elapsed_ns */
		u64 gbps_int = (rb * 8) / elapsed_ns;
		u64 gbps_frac = ((rb * 8) % elapsed_ns) * 100 / elapsed_ns;

		(void)gbps_x100;
		seq_printf(m, "  %-4d %-10d %-12llu %-12llu %-12llu %-12llu %llu.%02llu\n",
			   i, lr->hop, tb, rb, tp, rp,
			   gbps_int, gbps_frac);

		total_tx_bytes += tb;
		total_rx_bytes += rb;
		total_tx_pkts += tp;
		total_rx_pkts += rp;
	}

	{
		u64 tx_gbps_int = (total_tx_bytes * 8) / elapsed_ns;
		u64 rx_gbps_int = (total_rx_bytes * 8) / elapsed_ns;
		u64 tx_gbps_frac = ((total_tx_bytes * 8) % elapsed_ns) * 100 / elapsed_ns;
		u64 rx_gbps_frac = ((total_rx_bytes * 8) % elapsed_ns) * 100 / elapsed_ns;

		seq_puts(m, "\n");
		seq_printf(m, "AGGREGATE TX: %llu bytes, %llu pkts, %llu.%02llu Gbps\n",
			   total_tx_bytes, total_tx_pkts,
			   tx_gbps_int, tx_gbps_frac);
		seq_printf(m, "AGGREGATE RX: %llu bytes, %llu pkts, %llu.%02llu Gbps\n",
			   total_rx_bytes, total_rx_pkts,
			   rx_gbps_int, rx_gbps_frac);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(stats);

static ssize_t start_write(struct file *file, const char __user *user_buf,
			   size_t count, loff_t *ppos)
{
	struct loadtest_dev *dev = file->f_inode->i_private;
	char buf[8];
	int val;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;
	buf[count] = '\0';
	if (kstrtoint(buf, 0, &val))
		return -EINVAL;

	if (val) {
		int ret = loadtest_start(dev);
		if (ret)
			return ret;
	} else {
		loadtest_stop(dev);
	}
	return count;
}

static const struct file_operations start_fops = {
	.owner = THIS_MODULE,
	.write = start_write,
	.open  = simple_open,
	.llseek = noop_llseek,
};

/* ----- service driver probe / remove ------------------------------ */

static int loadtest_probe(struct tb_service *svc, const struct tb_service_id *id)
{
	struct loadtest_dev *dev;
	struct tb_xdomain *xd;
	int i, ret;

	pr_info("loadtest: probe entered, sizeof(loadtest_dev)=%zu, "
		"offsetof svc=%zu xd=%zu nr_rings=%zu rings=%zu rings_setup=%zu\n",
		sizeof(struct loadtest_dev),
		offsetof(struct loadtest_dev, svc),
		offsetof(struct loadtest_dev, xd),
		offsetof(struct loadtest_dev, nr_rings),
		offsetof(struct loadtest_dev, rings),
		offsetof(struct loadtest_dev, rings_setup));

	xd = tb_service_parent(svc);
	pr_info("loadtest: xd=%p\n", xd);

	if (!xd)
		return -ENODEV;

	if (nr_rings < 1 || nr_rings > LOADTEST_MAX_RINGS)
		return -EINVAL;

	pr_info("loadtest: route 0x%llx, link_speed=%u, %d rings\n",
		xd->route, xd->link_speed, nr_rings);

	dev = devm_kzalloc(&svc->dev, sizeof(*dev), GFP_KERNEL);
	pr_info("loadtest: dev=%p\n", dev);
	if (!dev)
		return -ENOMEM;

	dev->svc = svc;
	dev->xd = xd;
	dev->nr_rings = nr_rings;
	pr_info("loadtest: struct populated, entering ring loop\n");

	for (i = 0; i < nr_rings; i++) {
		ret = loadtest_ring_setup(dev, i);
		if (ret) {
			dev_warn(&svc->dev,
				 "ring %d setup failed (%d) — only %d ring(s) available\n",
				 i, ret, i);
			break;
		}
		dev->rings_setup = i + 1;
	}

	if (dev->rings_setup == 0) {
		dev_err(&svc->dev, "no rings could be set up\n");
		return -ENODEV;
	}
	dev->nr_rings = dev->rings_setup;

	dev->dir = debugfs_create_dir(dev_name(&svc->dev), loadtest_root);
	if (!IS_ERR_OR_NULL(dev->dir)) {
		debugfs_create_file("stats", 0444, dev->dir, dev, &stats_fops);
		debugfs_create_file("start", 0200, dev->dir, dev, &start_fops);
	}

	tb_service_set_drvdata(svc, dev);

	dev_info(&svc->dev,
		"loadtest: ready with %d ring pair(s). Write 1 to debugfs/start to begin TX.\n",
		dev->nr_rings);

	return 0;
}

static void loadtest_remove(struct tb_service *svc)
{
	struct loadtest_dev *dev = tb_service_get_drvdata(svc);
	int i;

	dev_info(&svc->dev, "loadtest: remove\n");
	if (!dev)
		return;

	loadtest_stop(dev);
	debugfs_remove_recursive(dev->dir);
	for (i = 0; i < dev->rings_setup; i++)
		loadtest_ring_teardown(dev, i);
	tb_service_set_drvdata(svc, NULL);
}

static const struct tb_service_id loadtest_ids[] = {
	{ TB_SERVICE(LOADTEST_PROTO_KEY, LOADTEST_PROTO_ID) },
	{ },
};
MODULE_DEVICE_TABLE(tbsvc, loadtest_ids);

static struct tb_service_driver loadtest_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name  = "u4_loadtest",
	},
	.probe   = loadtest_probe,
	.remove  = loadtest_remove,
	.id_table = loadtest_ids,
};

/* ----- public init / exit ----------------------------------------- */

int usb4_rdma_loadtest_init(struct dentry *parent_dir)
{
	struct tb_property_dir *dir;
	int err;

	pr_info("init: sizeof(loadtest_dev)=%zu, sizeof(loadtest_ring)=%zu, "
		"offsetof svc=%zu xd=%zu nr_rings=%zu rings=%zu rings_setup=%zu running=%zu\n",
		sizeof(struct loadtest_dev),
		sizeof(struct loadtest_ring),
		offsetof(struct loadtest_dev, svc),
		offsetof(struct loadtest_dev, xd),
		offsetof(struct loadtest_dev, nr_rings),
		offsetof(struct loadtest_dev, rings),
		offsetof(struct loadtest_dev, rings_setup),
		offsetof(struct loadtest_dev, running));

	loadtest_root = debugfs_create_dir("loadtest", parent_dir);
	if (IS_ERR(loadtest_root))
		loadtest_root = NULL;

	dir = tb_property_create_dir(&loadtest_uuid);
	if (!dir)
		return -ENOMEM;
	err = tb_property_add_immediate(dir, "prtcid", LOADTEST_PROTO_ID);
	err = err ?: tb_property_add_immediate(dir, "prtcvers", 1);
	err = err ?: tb_property_add_immediate(dir, "prtcrevs", 1);
	err = err ?: tb_property_add_immediate(dir, "prtcstns", 0);
	if (err) {
		tb_property_free_dir(dir);
		goto err_root;
	}

	err = tb_register_property_dir(LOADTEST_PROTO_KEY, dir);
	if (err) {
		tb_property_free_dir(dir);
		goto err_root;
	}
	loadtest_property_dir = dir;

	err = tb_register_service_driver(&loadtest_driver);
	if (err) {
		tb_unregister_property_dir(LOADTEST_PROTO_KEY, dir);
		tb_property_free_dir(dir);
		goto err_root;
	}

	pr_info("loadtest service registered (uuid %pUb, key=%s, %d ring(s))\n",
		&loadtest_uuid, LOADTEST_PROTO_KEY, nr_rings);
	return 0;

err_root:
	debugfs_remove_recursive(loadtest_root);
	loadtest_root = NULL;
	return err;
}

void usb4_rdma_loadtest_exit(void)
{
	tb_unregister_service_driver(&loadtest_driver);
	if (loadtest_property_dir) {
		tb_unregister_property_dir(LOADTEST_PROTO_KEY,
					   loadtest_property_dir);
		tb_property_free_dir(loadtest_property_dir);
		loadtest_property_dir = NULL;
	}
	debugfs_remove_recursive(loadtest_root);
	loadtest_root = NULL;
}
