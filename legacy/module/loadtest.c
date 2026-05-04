// SPDX-License-Identifier: GPL-2.0
/*
 * loadtest.c — multi-ring xdomain throughput probe.
 *
 * The question: when we open N parallel `tb_ring` pairs on the same
 * USB4 host router, does aggregate throughput scale linearly with N,
 * or is there a shared internal bottleneck that caps the controller
 * below N × per-ring rate?
 *
 * On Strix Halo this turned out narrow: REG_CAPS reports hop_count=3,
 * of which the connection manager and (when loaded) thunderbolt_net
 * consume 2, leaving 1 free hop per controller. So the practical
 * version of the question is "does a single raw `tb_ring` deliver
 * more than thunderbolt_net's TCP path (~10 Gbps), validating that
 * the cap is in `tbnet`/IP rather than in the controller itself?"
 *
 * Approach:
 *   1. Register a `tb_service` under a separate key ("u4lt") so we
 *      don't conflict with thunderbolt_net's "network".
 *   2. On probe (peer discovered), allocate N TX+RX ring pairs with
 *      auto-assigned hop IDs (`tb_xdomain_alloc_*_hopid(xd, -1)` —
 *      kernel picks the lowest free hop >= TB_PATH_MIN_HOPID = 8).
 *   3. Order matters (per `drivers/net/thunderbolt/main.c`):
 *        a. allocate TX/RX ring objects
 *        b. allocate TX/RX frame buffers
 *        c. tb_ring_start() on both
 *        d. post RX buffers via tb_ring_rx()
 *        e. tb_xdomain_enable_paths() LAST
 *      Posting RX before tb_ring_start() returns -ESHUTDOWN and the
 *      buffers never get queued, leaving RX silent.
 *   4. Frames carry no protocol — just SZ_4K of zeros, SOF/EOF
 *      markers per `dma_test`.
 *   5. RX callback re-posts the frame; TX callback marks slot free.
 *   6. Aggregate counters in debugfs:
 *        /sys/kernel/debug/usb4_rdma/loadtest/<peer>/stats
 *        /sys/kernel/debug/usb4_rdma/loadtest/<peer>/start
 *
 * Wire-protocol caveat: `tb_xdomain_enable_paths()` takes
 *   (xd, local_tx_path, local_tx_ring_hop, remote_tx_path, local_rx_ring_hop)
 * where `remote_tx_path` is the peer's TX HopID — what they send TO,
 * which is what we receive FROM. The correct way to learn this is a
 * login-style xdomain request/response (see `tbnet_login_request` for
 * the canonical example). This module currently relies on accidental
 * symmetry: both peers run identical code from identical state, so
 * `alloc_*_hopid(-1)` returns the same number on both sides. Works
 * for a single-ring-pair test from a clean boot; fragile in general.
 * Marked TODO; first-class login is its own commit.
 */

#define pr_fmt(fmt) "usb4_rdma/loadtest: " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/uuid.h>
#include <linux/bitops.h>
#include <linux/thunderbolt.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/kthread.h>
#include <linux/atomic.h>
#include <linux/ktime.h>
#include <linux/delay.h>
#include <linux/io.h>

#include "usb4_rdma.h"
#include "nhi_raw.h"

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
#define LOADTEST_RING_DEPTH_DEFAULT      256
#define LOADTEST_RING_DEPTH_MIN          16
#define LOADTEST_RING_DEPTH_MAX          4095
#define LOADTEST_FRAMES_PER_RING_SLACK   32
#define LOADTEST_RAW_BATCH_MAX    128

#define LOADTEST_NHI_REG_TX_RING_BASE  0x00000
#define LOADTEST_NHI_REG_RX_RING_BASE  0x08000
#define LOADTEST_NHI_RING_SLOT_SIZE    16

#define LOADTEST_NHI_DESC_LENGTH_MASK  GENMASK(11, 0)
#define LOADTEST_NHI_DESC_EOF_SHIFT    12
#define LOADTEST_NHI_DESC_EOF_MASK     GENMASK(15, 12)
#define LOADTEST_NHI_DESC_SOF_SHIFT    16
#define LOADTEST_NHI_DESC_SOF_MASK     GENMASK(19, 16)
#define LOADTEST_NHI_DESC_FLAGS_SHIFT  20
#define LOADTEST_NHI_DESC_FLAGS_MASK   GENMASK(31, 20)

struct loadtest_nhi_desc {
	u64 phys;
	u32 meta;
	u32 time;
} __packed;

