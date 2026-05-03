// SPDX-License-Identifier: GPL-2.0
/*
 * data.c — usb4_rdma data path: ring management, frame pool, RX dispatch.
 *
 * Each bound xdomain peer (a usb4rdma tb_service binding) gets as many
 * TX/RX ring pairs as the controller can spare. ibdev.c routes verbs
 * (post_send / post_recv) into this layer; on RX we parse the wire
 * header from wire.h and dispatch to the matching local QP.
 *
 * Concurrency model:
 *   - RX ring completions may be drained from a work item, a busy-poll
 *     kthread, or ib_poll_cq()'s process context. Callback dispatch is
 *     serialized per peer so multi-fragment SEND reassembly stays ordered.
 *     RX callbacks still must not sleep or copy to userspace directly.
 *   - QP table is RCU-protected for fast lookup on RX.
 *   - Per-CQ work-completion queue uses an irq-safe spinlock.
 *
 * Multi-lane policy: small/copied frames use round-robin lane selection.
 * Raw page streams are split into contiguous byte ranges and submitted
 * across all active lanes. RDMA READ responses put the byte offset in
 * hdr->remote_addr so the requester can reassemble out-of-order lanes.
 */

#define pr_fmt(fmt) "usb4_rdma/data: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/thunderbolt.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/hashtable.h>
#include <linux/rwlock_types.h>
#include <linux/rcupdate.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/hash.h>
#include <linux/math64.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/hardirq.h>
#include <linux/irq_work.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>

#include "usb4_rdma.h"
#include "wire.h"
#include "nhi_raw.h"

#define U4_DATA_RING_DEPTH_DEFAULT 256
#define U4_DATA_RING_DEPTH_MIN     16
#define U4_DATA_RING_DEPTH_MAX     4095
#define U4_DATA_FRAME_SLACK        32
#define U4_TX_ZCOPY_POOL_DEFAULT   256
#define U4_TX_ZCOPY_POOL_MAX       4096
#define U4_FRAME_SIZE              4096
#define U4_DATA_PDF_FRAME_START    1
#define U4_DATA_PDF_FRAME_END      2

/* We allocate local XDomain transmit HopIDs normally, exchange them with
 * the peer over the XDomain control channel, then allocate the peer's
 * transmit HopID as our receive HopID before enabling data paths. This
 * avoids relying on symmetric fixed HopIDs and lets the core IDA avoid
 * thunderbolt-net or other service allocations. */
#define U4_MAX_LANES_PER_SERVICE   2
#define U4_MAX_ACTIVE_LANES        4
#define U4_LOGIN_TIMEOUT_MS        1000
#define U4_LOGIN_RETRIES           5
#define U4_LOGIN_START_DELAY_MS    250
#define U4_LOGIN_WORK_RETRIES      3
#define U4_LOGIN_RETRY_DELAY_MS    1000
#define U4_TX_WAIT_TIMEOUT_MS      5000

static bool rx_busy_poll;
module_param(rx_busy_poll, bool, 0444);
MODULE_PARM_DESC(rx_busy_poll,
		 "busy-poll USB4 RX rings from a kernel thread for one-sided latency (default: false)");

static bool e2e_flow = true;
module_param(e2e_flow, bool, 0444);
MODULE_PARM_DESC(e2e_flow,
		 "enable NHI end-to-end ring flow control (default: true)");

static bool rx_drain_test;
module_param(rx_drain_test, bool, 0444);
MODULE_PARM_DESC(rx_drain_test,
		 "post only rx_drain_test_frames RX descriptors per lane to test E2E credit backpressure");

static uint rx_drain_test_frames = 1;
module_param(rx_drain_test_frames, uint, 0444);
MODULE_PARM_DESC(rx_drain_test_frames,
		 "RX descriptors per lane when rx_drain_test=1 (default: 1)");

static uint data_ring_depth = U4_DATA_RING_DEPTH_DEFAULT;
module_param(data_ring_depth, uint, 0444);
MODULE_PARM_DESC(data_ring_depth,
		 "NHI descriptor ring depth for RDMA data rings (default 256, max 4095)");

static uint data_frames_per_dir;
module_param(data_frames_per_dir, uint, 0444);
MODULE_PARM_DESC(data_frames_per_dir,
		 "preallocated staging frames per RDMA data direction; 0 = data_ring_depth - 32 (default)");

static bool rx_zcopy;
module_param(rx_zcopy, bool, 0444);
MODULE_PARM_DESC(rx_zcopy,
		 "post RDMA_WRITE raw-stream payloads directly into user MR pages (experimental, default: false)");

static uint rx_zcopy_min_bytes = SZ_16K;
module_param(rx_zcopy_min_bytes, uint, 0644);
MODULE_PARM_DESC(rx_zcopy_min_bytes,
		 "minimum RDMA_WRITE raw-stream size to use RX zero-copy (default: 16384, 0 = always)");

static uint rx_poll_opportunistic_lanes = 2;
module_param(rx_poll_opportunistic_lanes, uint, 0644);
MODULE_PARM_DESC(rx_poll_opportunistic_lanes,
		 "RX lanes to opportunistically scan from ib_poll_cq when no NHI interrupt is armed (default: 2, 0 = interrupt/irq_work only)");

static uint tx_zcopy_pool_frames = U4_TX_ZCOPY_POOL_DEFAULT;
module_param(tx_zcopy_pool_frames, uint, 0444);
MODULE_PARM_DESC(tx_zcopy_pool_frames,
		 "preallocated TX zero-copy frame descriptors per lane (default: 256, max: 4096)");

static bool tx_stream_affinity = true;
module_param(tx_stream_affinity, bool, 0644);
MODULE_PARM_DESC(tx_stream_affinity,
		 "keep each zero-copy page stream on one hashed lane when it fits in a ring window");

struct u4_data_frame {
	struct ring_frame frame;
	struct u4_data_peer *peer;
	void *buf;
	dma_addr_t dma;
	bool is_tx;
	atomic_t in_use;	/* 1 while submitted to ring, 0 when free */
};

struct u4_data_zcopy_frame {
	struct ring_frame frame;
	struct u4_data_peer *peer;
	struct list_head prep_link;
	dma_addr_t dma;
	u32 length;
	bool unmap_dma;
	usb4_rdma_data_done_fn done;
	void *done_ctx;
	bool pooled;
};

struct u4_data_rx_zcopy_stream {
	struct u4_data_peer *peer;
	struct u4_data_frame *header_frame;
	struct u4_wire_hdr hdr;
	usb4_rdma_data_rx_finish_fn finish;
	void *finish_ctx;
	atomic_t pending;
	u64 raw_base;
	u32 total_length;
	u32 posted_bytes;
	u32 fallback_remaining;
	bool error;
};

struct u4_data_rx_zcopy_frame {
	struct ring_frame frame;
	struct list_head prep_link;
	struct u4_data_rx_zcopy_stream *stream;
	dma_addr_t dma;
	u32 length;
	usb4_rdma_data_done_fn done;
	void *done_ctx;
};

struct u4_data_peer {
	struct list_head list;
	struct tb_service *svc;
	struct tb_xdomain *xd;
	int lane_idx;

	int out_hop;
	int in_hop;
	bool paths_enabled;
	struct tb_ring *tx_ring;
	struct tb_ring *rx_ring;
	unsigned int ring_depth;
	unsigned int frames_per_dir;

	struct u4_data_frame *tx_frames;
	struct u4_data_frame *rx_frames;
	struct u4_data_zcopy_frame *tx_zcopy_pool;
	struct list_head tx_zcopy_free;
	spinlock_t tx_zcopy_lock;
	int tx_zcopy_pool_count;
	int tx_zcopy_free_count;
	int rx_posted_initial;

	atomic_t tx_inflight;
	atomic_t refs;
	bool closing;
	struct mutex tx_lock;
	wait_queue_head_t tx_wait;
	wait_queue_head_t ref_wait;
	struct irq_work rx_poll_irq;
	struct task_struct *rx_poll_task;
	atomic_t rx_poll_armed;
	atomic_t rx_poll_busy;

	bool raw_pending;
	struct u4_wire_hdr raw_hdr;
	u64 raw_base;
	u32 raw_done;
	u32 raw_remaining;
	struct u4_data_rx_zcopy_stream *rx_zcopy_stream;

	/* Stats — exposed via debugfs */
	atomic64_t tx_frames_sent;
	atomic64_t rx_frames_recv;
	atomic64_t rx_frames_dropped;
	atomic64_t rx_invalid_hdr;
	atomic64_t rx_poll_armed_frames;
	atomic64_t rx_poll_opportunistic_frames;
	atomic64_t rx_poll_irq_frames;
	atomic64_t rx_poll_busy_frames;
	atomic64_t rx_desc_overrun;
	atomic64_t rx_repost_failed;
	atomic64_t rx_zcopy_streams;
	atomic64_t rx_zcopy_frames;
	atomic64_t rx_zcopy_bytes;
	atomic64_t rx_zcopy_fallback;
	atomic64_t rx_zcopy_errors;
	atomic64_t tx_zcopy_pool_hits;
	atomic64_t tx_zcopy_pool_misses;
};

enum u4_login_type {
	U4_LOGIN_REQUEST = 1,
	U4_LOGIN_RESPONSE = 2,
};

enum u4_login_status {
	U4_LOGIN_OK = 0,
	U4_LOGIN_NOT_READY = 1,
	U4_LOGIN_BAD_VERSION = 2,
	U4_LOGIN_NO_LANES = 3,
};

struct u4_login_header {
	u32 route_hi;
	u32 route_lo;
	u32 length_sn;
	uuid_t uuid;
	uuid_t initiator_uuid;
	uuid_t target_uuid;
	u32 type;
	u32 command_id;
};

#define U4_LOGIN_HDR_LENGTH_MASK GENMASK(5, 0)
#define U4_LOGIN_HDR_SN_MASK     GENMASK(28, 27)
#define U4_LOGIN_HDR_SN_SHIFT    27
#define U4_LOGIN_PROTO_VERSION   1

struct u4_login_lane {
	u32 lane_idx;
	u32 transmit_path;
};

struct u4_login_request {
	struct u4_login_header hdr;
	u32 proto_version;
	u32 lane_count;
	struct u4_login_lane lanes[U4_MAX_LANES_PER_SERVICE];
};

struct u4_login_response {
	struct u4_login_header hdr;
	u32 status;
	u32 lane_count;
	struct u4_login_lane lanes[U4_MAX_LANES_PER_SERVICE];
};

struct u4_login_ctx {
	struct list_head list;
	struct tb_service *svc;
	struct tb_xdomain *xd;
	struct delayed_work work;
	int lane_count;
	struct u4_data_peer *lanes[U4_MAX_LANES_PER_SERVICE];
	struct mutex lock;
	struct completion remote_ready;
	int remote_count;
	int remote_tx_path[U4_MAX_LANES_PER_SERVICE];
	int login_attempts;
	bool closing;
	bool notified_joined;
};

static LIST_HEAD(peer_list);
static DEFINE_RWLOCK(peer_lock);
static atomic_t peer_rr = ATOMIC_INIT(0);
static atomic_t peer_count = ATOMIC_INIT(0);
static atomic_t rx_poll_armed_count = ATOMIC_INIT(0);
static LIST_HEAD(login_ctx_list);
static DEFINE_MUTEX(login_ctx_lock);
static atomic_t login_command_id = ATOMIC_INIT(0);

/* Keep this in sync with main.c's advertised service UUID. XDomain
 * protocol dispatch keys off the UUID embedded in the control packet. */
static const uuid_t u4_login_uuid =
	UUID_INIT(0x7c2c8f1e, 0x5b4d, 0x4a01,
		  0x9f, 0x3a, 0x2b, 0x8e, 0x6d, 0x4c, 0x1a, 0x07);

static struct tb_protocol_handler u4_login_handler;
static bool u4_login_handler_registered;
static struct dentry *data_debugfs_root;

/* QP routing — RX dispatch finds the local QP by qp_num. */
struct u4_data_qp_entry {
	u32 qp_num;
	struct u4_data_peer *rail;	/* NULL = legacy shared device */
	void *qp;	/* opaque ib_qp; ibdev.c interprets */
	struct hlist_node node;
	struct rcu_head rcu;
};

#define U4_DATA_QP_HASH_BITS  6
static DEFINE_HASHTABLE(u4_data_qp_table, U4_DATA_QP_HASH_BITS);
static DEFINE_SPINLOCK(u4_data_qp_lock);

/* RX dispatcher — called from ibdev.c (registered via init). */
static void (*u4_data_rx_handler)(void *qp,
				  const struct u4_wire_hdr *hdr,
				  const void *payload, u32 length);
static usb4_rdma_data_rx_zcopy_prepare_fn u4_data_rx_zcopy_prepare;
static void u4_data_rx_complete(struct tb_ring *ring, struct ring_frame *frame,
				bool canceled);
static void u4_data_rx_complete_one(struct tb_ring *ring,
				    struct ring_frame *frame, bool canceled,
				    struct list_head *repost_list);