static_assert(sizeof(struct loadtest_nhi_desc) == 16);

static bool enable;
module_param(enable, bool, 0444);
MODULE_PARM_DESC(enable,
	"Register the loadtest tb_service and claim hops on probe (default 0). "
	"Off by default so the experimental driver doesn't compete with usb4_rdma "
	"data-path rings for the controller's narrow hop budget.");

static int nr_rings = 4;
module_param(nr_rings, int, 0644);
MODULE_PARM_DESC(nr_rings, "Number of ring pairs to open (1..5; default 4)");

static int frame_size = LOADTEST_FRAME_SIZE;
module_param(frame_size, int, 0644);
MODULE_PARM_DESC(frame_size, "Frame size in bytes (default 4096)");

static uint ring_depth = LOADTEST_RING_DEPTH_DEFAULT;
module_param(ring_depth, uint, 0444);
MODULE_PARM_DESC(ring_depth,
	"NHI descriptor ring depth for loadtest rings (default 256, max 4095)");

static uint frames_per_ring;
module_param(frames_per_ring, uint, 0444);
MODULE_PARM_DESC(frames_per_ring,
	"Outstanding frame buffers per loadtest ring; 0 = ring_depth - 32 (default)");

static bool loadtest_raw_batch = true;
module_param(loadtest_raw_batch, bool, 0644);
MODULE_PARM_DESC(loadtest_raw_batch,
	"Use raw-NHI batch post/poll in loadtest when raw_nhi=1 (default true)");

static bool loadtest_raw_unlocked;
module_param(loadtest_raw_unlocked, bool, 0644);
MODULE_PARM_DESC(loadtest_raw_unlocked,
	"Use unsafe unlocked raw-NHI helpers in loadtest; requires one software owner per ring (default false)");

static bool loadtest_flood;
module_param(loadtest_flood, bool, 0444);
MODULE_PARM_DESC(loadtest_flood,
	"Use experimental descriptor-flood loadtest path: fixed descriptor-slot buffers, no ring lists, callbacks, or per-frame atomics; requires raw_nhi=1 raw_nhi_desc_irq=0 (default false)");

static unsigned int loadtest_flood_doorbell_batch;
module_param(loadtest_flood_doorbell_batch, uint, 0444);
MODULE_PARM_DESC(loadtest_flood_doorbell_batch,
	"Flood path minimum free descriptors before producer-index write; 0 reposts any free descriptors immediately (default 0)");

static bool loadtest_tx = true;
module_param(loadtest_tx, bool, 0644);
MODULE_PARM_DESC(loadtest_tx,
	"Start loadtest TX threads when debugfs/start is written (default true; set false for RX-only peer)");

static bool loadtest_rx = true;
module_param(loadtest_rx, bool, 0644);
MODULE_PARM_DESC(loadtest_rx,
	"Start raw loadtest RX poll threads when debugfs/start is written (default true; set false for TX-only peer)");

static bool loadtest_e2e = true;
module_param(loadtest_e2e, bool, 0444);
MODULE_PARM_DESC(loadtest_e2e,
	"Enable E2E flow control on loadtest rings (default true)");

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
	atomic64_t tx_errors;
	atomic64_t rx_repost_errors;

	u64 flood_tx_bytes;
	u64 flood_rx_bytes;
	u64 flood_tx_packets;
	u64 flood_rx_packets;
	u64 flood_tx_errors;
	u64 flood_rx_errors;
	u64 flood_tx_doorbells;
	u64 flood_rx_doorbells;

	struct task_struct *tx_thread;
	struct task_struct *rx_thread;
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

static bool loadtest_use_raw_batch(void)
{
	return READ_ONCE(loadtest_raw_batch) && usb4_rdma_nhi_raw_enabled();
}

static bool loadtest_use_flood(void)
{
	return READ_ONCE(loadtest_flood) && loadtest_use_raw_batch();
}

static unsigned int loadtest_ring_depth(void)
{
	return clamp_t(uint, READ_ONCE(ring_depth), LOADTEST_RING_DEPTH_MIN,
		       LOADTEST_RING_DEPTH_MAX);
}

static unsigned int loadtest_auto_frames_per_ring(unsigned int depth)
{
	if (depth > LOADTEST_FRAMES_PER_RING_SLACK)
		return depth - LOADTEST_FRAMES_PER_RING_SLACK;

	return depth - 1;
}