static void u4_data_repost_rx_list(struct u4_data_peer *p,
				   struct list_head *frames);

static atomic64_t rx_poll_calls;
static atomic64_t rx_poll_armed_scans;
static atomic64_t rx_poll_opportunistic_scans;
static atomic64_t rx_poll_skipped;
static atomic64_t tx_stream_affinity_used;
static atomic64_t tx_stream_affinity_fallback;

static bool u4_data_warn_process_tx_from_atomic(const char *where)
{
	if (!WARN_ON_ONCE(in_interrupt()))
		return false;

	pr_warn_ratelimited("%s called from interrupt context; process-context TX path would sleep\n",
			    where);
	return true;
}

static void u4_data_tx_wake(struct u4_data_peer *p)
{
	if (wq_has_sleeper(&p->tx_wait))
		wake_up(&p->tx_wait);
}

static struct u4_data_peer *u4_data_peer_get_at(unsigned int target)
{
	struct u4_data_peer *p, *pick = NULL;
	unsigned long flags;
	int count = atomic_read(&peer_count);
	int i = 0;

	if (!count)
		return NULL;

	read_lock_irqsave(&peer_lock, flags);
	target %= count;
	list_for_each_entry(p, &peer_list, list) {
		if (READ_ONCE(p->closing))
			continue;
		if (i++ == target) {
			atomic_inc(&p->refs);
			pick = p;
			break;
		}
	}
	read_unlock_irqrestore(&peer_lock, flags);
	return pick;
}

static struct u4_data_peer *u4_data_peer_get(void)
{
	return u4_data_peer_get_at(atomic_inc_return(&peer_rr));
}

static void u4_data_peer_put(struct u4_data_peer *p);

bool usb4_rdma_data_rail_get(struct u4_data_peer *rail)
{
	if (!rail)
		return true;
	if (READ_ONCE(rail->closing))
		return false;
	atomic_inc(&rail->refs);
	if (READ_ONCE(rail->closing)) {
		u4_data_peer_put(rail);
		return false;
	}
	return true;
}

void usb4_rdma_data_rail_put(struct u4_data_peer *rail)
{
	if (rail)
		u4_data_peer_put(rail);
}

int usb4_rdma_data_rail_index(struct u4_data_peer *rail)
{
	const char *name;
	unsigned int domain;

	if (!rail || !rail->svc)
		return -ENODEV;

	name = dev_name(&rail->svc->dev);
	if (sscanf(name, "%u-", &domain) != 1)
		return -EINVAL;
	if (rail->lane_idx < 0 || rail->lane_idx >= U4_MAX_LANES_PER_SERVICE)
		return -EINVAL;
	if (domain > INT_MAX / U4_MAX_LANES_PER_SERVICE)
		return -ERANGE;

	return domain * U4_MAX_LANES_PER_SERVICE + rail->lane_idx;
}

static struct u4_data_peer *u4_data_peer_get_by_qp(struct u4_data_peer *rail,
						   u32 src_qp, u32 dest_qp)
{
	if (rail) {
		if (!usb4_rdma_data_rail_get(rail))
			return NULL;
		return rail;
	}
	return u4_data_peer_get_at(hash_32(src_qp ^ dest_qp, 32));
}

static u32 u4_data_tx_desc_size(u32 len)
{
	if (len >= U4_FRAME_SIZE)
		return 0; /* NHI encoding for a full 4096-byte frame. */
	return max_t(u32, len, TB_FRAME_SIZE);
}

static u32 u4_data_desc_len(u32 desc_size)
{
	return desc_size ?: U4_FRAME_SIZE;
}

static int u4_data_peers_get(struct u4_data_peer **peers, int max)
{
	struct u4_data_peer *p;
	unsigned long flags;
	int n = 0;

	read_lock_irqsave(&peer_lock, flags);
	list_for_each_entry(p, &peer_list, list) {
		if (READ_ONCE(p->closing))
			continue;
		atomic_inc(&p->refs);
		peers[n++] = p;
		if (n == max)
			break;
	}
	read_unlock_irqrestore(&peer_lock, flags);
	return n;
}

static void u4_data_peer_put(struct u4_data_peer *p)
{
	if (atomic_dec_return(&p->refs) == 1)
		wake_up(&p->ref_wait);
}

static unsigned int u4_data_ring_depth(void)
{
	return clamp_t(uint, READ_ONCE(data_ring_depth),
		       U4_DATA_RING_DEPTH_MIN, U4_DATA_RING_DEPTH_MAX);
}

static unsigned int u4_data_auto_frames_per_dir(unsigned int depth)
{
	if (depth > U4_DATA_FRAME_SLACK)
		return depth - U4_DATA_FRAME_SLACK;

	return depth - 1;
}

static unsigned int u4_data_frames_per_dir(unsigned int depth)
{
	uint frames = READ_ONCE(data_frames_per_dir);

	if (!frames)
		frames = u4_data_auto_frames_per_dir(depth);

	return clamp_t(uint, frames, 2, depth - 1);
}

static u64 u4_tx_stream_affinity_max(struct u4_data_peer *p)
{
	if (p->frames_per_dir <= 1)
		return 0;

	return (u64)(p->frames_per_dir - 1) * U4_FRAME_SIZE;
}

static bool u4_data_tx_room(struct u4_data_peer *p, int needed)
{
	return atomic_read(&p->tx_inflight) <= p->frames_per_dir - needed;
}

static int u4_data_initial_rx_frames(struct u4_data_peer *p)
{
	if (READ_ONCE(rx_drain_test))
		return clamp_t(uint, READ_ONCE(rx_drain_test_frames), 1,
			       p->frames_per_dir);
	if (READ_ONCE(rx_zcopy))
		return 1;
	return p->frames_per_dir;
}

static int u4_data_poll_rx_peer(struct u4_data_peer *p)
{
	struct ring_frame *frame, *tmp;
	LIST_HEAD(done_frames);
	LIST_HEAD(repost_frames);
	int done = 0;
	bool armed;

	if (!p->rx_ring)
		return 0;
	if (atomic_cmpxchg(&p->rx_poll_busy, 0, 1))
		return 0;

	while (usb4_rdma_nhi_raw_poll_batch(p->rx_ring, &done_frames,
					    p->ring_depth)) {
		list_for_each_entry_safe(frame, tmp, &done_frames, list) {
			list_del_init(&frame->list);
			if (frame->callback == u4_data_rx_complete)
				u4_data_rx_complete_one(p->rx_ring, frame,
							false, &repost_frames);
			else if (frame->callback)
				frame->callback(p->rx_ring, frame, false);
			done++;
		}
		u4_data_repost_rx_list(p, &repost_frames);
	}

	armed = atomic_xchg(&p->rx_poll_armed, 0);
	if (armed) {
		atomic_dec(&rx_poll_armed_count);
		usb4_rdma_nhi_raw_poll_complete(p->rx_ring);
	}
	atomic_set(&p->rx_poll_busy, 0);
	return done;
}

static void u4_data_rx_poll_irq(struct irq_work *work)
{
	struct u4_data_peer *p =
		container_of(work, struct u4_data_peer, rx_poll_irq);
	int done;

	done = u4_data_poll_rx_peer(p);
	if (done)
		atomic64_add(done, &p->rx_poll_irq_frames);
}

static void u4_data_start_rx_poll(void *data)
{
	struct u4_data_peer *p = data;

	if (atomic_cmpxchg(&p->rx_poll_armed, 0, 1) == 0)
		atomic_inc(&rx_poll_armed_count);
	irq_work_queue(&p->rx_poll_irq);
}

static int u4_data_rx_poll_thread(void *data)
{
	struct u4_data_peer *p = data;
	unsigned int empty_polls = 0;

	while (!kthread_should_stop()) {
		int done = u4_data_poll_rx_peer(p);

		if (done) {
			atomic64_add(done, &p->rx_poll_busy_frames);
			empty_polls = 0;
			continue;
		}

		cpu_relax();
		if (++empty_polls >= 4096) {
			empty_polls = 0;
			cond_resched();
		}
	}
	return 0;
}

static int u4_data_zcopy_frame_count(struct list_head *frames)
{
	struct u4_data_zcopy_frame *zf;
	int count = 0;

	list_for_each_entry(zf, frames, prep_link)
		count++;
	return count;
}

static u32 u4_data_move_zcopy_chunk(struct list_head *from,
				    struct list_head *to,
				    int max_frames)
{
	struct u4_data_zcopy_frame *zf;
	u32 length = 0;
	int count = 0;

	while (count < max_frames && !list_empty(from)) {
		zf = list_first_entry(from, struct u4_data_zcopy_frame,
				      prep_link);
		list_move_tail(&zf->prep_link, to);
		length += zf->length;
		count++;
	}
	return length;
}

static void u4_login_fill_header(struct u4_login_header *hdr, u64 route,
				  u8 sequence, const uuid_t *initiator_uuid,
				  const uuid_t *target_uuid,
				  enum u4_login_type type, size_t size,
				  u32 command_id)
{
	u32 length_sn;

	length_sn = (size - 3 * sizeof(u32)) / sizeof(u32);
	length_sn |= (sequence << U4_LOGIN_HDR_SN_SHIFT) & U4_LOGIN_HDR_SN_MASK;

	hdr->route_hi = upper_32_bits(route);
	hdr->route_lo = lower_32_bits(route);
	hdr->length_sn = length_sn;
	uuid_copy(&hdr->uuid, &u4_login_uuid);
	uuid_copy(&hdr->initiator_uuid, initiator_uuid);
	uuid_copy(&hdr->target_uuid, target_uuid);
	hdr->type = type;
	hdr->command_id = command_id;
}

static bool u4_login_header_matches_ctx(struct u4_login_ctx *ctx,
					const struct u4_login_header *hdr,
					u64 route)
{
	if (route != ctx->xd->route)
		return false;
	if (!uuid_equal(&hdr->initiator_uuid, ctx->xd->remote_uuid))
		return false;
	if (!uuid_equal(&hdr->target_uuid, ctx->xd->local_uuid))
		return false;
	return true;
}

static int u4_login_handle_packet(const void *buf, size_t size, void *data)
{
	const struct u4_login_request *req = buf;
	struct u4_login_response res = {};
	struct tb_xdomain *xd = NULL;
	struct u4_login_ctx *ctx;
	u32 sequence;
	u64 route;
	int ret = 0;
	int i;

	if (size < sizeof(struct u4_login_header))
		return 0;
	if (!uuid_equal(&req->hdr.uuid, &u4_login_uuid))
		return 0;
	if (req->hdr.type != U4_LOGIN_REQUEST)
		return 1;
	if (size < sizeof(*req))
		return 1;

	route = ((u64)req->hdr.route_hi << 32) | req->hdr.route_lo;
	route &= ~BIT_ULL(63);
	sequence = req->hdr.length_sn & U4_LOGIN_HDR_SN_MASK;
	sequence >>= U4_LOGIN_HDR_SN_SHIFT;

	mutex_lock(&login_ctx_lock);
	list_for_each_entry(ctx, &login_ctx_list, list) {
		u32 req_count;
		u32 resp_count;

		if (!u4_login_header_matches_ctx(ctx, &req->hdr, route))
			continue;

		xd = tb_xdomain_get(ctx->xd);
		u4_login_fill_header(&res.hdr, route, sequence, xd->local_uuid,
				     xd->remote_uuid, U4_LOGIN_RESPONSE,
				     sizeof(res), req->hdr.command_id);

		if (req->proto_version != U4_LOGIN_PROTO_VERSION) {
			res.status = U4_LOGIN_BAD_VERSION;
			break;
		}

		req_count = req->lane_count;
		if (!req_count || req_count > U4_MAX_LANES_PER_SERVICE) {
			res.status = U4_LOGIN_NO_LANES;
			break;
		}

		mutex_lock(&ctx->lock);
		resp_count = min_t(u32, req_count, ctx->lane_count);
		if (!resp_count) {
			res.status = U4_LOGIN_NOT_READY;
			mutex_unlock(&ctx->lock);
			break;
		}
		for (i = 0; i < resp_count; i++) {
			if (!req->lanes[i].transmit_path) {
				res.status = U4_LOGIN_NO_LANES;
				mutex_unlock(&ctx->lock);
				goto out_unlock;
			}
		}

		ctx->remote_count = resp_count;
		for (i = 0; i < resp_count; i++) {
			ctx->remote_tx_path[i] = req->lanes[i].transmit_path;
			res.lanes[i].lane_idx = i;
			res.lanes[i].transmit_path = ctx->lanes[i]->out_hop;
		}
		res.status = U4_LOGIN_OK;
		res.lane_count = resp_count;
		complete_all(&ctx->remote_ready);
		if (!ctx->notified_joined && !READ_ONCE(ctx->closing)) {
			ctx->login_attempts = 0;
			queue_delayed_work(system_long_wq, &ctx->work, 0);
		}
		mutex_unlock(&ctx->lock);
		break;
	}
out_unlock:
	mutex_unlock(&login_ctx_lock);

	if (!xd)
		return 0;

	ret = tb_xdomain_response(xd, &res, sizeof(res),
				  TB_CFG_PKG_XDOMAIN_RESP);
	if (ret)
		dev_warn(&xd->dev, "data: login response failed: %d\n", ret);
	tb_xdomain_put(xd);
	return 1;
}