static unsigned int loadtest_frames_per_ring(unsigned int depth, bool flood)
{
	uint frames;

	if (flood)
		return depth;

	frames = READ_ONCE(frames_per_ring);
	if (!frames)
		frames = loadtest_auto_frames_per_ring(depth);

	return clamp_t(uint, frames, 1, depth - 1);
}

static void __iomem *loadtest_nhi_desc_base(const struct tb_ring *ring)
{
	void __iomem *io = ring->nhi->iobase;

	io += ring->is_tx ? LOADTEST_NHI_REG_TX_RING_BASE :
			    LOADTEST_NHI_REG_RX_RING_BASE;
	io += ring->hop * LOADTEST_NHI_RING_SLOT_SIZE;
	return io;
}

static void loadtest_nhi_write_index(const struct tb_ring *ring, u16 index)
{
	void __iomem *reg = loadtest_nhi_desc_base(ring) + 8;

	if (ring->is_tx)
		iowrite32((u32)index << 16, reg);
	else
		iowrite32(index, reg);
}

static bool loadtest_ring_empty(const struct tb_ring *ring)
{
	return ring->head == ring->tail;
}

static unsigned int loadtest_ring_free(const struct tb_ring *ring)
{
	if (ring->head >= ring->tail)
		return ring->size - (ring->head - ring->tail) - 1;

	return ring->tail - ring->head - 1;
}

static u32 loadtest_desc_flags(u32 meta)
{
	return (meta & LOADTEST_NHI_DESC_FLAGS_MASK) >>
	       LOADTEST_NHI_DESC_FLAGS_SHIFT;
}

static u32 loadtest_desc_length(u32 meta)
{
	return meta & LOADTEST_NHI_DESC_LENGTH_MASK;
}

static u32 loadtest_flood_tx_meta(void)
{
	u32 flags = RING_DESC_POSTED;
	u32 meta = flags << LOADTEST_NHI_DESC_FLAGS_SHIFT;

	meta |= frame_size & LOADTEST_NHI_DESC_LENGTH_MASK;
	meta |= (LOADTEST_PDF_FRAME_END << LOADTEST_NHI_DESC_EOF_SHIFT) &
		LOADTEST_NHI_DESC_EOF_MASK;
	meta |= (LOADTEST_PDF_FRAME_START << LOADTEST_NHI_DESC_SOF_SHIFT) &
		LOADTEST_NHI_DESC_SOF_MASK;
	return meta;
}

static u32 loadtest_flood_rx_meta(void)
{
	return RING_DESC_POSTED << LOADTEST_NHI_DESC_FLAGS_SHIFT;
}

static u32 loadtest_flood_wire_bytes(u32 desc_len)
{
	if (desc_len)
		return desc_len;
	return min_t(u32, frame_size, SZ_4K);
}