static int u4_login_parse_response(struct u4_login_ctx *ctx,
				   const struct u4_login_response *res,
				   int *remote_tx_path, int *remote_count)
{
	u64 route;
	u32 count;
	int i;

	if (!uuid_equal(&res->hdr.uuid, &u4_login_uuid))
		return -EPROTO;
	if (res->hdr.type != U4_LOGIN_RESPONSE)
		return -EPROTO;
	if (!uuid_equal(&res->hdr.initiator_uuid, ctx->xd->remote_uuid))
		return -EPROTO;
	if (!uuid_equal(&res->hdr.target_uuid, ctx->xd->local_uuid))
		return -EPROTO;

	route = ((u64)res->hdr.route_hi << 32) | res->hdr.route_lo;
	route &= ~BIT_ULL(63);
	if (route != ctx->xd->route)
		return -EPROTO;

	switch (res->status) {
	case U4_LOGIN_OK:
		break;
	case U4_LOGIN_BAD_VERSION:
		return -EPROTONOSUPPORT;
	case U4_LOGIN_NO_LANES:
	case U4_LOGIN_NOT_READY:
		return -EAGAIN;
	default:
		return -EREMOTEIO;
	}

	count = res->lane_count;
	if (!count || count > U4_MAX_LANES_PER_SERVICE ||
	    count > ctx->lane_count)
		return -EPROTO;

	for (i = 0; i < count; i++) {
		if (!res->lanes[i].transmit_path)
			return -EPROTO;
		remote_tx_path[i] = res->lanes[i].transmit_path;
	}
	*remote_count = count;
	return 0;
}

static int u4_login_request(struct u4_login_ctx *ctx, int *remote_tx_path,
			    int *remote_count)
{
	struct u4_login_response res;
	struct u4_login_request req;
	int ret = -ETIMEDOUT;
	int retry, i;

	for (retry = 0; retry < U4_LOGIN_RETRIES; retry++) {
		memset(&req, 0, sizeof(req));
		memset(&res, 0, sizeof(res));

		u4_login_fill_header(&req.hdr, ctx->xd->route, retry % 4,
				     ctx->xd->local_uuid, ctx->xd->remote_uuid,
				     U4_LOGIN_REQUEST, sizeof(req),
				     atomic_inc_return(&login_command_id));
		req.proto_version = U4_LOGIN_PROTO_VERSION;
		req.lane_count = ctx->lane_count;
		for (i = 0; i < ctx->lane_count; i++) {
			req.lanes[i].lane_idx = i;
			req.lanes[i].transmit_path = ctx->lanes[i]->out_hop;
		}

		/* Use XDOMAIN_REQ for the login request so simultaneous
		 * peer logins are dispatched to the protocol handler instead
		 * of being mistaken for the response to our own request. */
		ret = tb_xdomain_request(ctx->xd, &req, sizeof(req),
					 TB_CFG_PKG_XDOMAIN_REQ, &res,
					 sizeof(res), TB_CFG_PKG_XDOMAIN_RESP,
					 U4_LOGIN_TIMEOUT_MS);
		if (!ret)
			ret = u4_login_parse_response(ctx, &res,
						      remote_tx_path,
						      remote_count);
		if (!ret)
			return 0;
		if (ret != -ETIMEDOUT && ret != -EAGAIN)
			break;
		msleep(100);
	}

	if (wait_for_completion_timeout(&ctx->remote_ready,
					msecs_to_jiffies(U4_LOGIN_TIMEOUT_MS))) {
		mutex_lock(&ctx->lock);
		if (ctx->remote_count > 0) {
			*remote_count = ctx->remote_count;
			for (i = 0; i < ctx->remote_count; i++)
				remote_tx_path[i] = ctx->remote_tx_path[i];
			ret = 0;
		}
		mutex_unlock(&ctx->lock);
	}

	return ret;
}

static struct u4_data_frame *u4_data_claim_tx_frame(struct u4_data_peer *p)
{
	int i;

	for (i = 0; i < p->frames_per_dir; i++) {
		struct u4_data_frame *cand = &p->tx_frames[i];

		if (atomic_cmpxchg(&cand->in_use, 0, 1) == 0)
			return cand;
	}
	return NULL;
}

/* ----- frame pool helpers ----------------------------------------- */

static int alloc_frames(struct u4_data_peer *p,
			struct u4_data_frame **out, int n, bool tx)
{
	struct u4_data_frame *frames;
	struct device *dma_dev = tb_ring_dma_device(p->tx_ring ?: p->rx_ring);
	int i;

	frames = kcalloc(n, sizeof(*frames), GFP_KERNEL);
	if (!frames)
		return -ENOMEM;
	for (i = 0; i < n; i++) {
		struct u4_data_frame *f = &frames[i];

		f->peer = p;
		f->is_tx = tx;
		f->buf = kmalloc(U4_FRAME_SIZE, GFP_KERNEL);
		if (!f->buf)
			goto err;
		f->dma = dma_map_single(dma_dev, f->buf, U4_FRAME_SIZE,
					tx ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
		if (dma_mapping_error(dma_dev, f->dma)) {
			kfree(f->buf);
			f->buf = NULL;
			goto err;
		}
		f->frame.buffer_phy = f->dma;
		/* ring_frame.size is 12 bits; zero is the hardware/core
		 * encoding we already treat as a full 4096-byte frame on RX. */
		f->frame.size = 0;
		INIT_LIST_HEAD(&f->frame.list);
	}
	*out = frames;
	return 0;
err:
	while (--i >= 0) {
		struct u4_data_frame *f = &frames[i];
		dma_unmap_single(dma_dev, f->dma, U4_FRAME_SIZE,
				 tx ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
		kfree(f->buf);
	}
	kfree(frames);
	return -ENOMEM;
}

static void free_frames(struct u4_data_peer *p,
			struct u4_data_frame *frames, int n, bool tx)
{
	struct device *dma_dev;
	int i;

	if (!frames)
		return;
	dma_dev = tb_ring_dma_device(p->tx_ring ?: p->rx_ring);
	for (i = 0; i < n; i++) {
		struct u4_data_frame *f = &frames[i];
		if (f->buf) {
			dma_unmap_single(dma_dev, f->dma, U4_FRAME_SIZE,
					 tx ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
			kfree(f->buf);
		}
	}
	kfree(frames);
}

static void u4_data_init_tx_zcopy_pool(struct u4_data_peer *p)
{
	int count = clamp_t(uint, READ_ONCE(tx_zcopy_pool_frames),
			    p->frames_per_dir,
			    U4_TX_ZCOPY_POOL_MAX);
	int i;

	INIT_LIST_HEAD(&p->tx_zcopy_free);
	spin_lock_init(&p->tx_zcopy_lock);
	p->tx_zcopy_pool_count = count;
	p->tx_zcopy_pool = kcalloc(count, sizeof(*p->tx_zcopy_pool),
				   GFP_KERNEL);
	if (!p->tx_zcopy_pool) {
		p->tx_zcopy_pool_count = 0;
		return;
	}

	for (i = 0; i < count; i++) {
		struct u4_data_zcopy_frame *zf = &p->tx_zcopy_pool[i];

		zf->peer = p;
		zf->pooled = true;
		INIT_LIST_HEAD(&zf->prep_link);
		INIT_LIST_HEAD(&zf->frame.list);
		list_add_tail(&zf->prep_link, &p->tx_zcopy_free);
	}
	p->tx_zcopy_free_count = count;
}

static void u4_data_free_tx_zcopy_pool(struct u4_data_peer *p)
{
	if (!p->tx_zcopy_pool)
		return;
	if (p->tx_zcopy_free_count != p->tx_zcopy_pool_count)
		dev_warn(&p->svc->dev,
			 "data: lane %d freeing TX zcopy pool with %d/%d free\n",
			 p->lane_idx, p->tx_zcopy_free_count,
			 p->tx_zcopy_pool_count);
	kfree(p->tx_zcopy_pool);
	p->tx_zcopy_pool = NULL;
	p->tx_zcopy_pool_count = 0;
	p->tx_zcopy_free_count = 0;
	INIT_LIST_HEAD(&p->tx_zcopy_free);
}

static struct u4_data_zcopy_frame *
u4_data_alloc_tx_zcopy_frame(struct u4_data_peer *p, gfp_t gfp)
{
	struct u4_data_zcopy_frame *zf = NULL;
	unsigned long flags;

	spin_lock_irqsave(&p->tx_zcopy_lock, flags);
	if (!list_empty(&p->tx_zcopy_free)) {
		zf = list_first_entry(&p->tx_zcopy_free,
				      struct u4_data_zcopy_frame, prep_link);
		list_del_init(&zf->prep_link);
		p->tx_zcopy_free_count--;
	}
	spin_unlock_irqrestore(&p->tx_zcopy_lock, flags);

	if (zf) {
		memset(zf, 0, sizeof(*zf));
		zf->peer = p;
		zf->pooled = true;
		INIT_LIST_HEAD(&zf->prep_link);
		INIT_LIST_HEAD(&zf->frame.list);
		atomic64_inc(&p->tx_zcopy_pool_hits);
		return zf;
	}

	zf = kzalloc(sizeof(*zf), gfp);
	if (zf) {
		zf->peer = p;
		INIT_LIST_HEAD(&zf->prep_link);
		INIT_LIST_HEAD(&zf->frame.list);
	}
	atomic64_inc(&p->tx_zcopy_pool_misses);
	return zf;
}

static void u4_data_free_tx_zcopy_frame(struct u4_data_zcopy_frame *zf)
{
	struct u4_data_peer *p;
	unsigned long flags;

	if (!zf)
		return;
	if (!zf->pooled) {
		kfree(zf);
		return;
	}

	p = zf->peer;
	zf->peer = p;
	zf->pooled = true;
	INIT_LIST_HEAD(&zf->prep_link);
	INIT_LIST_HEAD(&zf->frame.list);

	spin_lock_irqsave(&p->tx_zcopy_lock, flags);
	list_add_tail(&zf->prep_link, &p->tx_zcopy_free);
	p->tx_zcopy_free_count++;
	spin_unlock_irqrestore(&p->tx_zcopy_lock, flags);
}

/* ----- ring callbacks --------------------------------------------- */

static void u4_data_tx_complete(struct tb_ring *ring, struct ring_frame *frame,
				bool canceled)
{
	struct u4_data_frame *f = container_of(frame, typeof(*f), frame);
	struct device *dma_dev = tb_ring_dma_device(ring);

	dma_sync_single_for_cpu(dma_dev, f->dma, U4_FRAME_SIZE, DMA_TO_DEVICE);
	atomic_dec(&f->peer->tx_inflight);
	atomic_set(&f->in_use, 0);
	u4_data_tx_wake(f->peer);
	if (!canceled)
		atomic64_inc(&f->peer->tx_frames_sent);
}

static void u4_data_zcopy_tx_complete(struct tb_ring *ring,
				      struct ring_frame *frame, bool canceled)
{
	struct u4_data_zcopy_frame *zf =
		container_of(frame, typeof(*zf), frame);
	struct device *dma_dev = tb_ring_dma_device(ring);

	if (zf->unmap_dma)
		dma_unmap_page(dma_dev, zf->dma, zf->length, DMA_TO_DEVICE);
	atomic_dec(&zf->peer->tx_inflight);
	u4_data_tx_wake(zf->peer);
	if (!canceled)
		atomic64_inc(&zf->peer->tx_frames_sent);
	if (zf->done)
		zf->done(zf->done_ctx);
	u4_data_free_tx_zcopy_frame(zf);
}

static void *u4_data_lookup_qp_rcu(u32 qp_num, struct u4_data_peer *rail)
{
	struct u4_data_qp_entry *qe;
	void *fallback = NULL;

	hash_for_each_possible_rcu(u4_data_qp_table, qe, node, qp_num) {
		if (qe->qp_num != qp_num)
			continue;
		if (qe->rail == rail)
			return qe->qp;
		if (!qe->rail)
			fallback = qe->qp;
	}
	return fallback;
}

static void u4_data_repost_rx_frame(struct u4_data_peer *p,
				    struct u4_data_frame *f)
{
	struct device *dma_dev;

	if (READ_ONCE(p->closing) || !p->rx_ring)
		return;

	dma_dev = tb_ring_dma_device(p->rx_ring);
	dma_sync_single_for_device(dma_dev, f->dma, U4_FRAME_SIZE,
				   DMA_FROM_DEVICE);
	if (usb4_rdma_nhi_raw_rx(p->rx_ring, &f->frame))
		atomic64_inc(&p->rx_repost_failed);
}

static void u4_data_repost_rx_list(struct u4_data_peer *p,
				   struct list_head *frames)
{
	struct ring_frame *frame, *tmp;
	struct device *dma_dev;
	int ret;

	if (list_empty(frames))
		return;
	if (READ_ONCE(p->closing) || !p->rx_ring)
		goto drop;

	dma_dev = tb_ring_dma_device(p->rx_ring);
	list_for_each_entry(frame, frames, list) {
		struct u4_data_frame *f =
			container_of(frame, struct u4_data_frame, frame);

		dma_sync_single_for_device(dma_dev, f->dma, U4_FRAME_SIZE,
					   DMA_FROM_DEVICE);
	}

	ret = usb4_rdma_nhi_raw_rx_batch(p->rx_ring, frames);
	if (!ret)
		return;

drop:
	list_for_each_entry_safe(frame, tmp, frames, list) {
		list_del_init(&frame->list);
		atomic64_inc(&p->rx_repost_failed);
	}
}

static void
u4_data_free_rx_zcopy_list(struct device *dma_dev, struct list_head *frames)
{
	struct u4_data_rx_zcopy_frame *zf, *tmp;

	list_for_each_entry_safe(zf, tmp, frames, prep_link) {
		list_del(&zf->prep_link);
		dma_unmap_page(dma_dev, zf->dma, zf->length,
			       DMA_FROM_DEVICE);
		if (zf->done)
			zf->done(zf->done_ctx);
		kfree(zf);
	}
}

static void u4_data_rx_zcopy_stream_done(struct u4_data_rx_zcopy_stream *s,
					 bool canceled)
{
	struct u4_data_peer *p = s->peer;

	if (s->fallback_remaining && !canceled && !READ_ONCE(p->closing)) {
		p->raw_hdr = s->hdr;
		p->raw_hdr.flags &= ~U4_F_RAW_STREAM;
		p->raw_base = s->raw_base + s->posted_bytes;
		p->raw_done = 0;
		p->raw_remaining = s->fallback_remaining;
		p->raw_pending = true;
	}

	if (s->finish)
		s->finish(s->finish_ctx);
	WRITE_ONCE(p->rx_zcopy_stream, NULL);

	if (!canceled && !READ_ONCE(p->closing))
		u4_data_repost_rx_frame(p, s->header_frame);
	kfree(s);
}

static void u4_data_rx_zcopy_complete(struct tb_ring *ring,
				      struct ring_frame *frame, bool canceled)
{
	struct u4_data_rx_zcopy_frame *zf =
		container_of(frame, typeof(*zf), frame);
	struct u4_data_rx_zcopy_stream *s = zf->stream;
	struct u4_data_peer *p = s->peer;
	struct device *dma_dev = tb_ring_dma_device(ring);
	u32 frame_len = frame->size ?: U4_FRAME_SIZE;

	if (!canceled) {
		atomic64_inc(&p->rx_frames_recv);
		if (frame->flags & RING_DESC_BUFFER_OVERRUN) {
			atomic64_inc(&p->rx_desc_overrun);
			s->error = true;
		} else if (frame_len != zf->length) {
			atomic64_inc(&p->rx_invalid_hdr);
			s->error = true;
		} else {
			atomic64_inc(&p->rx_zcopy_frames);
			atomic64_add(zf->length, &p->rx_zcopy_bytes);
		}
	}

	dma_unmap_page(dma_dev, zf->dma, zf->length, DMA_FROM_DEVICE);
	if (zf->done)
		zf->done(zf->done_ctx);
	if (atomic_dec_and_test(&s->pending))
		u4_data_rx_zcopy_stream_done(s, canceled);
	kfree(zf);
}

static int
u4_data_prepare_rx_zcopy(struct u4_data_peer *p, void *target_qp,
			 const struct u4_wire_hdr *hdr, u32 total_length,
			 struct list_head *frames,
			 struct u4_data_rx_zcopy_stream **stream_out)
{
	usb4_rdma_data_rx_next_page_fn next = NULL;
	usb4_rdma_data_rx_finish_fn finish = NULL;
	struct u4_data_rx_zcopy_stream *s;
	struct device *dma_dev = tb_ring_dma_device(p->rx_ring);
	void *next_ctx = NULL;
	u32 remaining = total_length;
	int frame_count = 0;
	int ret;

	ret = u4_data_rx_zcopy_prepare(target_qp, hdr, total_length,
				       &next, &next_ctx, &finish);
	if (ret)
		return ret;
	if (!next) {
		if (finish)
			finish(next_ctx);
		return -EINVAL;
	}

	s = kzalloc(sizeof(*s), GFP_ATOMIC);
	if (!s) {
		if (finish)
			finish(next_ctx);
		return -ENOMEM;
	}
	s->peer = p;
	s->hdr = *hdr;
	s->finish = finish;
	s->finish_ctx = next_ctx;
	s->raw_base = le64_to_cpu(hdr->remote_addr);
	s->total_length = total_length;
	atomic_set(&s->pending, 0);

	while (remaining) {
		struct u4_data_rx_zcopy_frame *zf;
		struct page *page = NULL;
		u32 page_off = 0;
		u32 length = 0;
		usb4_rdma_data_done_fn done = NULL;
		void *done_ctx = NULL;

		if (++frame_count > p->frames_per_dir) {
			ret = -E2BIG;
			goto err;
		}

		ret = next(next_ctx, &page, &page_off, &length, &done,
			   &done_ctx);
		if (ret)
			goto err;
		if (!page || !length || length > U4_FRAME_SIZE ||
		    length > remaining || page_off > PAGE_SIZE ||
		    length > PAGE_SIZE - page_off) {
			if (done)
				done(done_ctx);
			ret = -EINVAL;
			goto err;
		}

		zf = kzalloc(sizeof(*zf), GFP_ATOMIC);
		if (!zf) {
			if (done)
				done(done_ctx);
			ret = -ENOMEM;
			goto err;
		}
		zf->stream = s;
		zf->length = length;
		zf->done = done;
		zf->done_ctx = done_ctx;
		zf->dma = dma_map_page(dma_dev, page, page_off, length,
				       DMA_FROM_DEVICE);
		if (dma_mapping_error(dma_dev, zf->dma)) {
			if (done)
				done(done_ctx);
			kfree(zf);
			ret = -EIO;
			goto err;
		}
		INIT_LIST_HEAD(&zf->prep_link);
		list_add_tail(&zf->prep_link, frames);
		remaining -= length;
	}

	*stream_out = s;
	return 0;

err:
	u4_data_free_rx_zcopy_list(dma_dev, frames);
	if (s->finish)
		s->finish(s->finish_ctx);
	kfree(s);
	return ret;
}

static bool u4_data_try_rx_zcopy(struct u4_data_peer *p,
				 struct u4_data_frame *header_frame,
				 const struct u4_wire_hdr *hdr, u32 length)
{
	struct u4_data_rx_zcopy_frame *zf, *tmp;
	struct u4_data_rx_zcopy_stream *s = NULL;
	struct device *dma_dev = tb_ring_dma_device(p->rx_ring);
	LIST_HEAD(frames);
	void *target_qp = NULL;
	u32 min_bytes = READ_ONCE(rx_zcopy_min_bytes);
	u32 dest_qp;
	int posted = 0;
	int ret = 0;

	if (!READ_ONCE(rx_zcopy) || !u4_data_rx_zcopy_prepare)
		return false;
	if (hdr->opcode != U4_OP_RDMA_WRITE)
		return false;
	if (min_bytes && length < min_bytes)
		return false;
	if (READ_ONCE(p->rx_zcopy_stream)) {
		atomic64_inc(&p->rx_zcopy_errors);
		return false;
	}

	dest_qp = le32_to_cpu(hdr->dest_qp);
	rcu_read_lock();
	target_qp = u4_data_lookup_qp_rcu(dest_qp, p);
	if (target_qp)
		ret = u4_data_prepare_rx_zcopy(p, target_qp, hdr, length,
					       &frames, &s);
	else
		ret = -ENOENT;
	rcu_read_unlock();
	if (ret) {
		atomic64_inc(&p->rx_zcopy_fallback);
		return false;
	}

	s->header_frame = header_frame;
	WRITE_ONCE(p->rx_zcopy_stream, s);
	list_for_each_entry_safe(zf, tmp, &frames, prep_link) {
		list_del(&zf->prep_link);
		zf->frame.buffer_phy = zf->dma;
		zf->frame.callback = u4_data_rx_zcopy_complete;
		zf->frame.size = zf->length == U4_FRAME_SIZE ? 0 : zf->length;
		zf->frame.sof = U4_DATA_PDF_FRAME_START;
		zf->frame.eof = U4_DATA_PDF_FRAME_END;
		INIT_LIST_HEAD(&zf->frame.list);

		ret = usb4_rdma_nhi_raw_rx(p->rx_ring, &zf->frame);
		if (ret) {
			list_add(&zf->prep_link, &frames);
			atomic64_inc(&p->rx_zcopy_errors);
			break;
		}
		atomic_inc(&s->pending);
		s->posted_bytes += zf->length;
		posted++;
	}

	if (ret) {
		u4_data_free_rx_zcopy_list(dma_dev, &frames);
		if (!posted) {
			if (s->finish)
				s->finish(s->finish_ctx);
			WRITE_ONCE(p->rx_zcopy_stream, NULL);
			kfree(s);
			atomic64_inc(&p->rx_zcopy_fallback);
			return false;
		}
		s->fallback_remaining = length - s->posted_bytes;
		atomic64_inc(&p->rx_zcopy_fallback);
	}

	atomic64_inc(&p->rx_zcopy_streams);
	return true;
}

static void u4_data_dispatch_raw(struct u4_data_peer *p, const void *payload,
				 u32 frame_len)
{
	struct u4_wire_hdr hdr = p->raw_hdr;
	void *target_qp = NULL;
	u32 dest_qp;
	u32 length;

	if (!frame_len || frame_len > p->raw_remaining ||
	    frame_len > U4_FRAME_SIZE) {
		p->raw_pending = false;
		p->raw_remaining = 0;
		atomic64_inc(&p->rx_invalid_hdr);
		return;
	}

	p->raw_remaining -= frame_len;
	if (p->raw_remaining)
		hdr.flags &= ~U4_F_LAST;
	else
		p->raw_pending = false;
	hdr.length = cpu_to_le32(frame_len);
	hdr.remote_addr = cpu_to_le64(p->raw_base + p->raw_done);
	p->raw_done += frame_len;
	length = frame_len;

	dest_qp = le32_to_cpu(hdr.dest_qp);
	rcu_read_lock();
	target_qp = u4_data_lookup_qp_rcu(dest_qp, p);
	if (target_qp && u4_data_rx_handler)
		u4_data_rx_handler(target_qp, &hdr, payload, length);
	rcu_read_unlock();

	if (!target_qp)
		atomic64_inc(&p->rx_frames_dropped);
}

static void u4_data_rx_complete_one(struct tb_ring *ring,
				    struct ring_frame *frame, bool canceled,
				    struct list_head *repost_list)
{
	struct u4_data_frame *f = container_of(frame, typeof(*f), frame);
	struct u4_wire_hdr *hdr;
	void *payload;
	u32 length;
	void *target_qp = NULL;
	u32 dest_qp;
	struct device *dma_dev = tb_ring_dma_device(ring);
	u32 frame_len;

	if (canceled)
		return;

	dma_sync_single_for_cpu(dma_dev, f->dma, U4_FRAME_SIZE, DMA_FROM_DEVICE);
	atomic64_inc(&f->peer->rx_frames_recv);
	if (frame->flags & RING_DESC_BUFFER_OVERRUN) {
		atomic64_inc(&f->peer->rx_desc_overrun);
		goto repost;
	}
	frame_len = frame->size ?: U4_FRAME_SIZE;

	if (f->peer->raw_pending) {
		u4_data_dispatch_raw(f->peer, f->buf, frame_len);
		goto repost;
	}

	if (frame_len < U4_HDR_SIZE) {
		atomic64_inc(&f->peer->rx_invalid_hdr);
		goto repost;
	}

	hdr = (struct u4_wire_hdr *)f->buf;
	if (!u4_wire_hdr_ok(hdr)) {
		atomic64_inc(&f->peer->rx_invalid_hdr);
		goto repost;
	}

	length = le32_to_cpu(hdr->length);
	if (hdr->flags & U4_F_RAW_STREAM) {
		/* The following ring frames are raw payload DMA, not more
		 * u4_wire_hdr frames. This lets TX map registered pages
		 * directly while preserving QP/opcode dispatch metadata. */
		if (!length) {
			atomic64_inc(&f->peer->rx_invalid_hdr);
			goto repost;
		}
		if (u4_data_try_rx_zcopy(f->peer, f, hdr, length))
			return;
		f->peer->raw_hdr = *hdr;
		f->peer->raw_hdr.flags &= ~U4_F_RAW_STREAM;
		f->peer->raw_base = le64_to_cpu(hdr->remote_addr);
		f->peer->raw_done = 0;
		f->peer->raw_remaining = length;
		f->peer->raw_pending = true;
		goto repost;
	}
	if (length > U4_MAX_PAYLOAD || U4_HDR_SIZE + length > frame_len) {
		atomic64_inc(&f->peer->rx_invalid_hdr);
		goto repost;
	}

	dest_qp = le32_to_cpu(hdr->dest_qp);
	payload = (u8 *)f->buf + U4_HDR_SIZE;

	rcu_read_lock();
	target_qp = u4_data_lookup_qp_rcu(dest_qp, f->peer);
	if (target_qp && u4_data_rx_handler)
		u4_data_rx_handler(target_qp, hdr, payload, length);
	rcu_read_unlock();

	if (!target_qp)
		atomic64_inc(&f->peer->rx_frames_dropped);

repost:
	/* Re-queue this RX buffer for the next frame. */
	if (repost_list)
		list_add_tail(&f->frame.list, repost_list);
	else
		u4_data_repost_rx_frame(f->peer, f);
}

static void u4_data_rx_complete(struct tb_ring *ring, struct ring_frame *frame,
				bool canceled)
{
	u4_data_rx_complete_one(ring, frame, canceled, NULL);
}

/* ----- public: peer attach / detach ------------------------------- */

static int u4_data_prepare_lane(struct u4_data_peer *p, struct tb_service *svc,
				int lane_idx)
{
	struct tb_xdomain *xd = tb_service_parent(svc);
	u16 sof_mask = BIT(U4_DATA_PDF_FRAME_START);
	u16 eof_mask = BIT(U4_DATA_PDF_FRAME_END);
	unsigned int ring_flags = RING_FLAG_FRAME;
	int out_hop, ret, i;

	if (READ_ONCE(e2e_flow))
		ring_flags |= RING_FLAG_E2E;

	p->svc = svc;
	p->xd = xd;
	p->lane_idx = lane_idx;
	p->out_hop = -1;
	p->in_hop = -1;
	p->ring_depth = u4_data_ring_depth();
	p->frames_per_dir = u4_data_frames_per_dir(p->ring_depth);
	atomic_set(&p->refs, 1);
	mutex_init(&p->tx_lock);
	init_waitqueue_head(&p->tx_wait);
	init_waitqueue_head(&p->ref_wait);
	init_irq_work(&p->rx_poll_irq, u4_data_rx_poll_irq);
	atomic_set(&p->rx_poll_armed, 0);
	atomic_set(&p->rx_poll_busy, 0);
	INIT_LIST_HEAD(&p->list);
	u4_data_init_tx_zcopy_pool(p);

	out_hop = tb_xdomain_alloc_out_hopid(xd, -1);
	if (out_hop < 0)
		return out_hop;
	p->out_hop = out_hop;

	p->tx_ring = tb_ring_alloc_tx(xd->tb->nhi, -1, p->ring_depth,
				      ring_flags);
	if (!p->tx_ring) {
		dev_info(&svc->dev,
			 "data: lane %d unavailable: TX ring allocation failed (route=0x%llx, out_hop=%d)\n",
			 lane_idx, xd->route, p->out_hop);
		ret = -ENOSPC;
		goto err_out;
	}
	if (p->tx_ring->hop < 0) {
		dev_info(&svc->dev,
			 "data: lane %d unavailable: TX ring has no valid hop (%d, route=0x%llx, out_hop=%d); controller ring slots are exhausted or claimed by another service\n",
			 lane_idx, p->tx_ring->hop, xd->route, p->out_hop);
		ret = -ENOSPC;
		goto err_tx;
	}

	p->rx_ring = tb_ring_alloc_rx(xd->tb->nhi, -1, p->ring_depth,
				      ring_flags,
				      p->tx_ring->hop, sof_mask, eof_mask,
				      u4_data_start_rx_poll, p);
	if (!p->rx_ring) {
		dev_info(&svc->dev,
			 "data: lane %d unavailable: RX ring allocation failed (route=0x%llx, tx_ring_hop=%d, out_hop=%d)\n",
			 lane_idx, xd->route, p->tx_ring->hop, p->out_hop);
		ret = -ENOSPC;
		goto err_tx;
	}

	ret = alloc_frames(p, &p->tx_frames, p->frames_per_dir, true);
	if (ret) goto err_rx;
	ret = alloc_frames(p, &p->rx_frames, p->frames_per_dir, false);
	if (ret) goto err_tx_frames;

	tb_ring_start(p->tx_ring);
	tb_ring_start(p->rx_ring);

	p->rx_posted_initial = u4_data_initial_rx_frames(p);
	for (i = 0; i < p->rx_posted_initial; i++) {
		p->rx_frames[i].frame.callback = u4_data_rx_complete;
		ret = usb4_rdma_nhi_raw_rx(p->rx_ring, &p->rx_frames[i].frame);
		if (ret) {
			pr_warn("lane %d: post rx %d: %d\n", lane_idx, i, ret);
			goto err_started;
		}
	}

	dev_info(&svc->dev,
		 "data: lane %d prepared, ring hops tx=%d rx=%d, depth=%u, local transmit path=%d, rx_posted=%d/%u%s\n",
		 lane_idx, p->tx_ring->hop, p->rx_ring->hop, p->ring_depth,
		 p->out_hop, p->rx_posted_initial, p->frames_per_dir,
		 READ_ONCE(rx_drain_test) ? " (drain-test)" :
		 READ_ONCE(rx_zcopy) ? " (rx-zcopy)" : "");
	return 0;

err_started:
	tb_ring_stop(p->tx_ring);
	tb_ring_stop(p->rx_ring);
	free_frames(p, p->rx_frames, p->frames_per_dir, false);
err_tx_frames:
	free_frames(p, p->tx_frames, p->frames_per_dir, true);
err_rx:
	tb_ring_free(p->rx_ring);
err_tx:
	tb_ring_free(p->tx_ring);
err_out:
	u4_data_free_tx_zcopy_pool(p);
	tb_xdomain_release_out_hopid(xd, out_hop);
	return ret;
}

static int u4_data_enable_lane(struct u4_data_peer *p, int remote_tx_path)
{
	unsigned long flags;
	int ret;

	ret = tb_xdomain_alloc_in_hopid(p->xd, remote_tx_path);
	if (ret != remote_tx_path) {
		if (ret >= 0) {
			tb_xdomain_release_in_hopid(p->xd, ret);
			ret = -EINVAL;
		}
		pr_warn("lane %d: failed to allocate remote transmit path %d as RX HopID (%d)\n",
			p->lane_idx, remote_tx_path, ret);
		return ret;
	}
	p->in_hop = ret;

	ret = tb_xdomain_enable_paths(p->xd, p->out_hop, p->tx_ring->hop,
				      p->in_hop, p->rx_ring->hop);
	if (ret) {
		pr_warn("lane %d: enable_paths failed: %d\n",
			p->lane_idx, ret);
		tb_xdomain_release_in_hopid(p->xd, p->in_hop);
		p->in_hop = -1;
		return ret;
	}
	p->paths_enabled = true;

	write_lock_irqsave(&peer_lock, flags);
	list_add_tail(&p->list, &peer_list);
	atomic_inc(&peer_count);
	write_unlock_irqrestore(&peer_lock, flags);

	if (READ_ONCE(rx_busy_poll)) {
		p->rx_poll_task = kthread_run(u4_data_rx_poll_thread, p,
					      "u4rdma-rx/%d",
					      p->rx_ring->hop);
		if (IS_ERR(p->rx_poll_task)) {
			ret = PTR_ERR(p->rx_poll_task);
			p->rx_poll_task = NULL;
			dev_warn(&p->svc->dev,
				 "data: lane %d failed to start RX poll thread (%d); falling back to interrupt/irq_work RX\n",
				 p->lane_idx, ret);
		} else {
			dev_info(&p->svc->dev,
				 "data: lane %d RX busy-poll enabled on ring hop %d\n",
				 p->lane_idx, p->rx_ring->hop);
		}
	}

	dev_info(&p->svc->dev,
		 "data: lane %d attached, ring hops tx=%d rx=%d, depth=%u, local transmit path=%d remote transmit path=%d, rx_posted=%d/%u\n",
		 p->lane_idx, p->tx_ring->hop, p->rx_ring->hop,
		 p->ring_depth, p->out_hop, p->in_hop,
		 p->rx_posted_initial, p->frames_per_dir);
	usb4_rdma_ibdev_rail_event(p, true);
	return 0;
}

static void u4_data_teardown_lane(struct u4_data_peer *p)
{
	wake_up_all(&p->tx_wait);
	wait_event(p->ref_wait, atomic_read(&p->refs) == 1);

	if (p->paths_enabled)
		tb_xdomain_disable_paths(p->xd, p->out_hop, p->tx_ring->hop,
					 p->in_hop, p->rx_ring->hop);
	if (p->rx_poll_task) {
		kthread_stop(p->rx_poll_task);
		p->rx_poll_task = NULL;
	}
	tb_ring_stop(p->tx_ring);
	tb_ring_stop(p->rx_ring);
	irq_work_sync(&p->rx_poll_irq);
	if (atomic_xchg(&p->rx_poll_armed, 0))
		atomic_dec(&rx_poll_armed_count);
	u4_data_free_tx_zcopy_pool(p);
	free_frames(p, p->rx_frames, p->frames_per_dir, false);
	free_frames(p, p->tx_frames, p->frames_per_dir, true);
	tb_ring_free(p->rx_ring);
	tb_ring_free(p->tx_ring);
	if (p->in_hop >= 0)
		tb_xdomain_release_in_hopid(p->xd, p->in_hop);
	if (p->out_hop >= 0)
		tb_xdomain_release_out_hopid(p->xd, p->out_hop);
	kfree(p);
}

static void u4_data_deactivate_lane(struct u4_data_peer *p)
{
	unsigned long flags;

	write_lock_irqsave(&peer_lock, flags);
	if (!list_empty(&p->list)) {
		list_del_init(&p->list);
		atomic_dec(&peer_count);
	}
	WRITE_ONCE(p->closing, true);
	write_unlock_irqrestore(&peer_lock, flags);

	usb4_rdma_ibdev_rail_event(p, false);
	u4_data_teardown_lane(p);
}

static int u4_data_stats_show(struct seq_file *m, void *unused)
{
	struct u4_data_peer *p;
	unsigned long flags;
	int idx = 0;

	seq_printf(m, "rx_busy_poll:         %u\n", READ_ONCE(rx_busy_poll));
	seq_printf(m, "e2e_flow:             %u\n", READ_ONCE(e2e_flow));
	seq_printf(m, "rx_drain_test:        %u\n", READ_ONCE(rx_drain_test));
	seq_printf(m, "rx_drain_test_frames: %u\n",
		   READ_ONCE(rx_drain_test_frames));
	seq_printf(m, "data_ring_depth:      %u\n", u4_data_ring_depth());
	seq_printf(m, "data_frames_per_dir:  %u\n",
		   u4_data_frames_per_dir(u4_data_ring_depth()));
	seq_printf(m, "rx_zcopy:             %u\n", READ_ONCE(rx_zcopy));
	seq_printf(m, "rx_zcopy_min_bytes:   %u\n",
		   READ_ONCE(rx_zcopy_min_bytes));
	seq_printf(m, "rx_poll_opportunistic_lanes: %u\n",
		   READ_ONCE(rx_poll_opportunistic_lanes));
	seq_printf(m, "tx_zcopy_pool_frames: %u\n",
		   READ_ONCE(tx_zcopy_pool_frames));
	seq_printf(m, "tx_stream_affinity:   %u\n",
		   READ_ONCE(tx_stream_affinity));
	seq_printf(m, "tx_stream_affinity_used: %lld\n",
		   (long long)atomic64_read(&tx_stream_affinity_used));
	seq_printf(m, "tx_stream_affinity_fallback: %lld\n",
		   (long long)atomic64_read(&tx_stream_affinity_fallback));
	seq_printf(m, "rx_poll_armed_count:  %d\n",
		   atomic_read(&rx_poll_armed_count));
	seq_printf(m, "rx_poll_calls:        %lld\n",
		   (long long)atomic64_read(&rx_poll_calls));
	seq_printf(m, "rx_poll_armed_scans:  %lld\n",
		   (long long)atomic64_read(&rx_poll_armed_scans));
	seq_printf(m, "rx_poll_opportunistic_scans: %lld\n",
		   (long long)atomic64_read(&rx_poll_opportunistic_scans));
	seq_printf(m, "rx_poll_skipped:      %lld\n",
		   (long long)atomic64_read(&rx_poll_skipped));
	seq_printf(m, "active_lanes:         %d\n",
		   usb4_rdma_data_active_lane_count());
	usb4_rdma_nhi_raw_stats_show(m);

	read_lock_irqsave(&peer_lock, flags);
	list_for_each_entry(p, &peer_list, list) {
		seq_printf(m, "\nlane[%d]:\n", idx++);
		seq_printf(m, "  service:            %s\n",
			   p->svc ? dev_name(&p->svc->dev) : "(none)");
		seq_printf(m, "  lane_idx:           %d\n", p->lane_idx);
		seq_printf(m, "  closing:            %u\n", READ_ONCE(p->closing));
		seq_printf(m, "  paths_enabled:      %u\n", p->paths_enabled);
		seq_printf(m, "  tx_hop:             %d\n",
			   p->tx_ring ? p->tx_ring->hop : -1);
		seq_printf(m, "  rx_hop:             %d\n",
			   p->rx_ring ? p->rx_ring->hop : -1);
		seq_printf(m, "  out_hop:            %d\n", p->out_hop);
		seq_printf(m, "  in_hop:             %d\n", p->in_hop);
		usb4_rdma_nhi_raw_ring_show(m, "tx", p->tx_ring);
		usb4_rdma_nhi_raw_ring_show(m, "rx", p->rx_ring);
		seq_printf(m, "  rx_posted_initial:  %d\n",
			   p->rx_posted_initial);
		seq_printf(m, "  tx_inflight:        %d\n",
			   atomic_read(&p->tx_inflight));
		seq_printf(m, "  tx_zcopy_pool_free: %d/%d\n",
			   p->tx_zcopy_free_count, p->tx_zcopy_pool_count);
		seq_printf(m, "  raw_pending:        %u\n",
			   READ_ONCE(p->raw_pending));
		seq_printf(m, "  raw_remaining:      %u\n",
			   READ_ONCE(p->raw_remaining));
		seq_printf(m, "  rx_zcopy_active:    %u\n",
			   !!READ_ONCE(p->rx_zcopy_stream));
		seq_printf(m, "  tx_frames_sent:     %lld\n",
			   (long long)atomic64_read(&p->tx_frames_sent));
		seq_printf(m, "  rx_frames_recv:     %lld\n",
			   (long long)atomic64_read(&p->rx_frames_recv));
		seq_printf(m, "  rx_frames_dropped:  %lld\n",
			   (long long)atomic64_read(&p->rx_frames_dropped));
		seq_printf(m, "  rx_invalid_hdr:     %lld\n",
			   (long long)atomic64_read(&p->rx_invalid_hdr));
		seq_printf(m, "  rx_poll_armed_frames: %lld\n",
			   (long long)atomic64_read(&p->rx_poll_armed_frames));
		seq_printf(m, "  rx_poll_opportunistic_frames: %lld\n",
			   (long long)atomic64_read(&p->rx_poll_opportunistic_frames));
		seq_printf(m, "  rx_poll_irq_frames: %lld\n",
			   (long long)atomic64_read(&p->rx_poll_irq_frames));
		seq_printf(m, "  rx_poll_busy_frames: %lld\n",
			   (long long)atomic64_read(&p->rx_poll_busy_frames));
		seq_printf(m, "  rx_desc_overrun:    %lld\n",
			   (long long)atomic64_read(&p->rx_desc_overrun));
		seq_printf(m, "  rx_repost_failed:   %lld\n",
			   (long long)atomic64_read(&p->rx_repost_failed));
		seq_printf(m, "  rx_zcopy_streams:   %lld\n",
			   (long long)atomic64_read(&p->rx_zcopy_streams));
		seq_printf(m, "  rx_zcopy_frames:    %lld\n",
			   (long long)atomic64_read(&p->rx_zcopy_frames));
		seq_printf(m, "  rx_zcopy_bytes:     %lld\n",
			   (long long)atomic64_read(&p->rx_zcopy_bytes));
		seq_printf(m, "  rx_zcopy_fallback:  %lld\n",
			   (long long)atomic64_read(&p->rx_zcopy_fallback));
		seq_printf(m, "  rx_zcopy_errors:    %lld\n",
			   (long long)atomic64_read(&p->rx_zcopy_errors));
		seq_printf(m, "  tx_zcopy_pool_hits: %lld\n",
			   (long long)atomic64_read(&p->tx_zcopy_pool_hits));
		seq_printf(m, "  tx_zcopy_pool_misses: %lld\n",
			   (long long)atomic64_read(&p->tx_zcopy_pool_misses));
	}
	read_unlock_irqrestore(&peer_lock, flags);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(u4_data_stats);

int usb4_rdma_data_init(struct dentry *parent_dir)
{
	int ret;

	INIT_LIST_HEAD(&u4_login_handler.list);
	u4_login_handler.uuid = &u4_login_uuid;
	u4_login_handler.callback = u4_login_handle_packet;
	u4_login_handler.data = NULL;

	ret = tb_register_protocol_handler(&u4_login_handler);
	if (ret)
		return ret;
	u4_login_handler_registered = true;
	data_debugfs_root = debugfs_create_dir("data", parent_dir);
	if (!IS_ERR_OR_NULL(data_debugfs_root))
		debugfs_create_file("stats", 0444, data_debugfs_root, NULL,
				    &u4_data_stats_fops);
	return 0;
}

void usb4_rdma_data_exit(void)
{
	debugfs_remove_recursive(data_debugfs_root);
	data_debugfs_root = NULL;
	if (!u4_login_handler_registered)
		return;
	tb_unregister_protocol_handler(&u4_login_handler);
	u4_login_handler_registered = false;
}

static void u4_login_work(struct work_struct *work)
{
	struct u4_login_ctx *login =
		container_of(work, struct u4_login_ctx, work.work);
	struct u4_data_peer *active[U4_MAX_LANES_PER_SERVICE] = {};
	struct u4_data_peer *dead[U4_MAX_LANES_PER_SERVICE] = {};
	struct u4_data_peer *p;
	int remote_tx_path[U4_MAX_LANES_PER_SERVICE];
	int remote_count = 0;
	int dead_count = 0;
	int ret, lane, prepared, attached = 0;
	int attempt;

	if (READ_ONCE(login->closing) || login->notified_joined)
		return;

	attempt = ++login->login_attempts;
	prepared = login->lane_count;
	ret = u4_login_request(login, remote_tx_path, &remote_count);
	if (ret) {
		if (!READ_ONCE(login->closing) &&
		    attempt < U4_LOGIN_WORK_RETRIES) {
			dev_info(&login->svc->dev,
				 "data: login attempt %d failed (%d); retrying\n",
				 attempt, ret);
			queue_delayed_work(system_long_wq, &login->work,
					   msecs_to_jiffies(U4_LOGIN_RETRY_DELAY_MS));
		} else {
			dev_warn(&login->svc->dev, "data: login failed (%d)\n",
				 ret);
		}
		return;
	}
	if (!wait_for_completion_timeout(&login->remote_ready,
					 msecs_to_jiffies(U4_LOGIN_TIMEOUT_MS))) {
		if (!READ_ONCE(login->closing) &&
		    attempt < U4_LOGIN_WORK_RETRIES) {
			dev_info(&login->svc->dev,
				 "data: reciprocal login not observed on attempt %d; retrying\n",
				 attempt);
			queue_delayed_work(system_long_wq, &login->work,
					   msecs_to_jiffies(U4_LOGIN_RETRY_DELAY_MS));
		} else {
			dev_warn(&login->svc->dev,
				 "data: reciprocal login request not observed; refusing half-open data path\n");
		}
		return;
	}
	if (READ_ONCE(login->closing))
		return;

	remote_count = min(remote_count, prepared);
	for (lane = 0; lane < prepared; lane++) {
		p = login->lanes[lane];
		if (!p)
			continue;
		if (lane >= remote_count) {
			dead[dead_count++] = p;
			continue;
		}

		ret = u4_data_enable_lane(p, remote_tx_path[lane]);
		if (ret) {
			dev_warn(&login->svc->dev,
				 "data: lane %d negotiated but failed to enable (%d)\n",
				 lane, ret);
			dead[dead_count++] = p;
			continue;
		}
		active[attached++] = p;
	}

	mutex_lock(&login->lock);
	for (lane = 0; lane < prepared; lane++)
		login->lanes[lane] = NULL;
	for (lane = 0; lane < attached; lane++)
		login->lanes[lane] = active[lane];
	login->lane_count = attached;
	if (attached)
		login->notified_joined = true;
	mutex_unlock(&login->lock);

	for (lane = 0; lane < dead_count; lane++) {
		if (dead[lane])
			u4_data_deactivate_lane(dead[lane]);
	}

	if (!attached) {
		dev_warn(&login->svc->dev,
			 "data: login completed but no lanes could be enabled\n");
		return;
	}

	usb4_rdma_ibdev_peer_event(true);
}

int usb4_rdma_data_attach_peer(struct tb_service *svc)
{
	struct u4_login_ctx *login;
	struct u4_data_peer *p;
	struct tb_xdomain *xd = tb_service_parent(svc);
	int ret = 0, lane, prepared = 0;

	if (!xd)
		return -ENODEV;

	login = kzalloc(sizeof(*login), GFP_KERNEL);
	if (!login)
		return -ENOMEM;
	INIT_LIST_HEAD(&login->list);
	mutex_init(&login->lock);
	init_completion(&login->remote_ready);
	INIT_DELAYED_WORK(&login->work, u4_login_work);
	login->svc = svc;
	login->xd = xd;

	for (lane = 0; lane < U4_MAX_LANES_PER_SERVICE; lane++) {
		p = kzalloc(sizeof(*p), GFP_KERNEL);
		if (!p) {
			ret = -ENOMEM;
			break;
		}

		ret = u4_data_prepare_lane(p, svc, lane);
		if (ret) {
			kfree(p);
			if (!prepared)
				goto err_free_login;
			dev_info(&svc->dev,
				 "data: lane %d unavailable (%d); continuing with %d lane(s)\n",
				 lane, ret, prepared);
			break;
		}
		login->lanes[prepared++] = p;
		login->lane_count = prepared;
	}

	mutex_lock(&login_ctx_lock);
	list_add_tail(&login->list, &login_ctx_list);
	mutex_unlock(&login_ctx_lock);

	queue_delayed_work(system_long_wq, &login->work,
			   msecs_to_jiffies(U4_LOGIN_START_DELAY_MS));
	return 0;

err_free_login:
	for (lane = 0; lane < prepared; lane++) {
		p = login->lanes[lane];
		if (p)
			u4_data_deactivate_lane(p);
	}
	kfree(login);
	return ret;
}

static bool u4_login_remove_ctx(struct tb_service *svc)
{
	struct u4_login_ctx *ctx, *tmp;
	bool notified;
	int lane;

	mutex_lock(&login_ctx_lock);
	list_for_each_entry_safe(ctx, tmp, &login_ctx_list, list) {
		if (ctx->svc != svc)
			continue;
		list_del_init(&ctx->list);
		WRITE_ONCE(ctx->closing, true);
		goto found;
	}
	mutex_unlock(&login_ctx_lock);
	return false;

found:
	mutex_unlock(&login_ctx_lock);

	cancel_delayed_work_sync(&ctx->work);
	notified = ctx->notified_joined;

	for (lane = 0; lane < ctx->lane_count; lane++) {
		struct u4_data_peer *p = ctx->lanes[lane];

		if (!p)
			continue;
		ctx->lanes[lane] = NULL;
		u4_data_deactivate_lane(p);
	}
	kfree(ctx);
	return notified;
}

bool usb4_rdma_data_detach_peer(struct tb_service *svc)
{
	bool detached = u4_login_remove_ctx(svc);

	if (detached)
		dev_info(&svc->dev, "data: peer detached\n");
	return detached;
}

/* ----- public: TX submit (called from post_send) ------------------ */

static int u4_data_send_ack_try(struct u4_data_peer *rail, u32 src_qp,
				u32 dest_qp, u32 psn,
				__be32 status, bool allow_fallback)
{
	struct u4_data_peer *p;
	struct u4_data_frame *f;
	u32 actual_len = U4_HDR_SIZE;
	u32 desc_len;
	int ret;

	p = u4_data_peer_get_by_qp(rail, src_qp, dest_qp);
	if (!p)
		return -ENOTCONN;
	if (READ_ONCE(p->closing)) {
		u4_data_peer_put(p);
		return -ENOTCONN;
	}

	f = u4_data_claim_tx_frame(p);
	if (!f) {
		u4_data_peer_put(p);
		return -EAGAIN;
	}

	u4_wire_hdr_init((struct u4_wire_hdr *)f->buf, U4_OP_SEND_ACK,
			 dest_qp, src_qp, psn, 0, U4_F_LAST, status, 0, 0);
	f->frame.size = u4_data_tx_desc_size(actual_len);
	desc_len = u4_data_desc_len(f->frame.size);
	if (desc_len > actual_len)
		memset((u8 *)f->buf + actual_len, 0, desc_len - actual_len);
	f->frame.callback = u4_data_tx_complete;
	f->frame.sof = U4_DATA_PDF_FRAME_START;
	f->frame.eof = U4_DATA_PDF_FRAME_END;

	dma_sync_single_for_device(tb_ring_dma_device(p->tx_ring), f->dma,
				   U4_FRAME_SIZE, DMA_TO_DEVICE);
	atomic_inc(&p->tx_inflight);
	if (allow_fallback)
		ret = usb4_rdma_nhi_raw_tx(p->tx_ring, &f->frame);
	else
		ret = usb4_rdma_nhi_raw_tx_atomic(p->tx_ring, &f->frame);
	if (ret) {
		dma_sync_single_for_cpu(tb_ring_dma_device(p->tx_ring), f->dma,
					U4_FRAME_SIZE, DMA_TO_DEVICE);
		atomic_dec(&p->tx_inflight);
		atomic_set(&f->in_use, 0);
		u4_data_tx_wake(p);
		u4_data_peer_put(p);
		return ret;
	}

	u4_data_peer_put(p);
	return 0;
}

int usb4_rdma_data_send_ack_atomic(struct u4_data_peer *rail,
				   u32 src_qp, u32 dest_qp, u32 psn,
				   __be32 status)
{
	return u4_data_send_ack_try(rail, src_qp, dest_qp, psn, status, false);
}

int usb4_rdma_data_send_ack_try(struct u4_data_peer *rail,
				u32 src_qp, u32 dest_qp, u32 psn,
				__be32 status)
{
	return u4_data_send_ack_try(rail, src_qp, dest_qp, psn, status, true);
}

int usb4_rdma_data_send(struct u4_data_peer *rail, u8 opcode,
			u32 src_qp, u32 dest_qp, u32 psn,
			u8 flags, __be32 imm_data, u64 remote_addr, u32 rkey,
			usb4_rdma_data_fill_fn fill, void *fill_ctx,
			u32 length)
{
	struct u4_data_peer *p;
	struct u4_data_frame *f = NULL;
	u32 actual_len;
	u32 desc_len;
	int ret;

	if (length > U4_MAX_PAYLOAD)
		return -EMSGSIZE;
	if (length && !fill)
		return -EINVAL;
	if (u4_data_warn_process_tx_from_atomic(__func__))
		return -EWOULDBLOCK;

	p = u4_data_peer_get_by_qp(rail, src_qp, dest_qp);
	if (!p)
		return -ENOTCONN;

	mutex_lock(&p->tx_lock);
	/* Fragmented WRs must not fail halfway through just because all TX
	 * staging frames are temporarily busy. Wait for a completion to free
	 * a slot, or for detach to mark the peer closing. */
	wait_event(p->tx_wait,
		   READ_ONCE(p->closing) ||
		   (f = u4_data_claim_tx_frame(p)));
	if (READ_ONCE(p->closing)) {
		if (f) {
			atomic_set(&f->in_use, 0);
			u4_data_tx_wake(p);
		}
		mutex_unlock(&p->tx_lock);
		u4_data_peer_put(p);
		return -ENOTCONN;
	}

	u4_wire_hdr_init((struct u4_wire_hdr *)f->buf, opcode, dest_qp, src_qp,
			 psn, length, flags, imm_data, remote_addr, rkey);
	if (length && fill) {
		ret = fill((u8 *)f->buf + U4_HDR_SIZE, length, fill_ctx);
		if (ret) {
			atomic_set(&f->in_use, 0);
			u4_data_tx_wake(p);
			mutex_unlock(&p->tx_lock);
			u4_data_peer_put(p);
			return ret;
		}
	}
	actual_len = U4_HDR_SIZE + length;
	f->frame.size = u4_data_tx_desc_size(actual_len);
	desc_len = u4_data_desc_len(f->frame.size);
	if (desc_len > actual_len)
		memset((u8 *)f->buf + actual_len, 0, desc_len - actual_len);
	f->frame.callback = u4_data_tx_complete;
	f->frame.sof = U4_DATA_PDF_FRAME_START;
	f->frame.eof = U4_DATA_PDF_FRAME_END;

	if (READ_ONCE(p->closing)) {
		atomic_set(&f->in_use, 0);
		u4_data_tx_wake(p);
		mutex_unlock(&p->tx_lock);
		u4_data_peer_put(p);
		return -ENOTCONN;
	}

	dma_sync_single_for_device(tb_ring_dma_device(p->tx_ring), f->dma,
				   U4_FRAME_SIZE, DMA_TO_DEVICE);
	atomic_inc(&p->tx_inflight);
	ret = usb4_rdma_nhi_raw_tx(p->tx_ring, &f->frame);
	if (ret) {
		dma_sync_single_for_cpu(tb_ring_dma_device(p->tx_ring), f->dma,
					U4_FRAME_SIZE, DMA_TO_DEVICE);
		atomic_dec(&p->tx_inflight);
		atomic_set(&f->in_use, 0);
		u4_data_tx_wake(p);
		mutex_unlock(&p->tx_lock);
		u4_data_peer_put(p);
		return ret;
	}
	mutex_unlock(&p->tx_lock);
	u4_data_peer_put(p);
	return 0;
}

int usb4_rdma_data_send_page(struct u4_data_peer *rail, u8 opcode,
			     u32 src_qp, u32 dest_qp, u32 psn,
			     u8 flags, __be32 imm_data, u64 remote_addr,
			     u32 rkey, struct page *page, u32 page_off,
			     u32 length, usb4_rdma_data_done_fn done,
			     void *done_ctx)
{
	struct u4_data_zcopy_frame *zf = NULL;
	struct u4_data_frame *hdrf = NULL;
	struct u4_data_peer *p;
	struct device *dma_dev;
	int ret;

	if (!page || !length || length > U4_FRAME_SIZE ||
	    page_off > PAGE_SIZE || length > PAGE_SIZE - page_off)
		return -EINVAL;
	if (u4_data_warn_process_tx_from_atomic(__func__))
		return -EWOULDBLOCK;

	if (rail) {
		if (!usb4_rdma_data_rail_get(rail))
			return -ENOTCONN;
		p = rail;
	} else {
		p = u4_data_peer_get();
	}
	if (!p)
		return -ENOTCONN;
	dma_dev = tb_ring_dma_device(p->tx_ring);

	zf = u4_data_alloc_tx_zcopy_frame(p, GFP_KERNEL);
	if (!zf) {
		u4_data_peer_put(p);
		return -ENOMEM;
	}
	zf->length = length;
	zf->unmap_dma = true;
	zf->done = done;
	zf->done_ctx = done_ctx;
	zf->dma = dma_map_page(dma_dev, page, page_off, length,
			       DMA_TO_DEVICE);
	if (dma_mapping_error(dma_dev, zf->dma)) {
		u4_data_free_tx_zcopy_frame(zf);
		u4_data_peer_put(p);
		return -EIO;
	}

	/* Single-page helper: one copied metadata frame followed by one raw
	 * DMA payload frame. Multi-frame streams use send_page_stream(). */
	mutex_lock(&p->tx_lock);
	wait_event(p->tx_wait,
		   READ_ONCE(p->closing) ||
		   (u4_data_tx_room(p, 2) &&
		    (hdrf = u4_data_claim_tx_frame(p))));
	if (READ_ONCE(p->closing)) {
		ret = -ENOTCONN;
		goto err_unlock;
	}

	u4_wire_hdr_init((struct u4_wire_hdr *)hdrf->buf, opcode, dest_qp,
			 src_qp, psn, length, flags | U4_F_RAW_STREAM,
			 imm_data, remote_addr, rkey);
	hdrf->frame.size = u4_data_tx_desc_size(U4_HDR_SIZE);
	memset((u8 *)hdrf->buf + U4_HDR_SIZE, 0,
	       u4_data_desc_len(hdrf->frame.size) - U4_HDR_SIZE);
	hdrf->frame.callback = u4_data_tx_complete;
	hdrf->frame.sof = U4_DATA_PDF_FRAME_START;
	hdrf->frame.eof = U4_DATA_PDF_FRAME_END;

	dma_sync_single_for_device(dma_dev, hdrf->dma, U4_FRAME_SIZE,
				   DMA_TO_DEVICE);
	atomic_inc(&p->tx_inflight);
	ret = usb4_rdma_nhi_raw_tx(p->tx_ring, &hdrf->frame);
	if (ret) {
		dma_sync_single_for_cpu(dma_dev, hdrf->dma, U4_FRAME_SIZE,
					DMA_TO_DEVICE);
		atomic_dec(&p->tx_inflight);
		atomic_set(&hdrf->in_use, 0);
		u4_data_tx_wake(p);
		goto err_unlock;
	}
	hdrf = NULL;

	zf->frame.buffer_phy = zf->dma;
	zf->frame.callback = u4_data_zcopy_tx_complete;
	zf->frame.size = length == U4_FRAME_SIZE ? 0 : length;
	zf->frame.sof = U4_DATA_PDF_FRAME_START;
	zf->frame.eof = U4_DATA_PDF_FRAME_END;
	INIT_LIST_HEAD(&zf->frame.list);

	atomic_inc(&p->tx_inflight);
	ret = usb4_rdma_nhi_raw_tx(p->tx_ring, &zf->frame);
	if (ret) {
		atomic_dec(&p->tx_inflight);
		u4_data_tx_wake(p);
		goto err_unlock;
	}

	mutex_unlock(&p->tx_lock);
	u4_data_peer_put(p);
	return 0;

err_unlock:
	if (hdrf) {
		atomic_set(&hdrf->in_use, 0);
		u4_data_tx_wake(p);
	}
	mutex_unlock(&p->tx_lock);
	dma_unmap_page(dma_dev, zf->dma, zf->length, DMA_TO_DEVICE);
	u4_data_free_tx_zcopy_frame(zf);
	u4_data_peer_put(p);
	return ret;
}

static void u4_data_free_zcopy_list(struct list_head *frames)
{
	struct u4_data_zcopy_frame *zf, *tmp;

	list_for_each_entry_safe(zf, tmp, frames, prep_link) {
		struct device *dma_dev = tb_ring_dma_device(zf->peer->tx_ring);

		list_del(&zf->prep_link);
		if (zf->unmap_dma)
			dma_unmap_page(dma_dev, zf->dma, zf->length,
				       DMA_TO_DEVICE);
		if (zf->done)
			zf->done(zf->done_ctx);
		u4_data_free_tx_zcopy_frame(zf);
	}
}

static int u4_data_submit_zcopy_stream(struct u4_data_peer *p,
				       u8 opcode, u32 src_qp, u32 dest_qp,
				       u32 psn, u8 flags, __be32 imm_data,
				       u64 remote_addr, u32 rkey,
				       u32 stream_off, u32 stream_length,
				       struct list_head *frames,
				       bool final_stream)
{
	struct u4_data_zcopy_frame *zf, *tmp;
	struct u4_data_frame *hdrf = NULL;
	struct device *dma_dev = tb_ring_dma_device(p->tx_ring);
	struct ring_frame *frame, *frame_tmp;
	LIST_HEAD(submit);
	u8 hdr_flags = (flags | U4_F_RAW_STREAM);
	bool hdrf_dma_synced = false;
	int unsubmitted;
	int needed;
	long wait;
	int ret;

	if (!stream_length || remote_addr > U64_MAX - stream_off)
		return -EINVAL;
	if (!final_stream)
		hdr_flags &= ~U4_F_LAST;
	needed = 1 + u4_data_zcopy_frame_count(frames);
	if (needed > p->frames_per_dir)
		return -E2BIG;

	mutex_lock(&p->tx_lock);
	wait = wait_event_killable_timeout(
		p->tx_wait,
		READ_ONCE(p->closing) ||
		(u4_data_tx_room(p, needed) &&
		 (hdrf = u4_data_claim_tx_frame(p))),
		msecs_to_jiffies(U4_TX_WAIT_TIMEOUT_MS));
	if (wait < 0) {
		ret = wait;
		goto out_unlock;
	}
	if (!wait) {
		ret = -ETIMEDOUT;
		goto out_unlock;
	}
	if (READ_ONCE(p->closing)) {
		ret = -ENOTCONN;
		goto out_unlock;
	}

	u4_wire_hdr_init((struct u4_wire_hdr *)hdrf->buf, opcode, dest_qp,
			 src_qp, psn, stream_length, hdr_flags, imm_data,
			 remote_addr + stream_off, rkey);
	hdrf->frame.size = u4_data_tx_desc_size(U4_HDR_SIZE);
	memset((u8 *)hdrf->buf + U4_HDR_SIZE, 0,
	       u4_data_desc_len(hdrf->frame.size) - U4_HDR_SIZE);
	hdrf->frame.callback = u4_data_tx_complete;
	hdrf->frame.sof = U4_DATA_PDF_FRAME_START;
	hdrf->frame.eof = U4_DATA_PDF_FRAME_END;

	dma_sync_single_for_device(dma_dev, hdrf->dma, U4_FRAME_SIZE,
				   DMA_TO_DEVICE);
	hdrf_dma_synced = true;
	INIT_LIST_HEAD(&hdrf->frame.list);
	list_add_tail(&hdrf->frame.list, &submit);

	list_for_each_entry_safe(zf, tmp, frames, prep_link) {
		list_del(&zf->prep_link);
		zf->frame.buffer_phy = zf->dma;
		zf->frame.callback = u4_data_zcopy_tx_complete;
		zf->frame.size = zf->length == U4_FRAME_SIZE ? 0 : zf->length;
		zf->frame.sof = U4_DATA_PDF_FRAME_START;
		zf->frame.eof = U4_DATA_PDF_FRAME_END;
		INIT_LIST_HEAD(&zf->frame.list);
		list_add_tail(&zf->frame.list, &submit);
	}

	if (READ_ONCE(p->closing)) {
		list_for_each_entry_safe(frame, frame_tmp, &submit, list) {
			list_del_init(&frame->list);
			if (frame != &hdrf->frame) {
				zf = container_of(frame,
						  struct u4_data_zcopy_frame,
						  frame);
				list_add(&zf->prep_link, frames);
			}
		}
		ret = -ENOTCONN;
		goto out_unlock;
	}

	atomic_add(needed, &p->tx_inflight);
	ret = usb4_rdma_nhi_raw_tx_batch(p->tx_ring, &submit);
	if (ret) {
		unsubmitted = 0;
		list_for_each_entry_safe(frame, frame_tmp, &submit, list) {
			list_del_init(&frame->list);
			atomic_dec(&p->tx_inflight);
			unsubmitted++;
			if (frame == &hdrf->frame) {
				dma_sync_single_for_cpu(dma_dev, hdrf->dma,
							U4_FRAME_SIZE,
							DMA_TO_DEVICE);
				atomic_set(&hdrf->in_use, 0);
			} else {
				zf = container_of(frame,
						  struct u4_data_zcopy_frame,
						  frame);
				list_add(&zf->prep_link, frames);
			}
		}
		if (unsubmitted)
			u4_data_tx_wake(p);
		hdrf = NULL;
		goto out_unlock;
	}
	hdrf = NULL;
	ret = 0;

out_unlock:
	if (hdrf) {
		if (hdrf_dma_synced)
			dma_sync_single_for_cpu(dma_dev, hdrf->dma,
						U4_FRAME_SIZE, DMA_TO_DEVICE);
		atomic_set(&hdrf->in_use, 0);
		u4_data_tx_wake(p);
	}
	mutex_unlock(&p->tx_lock);
	return ret;
}

int usb4_rdma_data_send_page_stream(struct u4_data_peer *rail, u8 opcode,
				    u32 src_qp, u32 dest_qp,
				    u32 psn, u8 flags, __be32 imm_data,
				    u64 remote_addr, u32 rkey,
				    u32 total_length,
				    usb4_rdma_data_next_page_fn next,
				    void *next_ctx)
{
	struct list_head stripes[U4_MAX_ACTIVE_LANES];
	struct u4_data_peer *peers[U4_MAX_ACTIVE_LANES];
	u32 stripe_off[U4_MAX_ACTIVE_LANES];
	bool stripe_seen[U4_MAX_ACTIVE_LANES];
	u32 prepared = 0;
	int last_stripe = -1;
	int affine_lane = -1;
	int npeers;
	int ret = 0;
	int i;

	if (!total_length || !next)
		return -EINVAL;
	if (u4_data_warn_process_tx_from_atomic(__func__))
		return -EWOULDBLOCK;

	if (rail) {
		if (!usb4_rdma_data_rail_get(rail))
			return -ENOTCONN;
		peers[0] = rail;
		npeers = 1;
	} else {
		npeers = u4_data_peers_get(peers, U4_MAX_ACTIVE_LANES);
	}
	if (!npeers)
		return -ENOTCONN;
	if (READ_ONCE(tx_stream_affinity)) {
		if (total_length <= u4_tx_stream_affinity_max(peers[0])) {
			affine_lane = hash_32(src_qp ^ dest_qp ^ psn, 32) %
				      npeers;
			atomic64_inc(&tx_stream_affinity_used);
		} else {
			atomic64_inc(&tx_stream_affinity_fallback);
		}
	}

	for (i = 0; i < U4_MAX_ACTIVE_LANES; i++) {
		INIT_LIST_HEAD(&stripes[i]);
		stripe_off[i] = 0;
		stripe_seen[i] = false;
	}

	while (prepared < total_length) {
		struct u4_data_zcopy_frame *zf;
		struct device *dma_dev;
		struct page *page;
		u32 page_idx = 0;
		u32 page_off, length;
		dma_addr_t dma_addr = 0;
		bool dma_mapped = false;
		usb4_rdma_data_dma_resolve_fn resolve = NULL;
		void *resolve_ctx = NULL;
		usb4_rdma_data_done_fn done = NULL;
		void *done_ctx = NULL;
		u64 stripe;
		int lane;

		ret = next(next_ctx, &page, &page_idx, &page_off, &length,
			   &resolve, &resolve_ctx,
			   &done, &done_ctx);
		if (ret)
			goto err_prepared;

		if (affine_lane >= 0) {
			lane = affine_lane;
		} else {
			stripe = div_u64((u64)prepared * npeers, total_length);
			if (stripe >= npeers)
				stripe = npeers - 1;
			lane = stripe;
		}
		if (!length || length > U4_FRAME_SIZE ||
		    length > total_length - prepared ||
		    (!dma_mapped &&
		     (!page || page_off > PAGE_SIZE ||
		      length > PAGE_SIZE - page_off))) {
			if (done)
				done(done_ctx);
			ret = -EINVAL;
			goto err_prepared;
		}

		zf = u4_data_alloc_tx_zcopy_frame(peers[lane], GFP_KERNEL);
		if (!zf) {
			if (done)
				done(done_ctx);
			ret = -ENOMEM;
			goto err_prepared;
		}
		zf->length = length;
		zf->done = done;
		zf->done_ctx = done_ctx;
		dma_dev = tb_ring_dma_device(zf->peer->tx_ring);
		if (resolve) {
			ret = resolve(resolve_ctx, dma_dev, page_idx, page_off,
				      length, DMA_TO_DEVICE, &dma_addr);
			if (!ret)
				dma_mapped = true;
			else if (ret != -EAGAIN) {
				if (done)
					done(done_ctx);
				u4_data_free_tx_zcopy_frame(zf);
				goto err_prepared;
			}
		}
		zf->unmap_dma = !dma_mapped;
		if (dma_mapped) {
			zf->dma = dma_addr;
			dma_sync_single_for_device(dma_dev, zf->dma, length,
						   DMA_TO_DEVICE);
		} else {
			zf->dma = dma_map_page(dma_dev, page, page_off, length,
					       DMA_TO_DEVICE);
			if (dma_mapping_error(dma_dev, zf->dma)) {
				if (done)
					done(done_ctx);
				u4_data_free_tx_zcopy_frame(zf);
				ret = -EIO;
				goto err_prepared;
			}
		}
		INIT_LIST_HEAD(&zf->prep_link);
		if (!stripe_seen[lane]) {
			stripe_seen[lane] = true;
			stripe_off[lane] = prepared;
		}
		list_add_tail(&zf->prep_link, &stripes[lane]);
		prepared += length;
	}

	for (i = 0; i < npeers; i++) {
		if (stripe_seen[i])
			last_stripe = i;
	}

	for (i = 0; i < npeers; i++) {
		u32 chunk_off;

		if (!stripe_seen[i])
			continue;
		chunk_off = stripe_off[i];
		while (!list_empty(&stripes[i])) {
			LIST_HEAD(chunk);
			u32 chunk_len;
			bool final_chunk;

			chunk_len = u4_data_move_zcopy_chunk(
				&stripes[i], &chunk,
				peers[i]->frames_per_dir - 1);
			final_chunk = i == last_stripe &&
				      list_empty(&stripes[i]);
			ret = u4_data_submit_zcopy_stream(
				peers[i], opcode, src_qp, dest_qp, psn,
				flags, imm_data, remote_addr, rkey,
				chunk_off, chunk_len, &chunk, final_chunk);
			if (ret) {
				u4_data_free_zcopy_list(&chunk);
				goto err_prepared;
			}
			chunk_off += chunk_len;
		}
	}
	goto out_put;

err_prepared:
	for (i = 0; i < npeers; i++)
		u4_data_free_zcopy_list(&stripes[i]);
out_put:
	for (i = 0; i < npeers; i++)
		u4_data_peer_put(peers[i]);
	return ret;
}

/* ----- public: QP table registration ------------------------------ */

int usb4_rdma_data_register_qp(u32 qp_num, void *qp,
			       struct u4_data_peer *rail)
{
	struct u4_data_qp_entry *qe, *cur;
	unsigned long flags;
	int ret = 0;

	qe = kzalloc(sizeof(*qe), GFP_KERNEL);
	if (!qe)
		return -ENOMEM;
	qe->qp_num = qp_num;
	qe->rail = rail;
	qe->qp = qp;

	spin_lock_irqsave(&u4_data_qp_lock, flags);
	hash_for_each_possible(u4_data_qp_table, cur, node, qp_num) {
		if (cur->qp_num == qp_num && cur->rail == rail) {
			ret = -EEXIST;
			goto out_unlock;
		}
	}
	hash_add_rcu(u4_data_qp_table, &qe->node, qp_num);
out_unlock:
	spin_unlock_irqrestore(&u4_data_qp_lock, flags);
	if (ret)
		kfree(qe);
	return ret;
}

void usb4_rdma_data_unregister_qp(u32 qp_num, struct u4_data_peer *rail)
{
	struct u4_data_qp_entry *qe;
	struct u4_data_qp_entry *dead = NULL;
	struct hlist_node *tmp;
	struct hlist_head *head;
	unsigned long flags;

	head = &u4_data_qp_table[hash_min(qp_num, U4_DATA_QP_HASH_BITS)];
	spin_lock_irqsave(&u4_data_qp_lock, flags);
	hlist_for_each_entry_safe(qe, tmp, head, node) {
		if (qe->qp_num == qp_num && qe->rail == rail) {
			hash_del_rcu(&qe->node);
			dead = qe;
			break;
		}
	}
	spin_unlock_irqrestore(&u4_data_qp_lock, flags);

	if (dead) {
		synchronize_rcu();
		kfree(dead);
	}
}

void usb4_rdma_data_set_rx_handler(void (*h)(void *qp,
					     const struct u4_wire_hdr *hdr,
					     const void *payload, u32 length))
{
	u4_data_rx_handler = h;
}

void usb4_rdma_data_set_rx_zcopy_prepare(
	usb4_rdma_data_rx_zcopy_prepare_fn prepare)
{
	u4_data_rx_zcopy_prepare = prepare;
}

bool usb4_rdma_data_peer_attached(void)
{
	return usb4_rdma_data_active_lane_count() > 0;
}

int usb4_rdma_data_poll_rx(void)
{
	struct u4_data_peer *peers[U4_MAX_ACTIVE_LANES];
	uint opportunistic;
	int armed;
	int n, i, done = 0;

	atomic64_inc(&rx_poll_calls);

	armed = atomic_read(&rx_poll_armed_count);
	if (armed > 0) {
		/* An NHI interrupt has already masked at least one RX ring.
		 * Drain all armed lanes promptly; this keeps interrupt-driven
		 * CQ delivery low-latency while avoiding all-lane scans when
		 * the controller has not signalled new work. */
		atomic64_inc(&rx_poll_armed_scans);
		n = u4_data_peers_get(peers, U4_MAX_ACTIVE_LANES);
		for (i = 0; i < n; i++) {
			if (atomic_read(&peers[i]->rx_poll_armed)) {
				int lane_done = u4_data_poll_rx_peer(peers[i]);

				if (lane_done)
					atomic64_add(lane_done,
						     &peers[i]->rx_poll_armed_frames);
				done += lane_done;
			}
			u4_data_peer_put(peers[i]);
		}
		return done;
	}

	opportunistic = clamp_t(uint, READ_ONCE(rx_poll_opportunistic_lanes),
				0, U4_MAX_ACTIVE_LANES);
	if (!opportunistic || !atomic_read(&peer_count)) {
		atomic64_inc(&rx_poll_skipped);
		return 0;
	}

	/* Busy-poll users call ib_poll_cq() far more often than the NHI
	 * produces frames. Scan a small round-robin subset when no interrupt
	 * is armed: enough to preserve the raw-NHI latency win, without four
	 * empty ring checks per userspace poll. */
	atomic64_inc(&rx_poll_opportunistic_scans);
	for (i = 0; i < opportunistic; i++) {
		struct u4_data_peer *p = u4_data_peer_get();
		int lane_done;

		if (!p)
			break;
		lane_done = u4_data_poll_rx_peer(p);
		if (lane_done)
			atomic64_add(lane_done,
				     &p->rx_poll_opportunistic_frames);
		done += lane_done;
		u4_data_peer_put(p);
	}
	return done;
}

int usb4_rdma_data_active_lane_count(void)
{
	return atomic_read(&peer_count);
}

struct device *usb4_rdma_data_dma_dev_get(void)
{
	struct u4_data_peer *p;
	struct device *dev;

	p = u4_data_peer_get();
	if (!p)
		return NULL;
	dev = tb_ring_dma_device(p->tx_ring);
	get_device(dev);
	u4_data_peer_put(p);
	return dev;
}

void usb4_rdma_data_dma_dev_put(struct device *dev)
{
	if (dev)
		put_device(dev);
}