static u32 loadtest_flood_tx_bytes_per_frame(void)
{
	return min_t(u32, frame_size, SZ_4K);
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

/*
 * struct ring_frame.size is a 12-bit field. A "full" 4096-byte frame
 * is encoded as 0 in the descriptor (the convention the hardware uses
 * because 4096 doesn't fit). On TX completion the value we set isn't
 * updated; on RX the hardware writes the actual received length, again
 * with 0 meaning 4096. So both directions need the same fixup before
 * accounting bytes.
 */
static inline u32 frame_actual_bytes(const struct ring_frame *frame)
{
	return frame->size ? frame->size : (u32)frame_size;
}

static void tx_callback(struct tb_ring *ring, struct ring_frame *frame,
			bool canceled)
{
	struct loadtest_frame *lf = container_of(frame, typeof(*lf), frame);
	struct loadtest_ring *lr = lf->lr;

	if (!canceled) {
		atomic64_add(frame_actual_bytes(frame), &lr->tx_bytes);
		atomic64_inc(&lr->tx_packets);
	}
	atomic_dec(&lr->tx_inflight);
}

static void loadtest_account_rx(struct loadtest_ring *lr,
				const struct ring_frame *frame)
{
	atomic64_add(frame_actual_bytes(frame), &lr->rx_bytes);
	atomic64_inc(&lr->rx_packets);
}

static void rx_callback(struct tb_ring *ring, struct ring_frame *frame,
			bool canceled)
{
	struct loadtest_frame *lf = container_of(frame, typeof(*lf), frame);
	struct loadtest_ring *lr = lf->lr;

	if (canceled)
		return;

	loadtest_account_rx(lr, frame);

	/* Re-post the frame for another receive. */
	tb_ring_rx(lr->rx_ring, &lf->frame);
}

/* ----- TX kthread -------------------------------------------------- */

static bool loadtest_poll_tx_raw(struct loadtest_ring *lr)
{
	struct ring_frame *frame, *tmp;
	LIST_HEAD(done);
	unsigned int n;

	if (READ_ONCE(loadtest_raw_unlocked))
		n = usb4_rdma_nhi_raw_poll_batch_unlocked(lr->tx_ring, &done,
							  LOADTEST_RAW_BATCH_MAX);
	else
		n = usb4_rdma_nhi_raw_poll_batch(lr->tx_ring, &done,
						 LOADTEST_RAW_BATCH_MAX);
	list_for_each_entry_safe(frame, tmp, &done, list) {
		list_del_init(&frame->list);
		if (frame->callback)
			frame->callback(lr->tx_ring, frame, false);
	}
	return n > 0;
}

static void loadtest_cancel_unposted_tx(struct loadtest_ring *lr,
					struct list_head *frames)
{
	struct ring_frame *frame, *tmp;

	list_for_each_entry_safe(frame, tmp, frames, list) {
		list_del_init(&frame->list);
		atomic_dec(&lr->tx_inflight);
		atomic64_inc(&lr->tx_errors);
	}
}

static bool loadtest_post_tx_raw(struct loadtest_ring *lr, int *slot)
{
	LIST_HEAD(submit);
	int avail, count = 0;
	int ret;

	avail = lr->frames_per_side - atomic_read(&lr->tx_inflight);
	while (avail-- > 0 && count < LOADTEST_RAW_BATCH_MAX) {
		struct loadtest_frame *lf = &lr->tx_frames[*slot];

		if (WARN_ON_ONCE(!list_empty(&lf->frame.list)))
			break;

		lf->frame.callback = tx_callback;
		lf->frame.sof = LOADTEST_PDF_FRAME_START;
		lf->frame.eof = LOADTEST_PDF_FRAME_END;
		lf->frame.flags = 0;
		lf->frame.size = frame_size;

		list_add_tail(&lf->frame.list, &submit);
		atomic_inc(&lr->tx_inflight);
		*slot = (*slot + 1) % lr->frames_per_side;
		count++;
	}

	if (list_empty(&submit))
		return false;

	if (READ_ONCE(loadtest_raw_unlocked))
		ret = usb4_rdma_nhi_raw_tx_batch_unlocked(lr->tx_ring,
							  &submit);
	else
		ret = usb4_rdma_nhi_raw_tx_batch_atomic(lr->tx_ring,
							&submit);
	if (ret && ret != -EAGAIN)
		atomic64_inc(&lr->tx_errors);
	if (!list_empty(&submit))
		loadtest_cancel_unposted_tx(lr, &submit);

	return true;
}

static int tx_thread_fn(void *data)
{
	struct loadtest_ring *lr = data;
	int slot = 0;
	unsigned int empty = 0;

	while (!kthread_should_stop()) {
		if (loadtest_use_raw_batch()) {
			bool work;

			work = loadtest_poll_tx_raw(lr);
			work |= loadtest_post_tx_raw(lr, &slot);
			if (!work) {
				cpu_relax();
				if (++empty >= 4096) {
					empty = 0;
					cond_resched();
				}
			} else {
				empty = 0;
			}
			continue;
		}

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
			lf->frame.flags = 0;
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

static void loadtest_repost_rx_raw(struct loadtest_ring *lr,
				   struct list_head *frames)
{
	struct ring_frame *frame, *tmp;
	int ret;

	while (!list_empty(frames)) {
		if (READ_ONCE(loadtest_raw_unlocked))
			ret = usb4_rdma_nhi_raw_rx_batch_unlocked(lr->rx_ring,
								  frames);
		else
			ret = usb4_rdma_nhi_raw_rx_batch_atomic(lr->rx_ring,
								frames);
		if (!ret)
			return;
		if (ret == -EAGAIN) {
			cpu_relax();
			cond_resched();
			continue;
		}

		list_for_each_entry_safe(frame, tmp, frames, list) {
			list_del_init(&frame->list);
			atomic64_inc(&lr->rx_repost_errors);
		}
		break;
	}
}

static bool loadtest_poll_rx_raw(struct loadtest_ring *lr)
{
	struct ring_frame *frame, *tmp;
	LIST_HEAD(done);
	LIST_HEAD(repost);
	unsigned int n;

	if (READ_ONCE(loadtest_raw_unlocked))
		n = usb4_rdma_nhi_raw_poll_batch_unlocked(lr->rx_ring, &done,
							  LOADTEST_RAW_BATCH_MAX);
	else
		n = usb4_rdma_nhi_raw_poll_batch(lr->rx_ring, &done,
						 LOADTEST_RAW_BATCH_MAX);
	list_for_each_entry_safe(frame, tmp, &done, list) {
		list_del_init(&frame->list);
		loadtest_account_rx(lr, frame);
		frame->callback = rx_callback;
		list_add_tail(&frame->list, &repost);
	}

	loadtest_repost_rx_raw(lr, &repost);
	return n > 0;
}

static int rx_thread_fn(void *data)
{
	struct loadtest_ring *lr = data;
	unsigned int empty = 0;

	while (!kthread_should_stop()) {
		if (loadtest_poll_rx_raw(lr)) {
			empty = 0;
			continue;
		}

		cpu_relax();
		if (++empty >= 4096) {
			empty = 0;
			cond_resched();
		}
	}

	return 0;
}

/* ----- descriptor-flood loadtest path ----------------------------- */

static unsigned int loadtest_flood_post(struct loadtest_ring *lr,
					struct tb_ring *ring,
					struct loadtest_frame *frames,
					bool tx)
{
	struct loadtest_nhi_desc *descs =
		(struct loadtest_nhi_desc *)ring->descriptors;
	u32 meta = tx ? loadtest_flood_tx_meta() : loadtest_flood_rx_meta();
	unsigned int posted = 0;
	unsigned int batch = READ_ONCE(loadtest_flood_doorbell_batch);
	unsigned int free;

	if (WARN_ON_ONCE(lr->frames_per_side < ring->size))
		return 0;
	if (!ring->running)
		return 0;
	if (batch >= ring->size)
		batch = ring->size - 1;

	free = loadtest_ring_free(ring);
	if (!free)
		return 0;
	if (batch && free < batch)
		return 0;

	while (posted < free) {
		struct loadtest_frame *lf = &frames[ring->head];

		descs[ring->head].phys = lf->frame.buffer_phy;
		descs[ring->head].meta = meta;
		descs[ring->head].time = 0;

		ring->head = (ring->head + 1) % ring->size;
		posted++;
	}

	if (posted) {
		dma_wmb();
		loadtest_nhi_write_index(ring, ring->head);
		if (tx)
			lr->flood_tx_doorbells++;
		else
			lr->flood_rx_doorbells++;
	}

	return posted;
}

static int loadtest_flood_prime_rx(struct loadtest_ring *lr)
{
	unsigned int posted;

	posted = loadtest_flood_post(lr, lr->rx_ring, lr->rx_frames, false);
	if (!posted)
		return -EAGAIN;

	pr_info("ring %d: flood-primed %u RX descriptors\n", lr->idx,
		posted);
	return 0;
}

static unsigned int loadtest_flood_poll_tx(struct loadtest_ring *lr)
{
	struct tb_ring *ring = lr->tx_ring;
	struct loadtest_nhi_desc *descs =
		(struct loadtest_nhi_desc *)ring->descriptors;
	unsigned int done = 0;

	if (!ring->running)
		return 0;

	while (!loadtest_ring_empty(ring)) {
		u32 meta = READ_ONCE(descs[ring->tail].meta);

		if (!(loadtest_desc_flags(meta) & RING_DESC_COMPLETED))
			break;

		ring->tail = (ring->tail + 1) % ring->size;
		done++;
	}

	if (done) {
		lr->flood_tx_packets += done;
		lr->flood_tx_bytes += (u64)done *
				      loadtest_flood_tx_bytes_per_frame();
	}

	return done;
}

static unsigned int loadtest_flood_poll_rx(struct loadtest_ring *lr)
{
	struct tb_ring *ring = lr->rx_ring;
	struct loadtest_nhi_desc *descs =
		(struct loadtest_nhi_desc *)ring->descriptors;
	unsigned int done = 0;
	u64 bytes = 0;

	if (!ring->running)
		return 0;

	while (!loadtest_ring_empty(ring)) {
		u32 meta = READ_ONCE(descs[ring->tail].meta);
		u32 flags = loadtest_desc_flags(meta);

		if (!(flags & RING_DESC_COMPLETED))
			break;

		if (flags & (RING_DESC_CRC_ERROR | RING_DESC_BUFFER_OVERRUN))
			lr->flood_rx_errors++;
		bytes += loadtest_flood_wire_bytes(loadtest_desc_length(meta));
		ring->tail = (ring->tail + 1) % ring->size;
		done++;
	}

	if (done) {
		lr->flood_rx_packets += done;
		lr->flood_rx_bytes += bytes;
	}

	return done;
}

static int flood_tx_thread_fn(void *data)
{
	struct loadtest_ring *lr = data;
	unsigned int empty = 0;

	while (!kthread_should_stop()) {
		bool work = false;

		work |= loadtest_flood_poll_tx(lr) > 0;
		work |= loadtest_flood_post(lr, lr->tx_ring,
					    lr->tx_frames, true) > 0;

		if (!work) {
			cpu_relax();
			if (++empty >= 4096) {
				empty = 0;
				cond_resched();
			}
		} else {
			empty = 0;
		}
	}

	return 0;
}

static int flood_rx_thread_fn(void *data)
{
	struct loadtest_ring *lr = data;
	unsigned int empty = 0;

	while (!kthread_should_stop()) {
		bool work = false;

		work |= loadtest_flood_poll_rx(lr) > 0;
		work |= loadtest_flood_post(lr, lr->rx_ring,
					    lr->rx_frames, false) > 0;

		if (!work) {
			cpu_relax();
			if (++empty >= 4096) {
				empty = 0;
				cond_resched();
			}
		} else {
			empty = 0;
		}
	}

	return 0;
}

/* ----- ring setup / teardown -------------------------------------- */

static int loadtest_ring_setup(struct loadtest_dev *dev, int idx)
{
	struct loadtest_ring *lr = &dev->rings[idx];
	struct tb_xdomain *xd = dev->xd;
	unsigned int depth = loadtest_ring_depth();
	int out_hop, in_hop;
	u16 sof_mask = BIT(LOADTEST_PDF_FRAME_START);
	u16 eof_mask = BIT(LOADTEST_PDF_FRAME_END);
	unsigned int ring_flags = RING_FLAG_FRAME;
	int ret, i;

	lr->dev = dev;
	lr->idx = idx;
	lr->frames_per_side = loadtest_frames_per_ring(depth,
						       READ_ONCE(loadtest_flood));
	atomic_set(&lr->tx_inflight, 0);
	atomic64_set(&lr->tx_bytes, 0);
	atomic64_set(&lr->rx_bytes, 0);

	if (READ_ONCE(loadtest_flood) && !loadtest_use_flood()) {
		pr_warn("ring %d: loadtest_flood requires raw_nhi=1 and loadtest_raw_batch=1\n",
			idx);
		return -EINVAL;
	}
	if (READ_ONCE(loadtest_flood) && frame_size > SZ_4K) {
		pr_warn("ring %d: loadtest_flood refuses frame_size=%d; NHI frame descriptors are limited to 4096 bytes\n",
			idx, frame_size);
		return -EINVAL;
	}

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

	/* RING_FLAG_E2E enables hardware end-to-end flow control: when our
	 * RX ring fills, the controller signals the matching TX ring on
	 * the *peer* controller to slow down. Without it, sustained TX
	 * outruns RX and the controller drops frames (we observed 1-3%
	 * loss in the no-E2E baseline). thunderbolt_net always sets E2E
	 * on its data ring; we mirror it. The RX ring's e2e_tx_hop arg
	 * is paired with the local TX ring's hop. */
	if (READ_ONCE(loadtest_e2e))
		ring_flags |= RING_FLAG_E2E;

	lr->tx_ring = tb_ring_alloc_tx(xd->tb->nhi, -1, depth, ring_flags);
	if (!lr->tx_ring) {
		ret = -ENOMEM;
		goto err_hopid;
	}

	lr->rx_ring = tb_ring_alloc_rx(xd->tb->nhi, -1, depth,
				       ring_flags,
				       READ_ONCE(loadtest_e2e) ?
				       lr->tx_ring->hop : 0,
				       sof_mask, eof_mask, NULL, NULL);
	if (!lr->rx_ring) {
		ret = -ENOMEM;
		goto err_tx_ring;
	}

	pr_info("ring %d: tx_ring->hop=%d rx_ring->hop=%d depth=%u frames=%d\n",
		idx, lr->tx_ring->hop, lr->rx_ring->hop, depth,
		lr->frames_per_side);

	ret = alloc_frames(lr, &lr->tx_frames, lr->frames_per_side, true);
	if (ret)
		goto err_rx_ring;
	ret = alloc_frames(lr, &lr->rx_frames, lr->frames_per_side, false);
	if (ret)
		goto err_tx_frames;

	/* Order matters: start rings BEFORE posting RX buffers
	 * (tb_ring_rx returns -ESHUTDOWN on a stopped ring), and call
	 * enable_paths LAST after both sides are ready to receive. This
	 * mirrors `tbnet_open` in drivers/net/thunderbolt/main.c. */
	tb_ring_start(lr->tx_ring);
	tb_ring_start(lr->rx_ring);

	if (loadtest_use_flood()) {
		ret = loadtest_flood_prime_rx(lr);
		if (ret) {
			pr_warn("ring %d: flood RX prime failed: %d\n",
				idx, ret);
			goto err_rings_started;
		}
	} else if (loadtest_use_raw_batch()) {
		LIST_HEAD(rx_submit);

		for (i = 0; i < lr->frames_per_side; i++) {
			struct loadtest_frame *lf = &lr->rx_frames[i];

			lf->frame.callback = rx_callback;
			list_add_tail(&lf->frame.list, &rx_submit);
		}
		if (READ_ONCE(loadtest_raw_unlocked))
			ret = usb4_rdma_nhi_raw_rx_batch_unlocked(lr->rx_ring,
								  &rx_submit);
		else
			ret = usb4_rdma_nhi_raw_rx_batch_atomic(lr->rx_ring,
								&rx_submit);
		if (ret || !list_empty(&rx_submit)) {
			struct ring_frame *frame, *tmp;

			pr_warn("ring %d: raw RX batch post failed: ret=%d remaining=%d\n",
				idx, ret, !list_empty(&rx_submit));
			list_for_each_entry_safe(frame, tmp, &rx_submit, list)
				list_del_init(&frame->list);
			goto err_rings_started;
		}
	} else {
		for (i = 0; i < lr->frames_per_side; i++) {
			struct loadtest_frame *lf = &lr->rx_frames[i];

			lf->frame.callback = rx_callback;
			ret = tb_ring_rx(lr->rx_ring, &lf->frame);
			if (ret) {
				pr_warn("ring %d: tb_ring_rx() returned %d at slot %d\n",
					idx, ret, i);
				goto err_rings_started;
			}
		}
	}

	/* TODO: real wire-protocol coordination. Today both peers happen
	 * to allocate matching hop numbers because they run identical
	 * code from identical state. Replace with a tb_xdomain_request
	 * exchange à la tbnet_login_request before deploying. */
	ret = tb_xdomain_enable_paths(xd, lr->out_hop, lr->tx_ring->hop,
				      lr->in_hop, lr->rx_ring->hop);
	if (ret) {
		pr_warn("ring %d: enable_paths failed: %d\n", idx, ret);
		goto err_rings_started;
	}

	return 0;

err_rings_started:
	tb_ring_stop(lr->tx_ring);
	tb_ring_stop(lr->rx_ring);
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
	if (lr->rx_thread) {
		kthread_stop(lr->rx_thread);
		lr->rx_thread = NULL;
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
		atomic64_set(&lr->tx_errors, 0);
		atomic64_set(&lr->rx_repost_errors, 0);
		lr->flood_tx_bytes = 0;
		lr->flood_rx_bytes = 0;
		lr->flood_tx_packets = 0;
		lr->flood_rx_packets = 0;
		lr->flood_tx_errors = 0;
		lr->flood_rx_errors = 0;
		lr->flood_tx_doorbells = 0;
		lr->flood_rx_doorbells = 0;
	}

	dev->start_time = ktime_get();

	if (loadtest_use_raw_batch() && READ_ONCE(loadtest_rx)) {
		for (i = 0; i < dev->nr_rings; i++) {
			struct loadtest_ring *lr = &dev->rings[i];

			lr->rx_thread = kthread_run(loadtest_use_flood() ?
						    flood_rx_thread_fn :
						    rx_thread_fn,
						    lr, "u4lt-rx-%d", i);
			if (IS_ERR(lr->rx_thread)) {
				int err = PTR_ERR(lr->rx_thread);

				lr->rx_thread = NULL;
				while (--i >= 0) {
					kthread_stop(dev->rings[i].rx_thread);
					dev->rings[i].rx_thread = NULL;
				}
				return err;
			}
		}
	}

	if (!READ_ONCE(loadtest_tx)) {
		dev->running = true;
		return 0;
	}

	for (i = 0; i < dev->nr_rings; i++) {
		struct loadtest_ring *lr = &dev->rings[i];
		lr->tx_thread = kthread_run(loadtest_use_flood() ?
					    flood_tx_thread_fn : tx_thread_fn,
					    lr,
					    "u4lt-tx-%d", i);
		if (IS_ERR(lr->tx_thread)) {
			int err = PTR_ERR(lr->tx_thread);
			lr->tx_thread = NULL;
			while (--i >= 0) {
				kthread_stop(dev->rings[i].tx_thread);
				dev->rings[i].tx_thread = NULL;
			}
			if (loadtest_use_raw_batch()) {
				for (i = 0; i < dev->nr_rings; i++) {
					if (dev->rings[i].rx_thread) {
						kthread_stop(dev->rings[i].rx_thread);
						dev->rings[i].rx_thread = NULL;
					}
				}
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
		if (dev->rings[i].rx_thread) {
			kthread_stop(dev->rings[i].rx_thread);
			dev->rings[i].rx_thread = NULL;
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
	u64 total_tx_doorbells = 0, total_rx_doorbells = 0;
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
	seq_printf(m, "ring_depth:  %u\n", loadtest_ring_depth());
	seq_printf(m, "frames/ring: %d\n",
		   dev->nr_rings > 0 ? dev->rings[0].frames_per_side : 0);
	seq_printf(m, "raw_batch:   %d\n", loadtest_use_raw_batch());
	seq_printf(m, "raw_unlocked:%d\n", READ_ONCE(loadtest_raw_unlocked));
	seq_printf(m, "flood:       %d\n", loadtest_use_flood());
	seq_printf(m, "flood_db_batch:%u\n",
		   READ_ONCE(loadtest_flood_doorbell_batch));
	seq_printf(m, "tx_enabled:  %d\n", READ_ONCE(loadtest_tx));
	seq_printf(m, "rx_enabled:  %d\n", READ_ONCE(loadtest_rx));
	seq_printf(m, "e2e:         %d\n", READ_ONCE(loadtest_e2e));
	seq_printf(m, "elapsed:     %llu ms\n", elapsed_ns / 1000000);
	seq_puts(m, "\n");

	seq_printf(m, "  %-4s %-10s %-12s %-12s %-12s %-12s %-12s %-12s %-12s\n",
		   "ring", "hop", "tx_bytes", "rx_bytes",
		   "tx_pkts", "rx_pkts", "rx_Gbps", "tx_err",
		   "rx_err");
	seq_puts(m, "  ------------------------------------------------------------------------------------------------------\n");

	for (i = 0; i < dev->nr_rings; i++) {
		struct loadtest_ring *lr = &dev->rings[i];
		u64 tb, rb, tp, rp, te, re;
		u64 gbps_int;
		u64 gbps_frac;

		if (loadtest_use_flood()) {
			tb = READ_ONCE(lr->flood_tx_bytes);
			rb = READ_ONCE(lr->flood_rx_bytes);
			tp = READ_ONCE(lr->flood_tx_packets);
			rp = READ_ONCE(lr->flood_rx_packets);
			te = READ_ONCE(lr->flood_tx_errors);
			re = READ_ONCE(lr->flood_rx_errors);
		} else {
			tb = atomic64_read(&lr->tx_bytes);
			rb = atomic64_read(&lr->rx_bytes);
			tp = atomic64_read(&lr->tx_packets);
			rp = atomic64_read(&lr->rx_packets);
			te = atomic64_read(&lr->tx_errors);
			re = atomic64_read(&lr->rx_repost_errors);
		}

		gbps_int = (rb * 8) / elapsed_ns;
		gbps_frac = ((rb * 8) % elapsed_ns) * 100 / elapsed_ns;

		seq_printf(m, "  %-4d %-10d %-12llu %-12llu %-12llu %-12llu %llu.%02llu      %-12llu %-12llu\n",
			   i, lr->hop, tb, rb, tp, rp,
			   gbps_int, gbps_frac, te, re);

		total_tx_bytes += tb;
		total_rx_bytes += rb;
		total_tx_pkts += tp;
		total_rx_pkts += rp;
		total_tx_doorbells += READ_ONCE(lr->flood_tx_doorbells);
		total_rx_doorbells += READ_ONCE(lr->flood_rx_doorbells);
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
		if (loadtest_use_flood())
			seq_printf(m, "FLOOD DOORBELLS: TX %llu, RX %llu\n",
				   total_tx_doorbells, total_rx_doorbells);
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
	/* Note: peer-event hook lives in main.c's usb4_rdma probe path, not
	 * here, so the active-peer counter doesn't double-count when both
	 * services bind to the same xdomain peer. */

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

	if (!enable) {
		pr_info("disabled (load with enable=1 to register tb_service)\n");
		return 0;
	}

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
	if (!enable)
		return;

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
