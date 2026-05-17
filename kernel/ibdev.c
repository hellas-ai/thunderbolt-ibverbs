// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/highmem.h>
#include <linux/idr.h>
#include <linux/module.h>
#include <linux/mmzone.h>
#include <linux/netdevice.h>
#include <linux/overflow.h>
#include <linux/refcount.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <rdma/ib_mad.h>
#include <rdma/ib_umem.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/ib_verbs.h>

#include "../proto/native_data.h"
#include "tbv.h"

#define TBV_IBDEV_ABI_VERSION 1
#define TBV_IBDEV_PORTS 1
#define TBV_IBDEV_MAX_QP 256
#define TBV_IBDEV_MAX_QP_WR 1024
#define TBV_IBDEV_MAX_CQ 256
#define TBV_IBDEV_MAX_CQE 4096
#define TBV_IBDEV_MAX_SGE 4
#define TBV_IBDEV_MAX_READ_CTX 128
#define TBV_IBDEV_QPN_MIN 0x900
#define TBV_IBDEV_QPN_MAX 0x00ffffff
#define TBV_APPLE_PRIMARY_QPN TBV_IBDEV_QPN_MIN
#define TBV_IBDEV_PAGE_SIZE_CAP (SZ_4K | SZ_2M | SZ_1G)
#define TBV_PSN_MASK 0x00ffffffu
#define TBV_RX_REORDER_MAX_MESSAGES 64
#define TBV_RX_REORDER_MAX_BYTES (64u * 1024u * 1024u)
#define TBV_RX_REORDER_MAX_FRAGS TBV_NATIVE_DATA_MAX_FRAGS
#define TBV_IBDEV_GID_TBL_LEN 8
#define TBV_APPLE_PENDING_RX_DEFAULT_SLOTS 4096
#define TBV_APPLE_PENDING_RX_MAX_SLOTS 16384
#define TBV_APPLE_PENDING_RX_TOTAL_BYTES_DEFAULT (64u * 1024u * 1024u)
#define TBV_APPLE_SLOT_WIRE_SIZE 256
#define TBV_APPLE_FRAME_SLOT_USER_SIZE 252
#define TBV_APPLE_FRAME_SPLIT_USER_SIZE 12
#define TBV_APPLE_TAIL_USER_SIZE 240

static char *roce_netdev;
module_param(roce_netdev, charp, 0444);
MODULE_PARM_DESC(roce_netdev,
		 "Netdev used for RoCE GID metadata, for example br0.lan");

const char *tbv_ibdev_roce_netdev_name(void)
{
	return roce_netdev;
}

static uint zcopy_min_bytes;
module_param(zcopy_min_bytes, uint, 0644);
MODULE_PARM_DESC(zcopy_min_bytes,
		 "Minimum native SEND/WRITE/READ-response bytes before zero-copy page streaming is used; 0 disables zero-copy");

static uint apple_tx_max_inflight_wr = 1;
module_param(apple_tx_max_inflight_wr, uint, 0644);
MODULE_PARM_DESC(apple_tx_max_inflight_wr,
		 "Maximum Apple-compatible UC SEND work requests in flight per QP; 0 disables the software window");

static uint apple_tx_max_inflight_frames;
module_param(apple_tx_max_inflight_frames, uint, 0644);
MODULE_PARM_DESC(apple_tx_max_inflight_frames,
		 "Maximum Apple-compatible 4 KiB FA57 frames in flight per QP; 0 disables the frame window");

static uint apple_tx_pace_us;
module_param(apple_tx_pace_us, uint, 0644);
MODULE_PARM_DESC(apple_tx_pace_us,
		 "Microseconds to sleep between Apple-compatible FA57 TX frame groups; 0 disables pacing");

static uint apple_tx_pace_interval_frames = 1;
module_param(apple_tx_pace_interval_frames, uint, 0644);
MODULE_PARM_DESC(apple_tx_pace_interval_frames,
		 "Apply Apple-compatible TX pacing every N non-terminal FA57 frames; 0 is treated as 1");

static uint apple_tx_pace_after_frames;
module_param(apple_tx_pace_after_frames, uint, 0644);
MODULE_PARM_DESC(apple_tx_pace_after_frames,
		 "Start Apple-compatible TX pacing after this many queued FA57 frames in a QP burst; 0 starts immediately");

static uint apple_rx_pending_bytes = TBV_APPLE_MAX_MSG_SIZE;
module_param(apple_rx_pending_bytes, uint, 0644);
MODULE_PARM_DESC(apple_rx_pending_bytes,
		 "Maximum bytes buffered per early Apple UC receive when no receive WQE is posted");

static uint apple_rx_pending_slots = TBV_APPLE_PENDING_RX_DEFAULT_SLOTS;
module_param(apple_rx_pending_slots, uint, 0644);
MODULE_PARM_DESC(apple_rx_pending_slots,
		 "Maximum number of early Apple UC receives buffered per QP");

static uint apple_rx_pending_total_bytes =
	TBV_APPLE_PENDING_RX_TOTAL_BYTES_DEFAULT;
module_param(apple_rx_pending_total_bytes, uint, 0644);
MODULE_PARM_DESC(apple_rx_pending_total_bytes,
		 "Maximum aggregate bytes buffered for early Apple UC receives per QP");

struct tbv_ucontext {
	struct ib_ucontext base;
	struct tbv_state *owner;
};

struct tbv_pd {
	struct ib_pd base;
	struct tbv_state *owner;
};

struct tbv_cq {
	struct ib_cq base;
	struct tbv_state *owner;
	spinlock_t lock;
	struct ib_wc *entries;
	u32 cqe;
	u32 head;
	u32 tail;
	u32 count;
	bool notify_armed;
};

struct tbv_recv_wqe {
	u64 wr_id;
	u64 addr;
	u32 length;
	u32 lkey;
};

struct tbv_send_segment {
	struct tbv_mr *mr;
	u64 addr;
	u32 length;
};

struct tbv_read_segment {
	struct tbv_mr *mr;
	u64 addr;
	u32 length;
};

struct tbv_rx_message {
	struct tbv_recv_wqe wqe;
	u32 src_qp;
	u32 psn;
	u32 total_len;
	u32 imm_data;
	u32 received;
	u32 delivered;
	int status;
	bool active;
	bool with_imm;
	bool solicited;
};

struct tbv_rx_reorder_msg {
	struct list_head node;
	u32 src_qp;
	u32 psn;
	u32 total_len;
	u32 imm_data;
	u32 received;
	u16 frag_count;
	u16 frags_received;
	DECLARE_BITMAP(frag_seen, TBV_RX_REORDER_MAX_FRAGS);
	bool complete;
	bool with_imm;
	bool solicited;
	void *buf;
};

struct tbv_apple_pending_rx {
	void *buf;
	u32 capacity;
	u32 delivered;
	int status;
	bool active;
	bool ready;
};

struct tbv_qp {
	struct ib_qp base;
	struct tbv_state *owner;
	spinlock_t lock;
	struct mutex rx_lock;
	wait_queue_head_t credit_wait;
	wait_queue_head_t apple_tx_wait;
	refcount_t refs;
	struct completion refs_zero;
	struct list_head pending_sends;
	struct list_head pending_reads;
	struct ib_qp_init_attr init_attr;
	struct ib_qp_attr attr;
	struct tbv_recv_wqe *recvq;
	enum ib_qp_state state;
	enum ib_qp_type type;
	u32 recvq_size;
	u32 recv_head;
	u32 recv_tail;
	u32 recv_count;
	u32 recv_credits_advertised;
	u32 remote_recv_credits;
	atomic_t apple_tx_inflight;
	atomic_t apple_tx_inflight_frames;
	atomic_t apple_tx_pace_queued_frames;
	u32 send_psn;
	u32 rx_expected_psn;
	struct tbv_rx_message rx_msg;
	struct list_head rx_reorder;
	u32 rx_reorder_count;
	u32 rx_reorder_bytes;
	struct tbv_apple_pending_rx *apple_pending;
	u32 apple_pending_slot_count;
	u32 apple_pending_head;
	u32 apple_pending_tail;
	u32 apple_pending_ready_count;
	u32 apple_pending_bytes;
	int apple_pending_active;
	bool qpn_allocated;
	bool closing;
};

struct tbv_mr {
	struct ib_mr base;
	struct tbv_state *owner;
	struct ib_umem *umem;
	refcount_t refs;
	struct completion refs_zero;
	u64 start;
	u64 length;
	u64 virt_addr;
	int access;
	bool closing;
};

struct tbv_ibdev {
	struct ib_device base;
	struct tbv_state *state;
};

struct tbv_send_ctx {
	struct list_head node;
	struct tbv_qp *tqp;
	refcount_t refs;
	spinlock_t lock;
	u64 wr_id;
	u32 psn;
	enum ib_wc_opcode wc_opcode;
	atomic_t apple_pending;
	bool signaled;
	bool completed;
	bool apple_window_acquired;
	bool apple_window_wr_acquired;
	u32 apple_window_frames;
};

struct tbv_read_ctx {
	struct list_head node;
	struct tbv_qp *tqp;
	refcount_t refs;
	spinlock_t lock;
	u64 wr_id;
	u32 psn;
	u32 total_len;
	u32 received;
	int nsegs;
	bool signaled;
	bool completed;
	struct tbv_read_segment segs[TBV_IBDEV_MAX_SGE];
};

struct tbv_read_req_work {
	struct work_struct work;
	struct tbv_state *state;
	struct tbv_native_data_header hdr;
};

struct tbv_read_resp_stream {
	struct tbv_mr *mr;
	refcount_t refs;
	u64 iova;
	u32 offset;
	u32 total_len;
};

struct tbv_send_page_stream {
	struct tbv_send_segment segs[TBV_IBDEV_MAX_SGE];
	struct tbv_send_ctx *send;
	refcount_t refs;
	u32 offset;
	u32 total_len;
	int nsegs;
};

struct tbv_apple_send_fill {
	const struct tbv_send_segment *segs;
	struct tbv_qp *tqp;
	u32 offset;
	int nsegs;
};

static DEFINE_IDA(tbv_qpn_ida);
static atomic_t tbv_mr_key = ATOMIC_INIT(1);

static int tbv_cq_push(struct tbv_cq *tcq, const struct ib_wc *wc);
static void tbv_send_ctx_put(struct tbv_send_ctx *send);
static bool tbv_send_complete(struct tbv_send_ctx *send, int status);
static void tbv_send_tx_done(void *ctx, int status);
static void tbv_apple_send_tx_done(void *ctx, int status);
static int tbv_qp_acquire_apple_tx_window(struct tbv_qp *tqp, u32 frames,
					  bool *wr_acquired,
					  u32 *frames_acquired);
static void tbv_read_ctx_put(struct tbv_read_ctx *read);
static bool tbv_read_complete(struct tbv_read_ctx *read, int status);
static void tbv_read_tx_done(void *ctx, int status);
static int tbv_umem_page_from_addr(struct tbv_mr *mr, u64 addr, u32 max_len,
				   struct page **page_out,
				   u32 *page_off_out, u32 *len_out);
static int tbv_rx_copy_to_wqe(struct tbv_state *state,
			      const struct tbv_recv_wqe *wqe, u32 offset,
			      const void *payload, u32 len, u32 *delivered);
static void tbv_qp_flush_reorder(struct tbv_qp *tqp);
static void tbv_rx_drain_reorder_locked(struct tbv_state *state,
					struct tbv_qp *tqp);
static void tbv_qp_flush_apple_pending(struct tbv_qp *tqp);
static void tbv_apple_rx_drain_pending_locked(struct tbv_state *state,
					      struct tbv_qp *tqp);
static void tbv_qp_advertise_recv_credits(struct tbv_qp *tqp);
static int tbv_qp_consume_remote_recv_credit(struct tbv_qp *tqp);
static void tbv_qp_return_remote_recv_credit(struct tbv_qp *tqp);

static u32 tbv_psn_next(u32 psn)
{
	return (psn + 1) & TBV_PSN_MASK;
}

static s32 tbv_psn_delta(u32 a, u32 b)
{
	u32 delta = (a - b) & TBV_PSN_MASK;

	if (delta & 0x00800000u)
		return (s32)(delta | 0xff000000u);
	return (s32)delta;
}

static bool tbv_state_apple_only(const struct tbv_state *state)
{
	return state && state->cfg.apple_enabled && !state->cfg.native_enabled;
}

static bool tbv_qp_uses_apple_transport(const struct tbv_qp *tqp)
{
	return tqp && tbv_state_apple_only(tqp->owner);
}

static u32 tbv_qp_max_msg_size(const struct tbv_qp *tqp)
{
	return tbv_qp_uses_apple_transport(tqp) ? TBV_APPLE_MAX_MSG_SIZE :
						  TBV_NATIVE_DATA_MAX_MSG_SIZE;
}

static u32 tbv_apple_qpn_from_path(const struct tbv_path *path)
{
	if (!path || path->cfg.receive_path < 0)
		return TBV_APPLE_PRIMARY_QPN;

	return (u32)path->cfg.receive_path << TBV_APPLE_QPN_SHIFT;
}

static int tbv_alloc_qpn(const struct tbv_state *state)
{
	if (tbv_state_apple_only(state))
		return ida_alloc_range(&tbv_qpn_ida, TBV_APPLE_PRIMARY_QPN,
				       TBV_APPLE_PRIMARY_QPN, GFP_KERNEL);

	return ida_alloc_range(&tbv_qpn_ida, TBV_IBDEV_QPN_MIN,
			       TBV_IBDEV_QPN_MAX, GFP_KERNEL);
}

static struct tbv_state *tbv_ibdev_state(struct ib_device *ibdev)
{
	struct tbv_ibdev *dev = container_of(ibdev, struct tbv_ibdev, base);

	return dev->state;
}

static struct tbv_mr *tbv_mr_get(struct tbv_state *state, u32 key)
{
	struct tbv_mr *mr;
	XA_STATE(xas, &state->verbs_mrs_xa, key);
	unsigned long flags;

	xas_lock_irqsave(&xas, flags);
	mr = xas_load(&xas);
	if (mr && !mr->closing && refcount_inc_not_zero(&mr->refs))
		goto out;
	mr = NULL;
out:
	xas_unlock_irqrestore(&xas, flags);
	return mr;
}

static void tbv_mr_put(struct tbv_mr *mr)
{
	if (mr && refcount_dec_and_test(&mr->refs))
		complete(&mr->refs_zero);
}

static struct tbv_qp *tbv_qp_get_by_num(struct tbv_state *state, u32 qpn)
{
	struct tbv_qp *tqp;
	XA_STATE(xas, &state->verbs_qps_xa, qpn);
	unsigned long flags;

	xas_lock_irqsave(&xas, flags);
	tqp = xas_load(&xas);
	if (tqp && !tqp->closing && refcount_inc_not_zero(&tqp->refs))
		goto out;
	tqp = NULL;
out:
	xas_unlock_irqrestore(&xas, flags);
	return tqp;
}

static void tbv_qp_put(struct tbv_qp *tqp)
{
	if (tqp && refcount_dec_and_test(&tqp->refs))
		complete(&tqp->refs_zero);
}

static bool tbv_qp_get_live(struct tbv_qp *tqp)
{
	bool ok = false;
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	if (!tqp->closing && refcount_inc_not_zero(&tqp->refs))
		ok = true;
	spin_unlock_irqrestore(&tqp->lock, flags);
	return ok;
}

static void tbv_qp_queue_send(struct tbv_qp *tqp, struct tbv_send_ctx *send)
{
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	list_add_tail(&send->node, &tqp->pending_sends);
	spin_unlock_irqrestore(&tqp->lock, flags);
}

static void tbv_qp_queue_read(struct tbv_qp *tqp, struct tbv_read_ctx *read)
{
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	list_add_tail(&read->node, &tqp->pending_reads);
	spin_unlock_irqrestore(&tqp->lock, flags);
}

static bool tbv_qp_unqueue_send(struct tbv_qp *tqp, struct tbv_send_ctx *send)
{
	struct tbv_send_ctx *pos;
	unsigned long flags;
	bool found = false;

	spin_lock_irqsave(&tqp->lock, flags);
	list_for_each_entry(pos, &tqp->pending_sends, node) {
		if (pos == send) {
			list_del_init(&send->node);
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&tqp->lock, flags);
	return found;
}

static bool tbv_qp_unqueue_read(struct tbv_qp *tqp, struct tbv_read_ctx *read)
{
	struct tbv_read_ctx *pos;
	unsigned long flags;
	bool found = false;

	spin_lock_irqsave(&tqp->lock, flags);
	list_for_each_entry(pos, &tqp->pending_reads, node) {
		if (pos == read) {
			list_del_init(&read->node);
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&tqp->lock, flags);
	return found;
}

static struct tbv_send_ctx *tbv_qp_take_send(struct tbv_qp *tqp, u32 psn)
{
	struct tbv_send_ctx *send;
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	list_for_each_entry(send, &tqp->pending_sends, node) {
		if (send->psn == psn) {
			list_del_init(&send->node);
			spin_unlock_irqrestore(&tqp->lock, flags);
			return send;
		}
	}
	spin_unlock_irqrestore(&tqp->lock, flags);
	return NULL;
}

static struct tbv_read_ctx *tbv_qp_find_read_get(struct tbv_qp *tqp, u32 psn)
{
	struct tbv_read_ctx *read;
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	list_for_each_entry(read, &tqp->pending_reads, node) {
		if (read->psn == psn && refcount_inc_not_zero(&read->refs)) {
			spin_unlock_irqrestore(&tqp->lock, flags);
			return read;
		}
	}
	spin_unlock_irqrestore(&tqp->lock, flags);
	return NULL;
}

static void tbv_qp_flush_sends(struct tbv_qp *tqp, struct list_head *flush)
{
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	list_splice_init(&tqp->pending_sends, flush);
	spin_unlock_irqrestore(&tqp->lock, flags);
}

static void tbv_qp_flush_reads(struct tbv_qp *tqp, struct list_head *flush)
{
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	list_splice_init(&tqp->pending_reads, flush);
	spin_unlock_irqrestore(&tqp->lock, flags);
}

static u32 tbv_cancel_send_ctx_packets(struct tbv_state *state,
				       struct tbv_send_ctx *send)
{
	struct tbv_peer *peer;
	u32 canceled = 0;

	if (!state || !send)
		return 0;

	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		if (peer->backend != TBV_BACKEND_NATIVE &&
		    peer->backend != TBV_BACKEND_APPLE)
			continue;
		list_for_each_entry(rail, &peer->rails, node) {
			canceled += tbv_path_cancel_data_done_ctx(
				&rail->path, tbv_send_tx_done, send);
			canceled += tbv_path_cancel_data_done_ctx(
				&rail->path, tbv_apple_send_tx_done, send);
		}
	}
	mutex_unlock(&state->lock);
	return canceled;
}

static u32 tbv_cancel_read_ctx_packets(struct tbv_state *state,
				       struct tbv_read_ctx *read)
{
	struct tbv_peer *peer;
	u32 canceled = 0;

	if (!state || !read)
		return 0;

	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		if (peer->backend != TBV_BACKEND_NATIVE)
			continue;
		list_for_each_entry(rail, &peer->rails, node)
			canceled += tbv_path_cancel_data_done_ctx(
				&rail->path, tbv_read_tx_done, read);
	}
	mutex_unlock(&state->lock);
	return canceled;
}

static struct tbv_path *tbv_first_active_native_path_locked(struct tbv_state *state)
{
	struct tbv_peer *peer;

	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		if (peer->backend != TBV_BACKEND_NATIVE)
			continue;
		list_for_each_entry(rail, &peer->rails, node) {
			if (tbv_rail_data_ready(rail))
				return &rail->path;
		}
	}

	return NULL;
}

static bool tbv_rail_get_data_ready_locked(struct tbv_rail *rail)
{
	if (!tbv_rail_data_ready(rail) || rail->removing)
		return false;

	refcount_inc(&rail->refcnt);
	return true;
}

static bool tbv_rail_get_apple_data_ready_locked(struct tbv_rail *rail)
{
	if (!tbv_rail_apple_data_ready(rail) || rail->removing)
		return false;

	refcount_inc(&rail->refcnt);
	return true;
}

static struct tbv_path *tbv_first_active_apple_path_locked(struct tbv_state *state)
{
	struct tbv_peer *peer;

	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		if (peer->backend != TBV_BACKEND_APPLE)
			continue;
		list_for_each_entry(rail, &peer->rails, node) {
			if (tbv_rail_get_apple_data_ready_locked(rail))
				return &rail->path;
		}
	}

	return NULL;
}

/* The selector is either the QPN for stable-QP routing or the PSN for WR striping. */
static struct tbv_path *
tbv_select_native_data_path_get_locked(struct tbv_state *state, u32 selector)
{
	struct tbv_peer *peer;
	u32 active = 0;
	u32 target;
	u32 idx = 0;

	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		if (peer->backend != TBV_BACKEND_NATIVE)
			continue;
		list_for_each_entry(rail, &peer->rails, node) {
			if (tbv_rail_data_ready(rail) && !rail->removing)
				active++;
		}
	}

	if (!active)
		return NULL;

	target = selector % active;

	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		if (peer->backend != TBV_BACKEND_NATIVE)
			continue;
		list_for_each_entry(rail, &peer->rails, node) {
			if (!tbv_rail_data_ready(rail) || rail->removing)
				continue;
			if (idx++ == target &&
			    tbv_rail_get_data_ready_locked(rail))
				return &rail->path;
		}
	}

	return NULL;
}

static u32 tbv_collect_native_data_paths_get_locked(struct tbv_state *state,
						    struct tbv_path **paths,
						    u32 max_paths)
{
	struct tbv_peer *peer;
	u32 count = 0;

	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		if (peer->backend != TBV_BACKEND_NATIVE)
			continue;
		list_for_each_entry(rail, &peer->rails, node) {
			if (!tbv_rail_data_ready(rail) || rail->removing)
				continue;
			if (count < max_paths &&
			    tbv_rail_get_data_ready_locked(rail))
				paths[count] = &rail->path;
			count++;
		}
	}

	return min(count, max_paths);
}

static void tbv_release_path_refs(struct tbv_path **paths, u32 path_count)
{
	u32 i;

	for (i = 0; i < path_count; i++) {
		if (paths[i] && paths[i]->rail)
			tbv_rail_put(paths[i]->rail);
	}
}

static void tbv_kick_paths(struct tbv_path **paths, u32 path_count)
{
	u32 i;

	for (i = 0; i < path_count; i++)
		tbv_path_kick_tx(paths[i]);
}

static void tbv_release_path_reservations(struct tbv_path **paths,
					  const u32 *reservations,
					  u32 path_count)
{
	u32 i;

	for (i = 0; i < path_count; i++) {
		if (reservations[i])
			tbv_path_release_data_reservation(paths[i],
							  reservations[i]);
	}
}

static bool tbv_ibdev_port_active(struct tbv_state *state)
{
	struct tbv_peer *peer;
	bool active = false;

	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		list_for_each_entry(rail, &peer->rails, node) {
			if ((peer->backend == TBV_BACKEND_APPLE ?
			     tbv_rail_apple_data_ready(rail) :
			     tbv_rail_data_ready(rail))) {
				active = true;
				goto out;
			}
		}
	}

out:
	mutex_unlock(&state->lock);
	return active;
}

static int tbv_query_device(struct ib_device *ibdev,
			    struct ib_device_attr *attr,
			    struct ib_udata *udata)
{
	bool apple = tbv_state_apple_only(tbv_ibdev_state(ibdev));

	memset(attr, 0, sizeof(*attr));
	attr->vendor_id = 0x1d6b;
	attr->vendor_part_id = 0x5442;
	attr->hw_ver = 1;
	attr->fw_ver = 0;
	attr->sys_image_guid = ibdev->node_guid;
	attr->device_cap_flags = IB_DEVICE_CHANGE_PHY_PORT;
	attr->kernel_cap_flags = IBK_LOCAL_DMA_LKEY;
	attr->max_mr_size = U64_MAX;
	attr->page_size_cap = TBV_IBDEV_PAGE_SIZE_CAP;
	attr->max_qp = apple ? 1 : TBV_IBDEV_MAX_QP;
	attr->max_qp_wr = TBV_IBDEV_MAX_QP_WR;
	attr->max_send_sge = TBV_IBDEV_MAX_SGE;
	attr->max_recv_sge = TBV_IBDEV_MAX_SGE;
	attr->max_sge_rd = TBV_IBDEV_MAX_SGE;
	attr->max_cq = TBV_IBDEV_MAX_CQ;
	attr->max_cqe = TBV_IBDEV_MAX_CQE;
	attr->max_mr = 1024;
	attr->max_pd = 256;
	attr->max_qp_rd_atom = apple ? 0 : TBV_IBDEV_MAX_READ_CTX;
	attr->max_res_rd_atom = apple ? 0 :
				 TBV_IBDEV_MAX_QP * TBV_IBDEV_MAX_READ_CTX;
	attr->max_qp_init_rd_atom = apple ? 0 : TBV_IBDEV_MAX_READ_CTX;
	attr->atomic_cap = IB_ATOMIC_NONE;
	attr->max_pkeys = 1;
	attr->local_ca_ack_delay = 15;
	return 0;
}

static int tbv_query_port(struct ib_device *ibdev, u32 port_num,
			  struct ib_port_attr *attr)
{
	struct tbv_ibdev *dev = container_of(ibdev, struct tbv_ibdev, base);
	bool active;

	if (port_num != 1)
		return -EINVAL;

	memset(attr, 0, sizeof(*attr));
	active = tbv_ibdev_port_active(dev->state);
	attr->state = active ? IB_PORT_ACTIVE : IB_PORT_DOWN;
	attr->phys_state = active ? IB_PORT_PHYS_STATE_LINK_UP :
				    IB_PORT_PHYS_STATE_DISABLED;
	attr->max_mtu = IB_MTU_4096;
	attr->active_mtu = IB_MTU_4096;
	attr->max_msg_sz = tbv_state_apple_only(dev->state) ?
				   TBV_APPLE_MAX_MSG_SIZE :
				   TBV_NATIVE_DATA_MAX_MSG_SIZE;
	attr->gid_tbl_len = TBV_IBDEV_GID_TBL_LEN;
	attr->pkey_tbl_len = 1;
	attr->max_vl_num = 1;
	attr->active_width = IB_WIDTH_4X;
	attr->active_speed = IB_SPEED_FDR10;
	return 0;
}

static int tbv_get_port_immutable(struct ib_device *ibdev, u32 port_num,
				  struct ib_port_immutable *immutable)
{
	struct ib_port_attr attr;
	int ret;

	ret = tbv_query_port(ibdev, port_num, &attr);
	if (ret)
		return ret;

	immutable->pkey_tbl_len = attr.pkey_tbl_len;
	immutable->gid_tbl_len = attr.gid_tbl_len;
	immutable->core_cap_flags = RDMA_CORE_PORT_IBA_ROCE_UDP_ENCAP;
	immutable->max_mad_size = IB_MGMT_MAD_SIZE;
	return 0;
}

static enum rdma_link_layer tbv_get_link_layer(struct ib_device *ibdev,
					       u32 port_num)
{
	return IB_LINK_LAYER_ETHERNET;
}

static int tbv_query_gid(struct ib_device *ibdev, u32 port_num, int index,
			 union ib_gid *gid)
{
	if (port_num != 1 || index < 0 || index >= TBV_IBDEV_GID_TBL_LEN)
		return -EINVAL;

	memset(gid, 0, sizeof(*gid));
	gid->global.subnet_prefix = cpu_to_be64(0xfe80000000000000ULL);
	gid->global.interface_id = ibdev->node_guid;
	return 0;
}

static struct net_device *tbv_get_netdev(struct ib_device *ibdev,
					 u32 port_num)
{
	if (port_num != 1 || !roce_netdev || !*roce_netdev)
		return NULL;

	return dev_get_by_name(&init_net, roce_netdev);
}

static int tbv_add_gid(const struct ib_gid_attr *attr, void **context)
{
	*context = NULL;
	return 0;
}

static int tbv_del_gid(const struct ib_gid_attr *attr, void **context)
{
	*context = NULL;
	return 0;
}

static int tbv_query_pkey(struct ib_device *ibdev, u32 port_num, u16 index,
			  u16 *pkey)
{
	if (port_num != 1 || index)
		return -EINVAL;
	*pkey = 0xffff;
	return 0;
}

static int tbv_alloc_ucontext(struct ib_ucontext *context,
			      struct ib_udata *udata)
{
	struct tbv_ucontext *ctx = container_of(context, struct tbv_ucontext,
						base);

	ctx->owner = tbv_ibdev_state(context->device);
	atomic_inc(&ctx->owner->verbs_ucontexts);
	return 0;
}

static void tbv_dealloc_ucontext(struct ib_ucontext *context)
{
	struct tbv_ucontext *ctx = container_of(context, struct tbv_ucontext,
						base);

	if (ctx->owner)
		atomic_dec(&ctx->owner->verbs_ucontexts);
}

static int tbv_alloc_pd(struct ib_pd *pd, struct ib_udata *udata)
{
	struct tbv_pd *tpd = container_of(pd, struct tbv_pd, base);

	tpd->owner = tbv_ibdev_state(pd->device);
	atomic_inc(&tpd->owner->verbs_pds);
	return 0;
}

static int tbv_dealloc_pd(struct ib_pd *pd, struct ib_udata *udata)
{
	struct tbv_pd *tpd = container_of(pd, struct tbv_pd, base);

	if (tpd->owner)
		atomic_dec(&tpd->owner->verbs_pds);
	return 0;
}

static int tbv_create_cq(struct ib_cq *cq, const struct ib_cq_init_attr *attr,
			 struct uverbs_attr_bundle *attrs)
{
	struct tbv_cq *tcq = container_of(cq, struct tbv_cq, base);

	if (!attr || attr->cqe <= 0 || attr->cqe > TBV_IBDEV_MAX_CQE)
		return -EINVAL;

	tcq->entries = kcalloc(attr->cqe, sizeof(*tcq->entries), GFP_KERNEL);
	if (!tcq->entries)
		return -ENOMEM;

	spin_lock_init(&tcq->lock);
	tcq->owner = tbv_ibdev_state(cq->device);
	tcq->cqe = attr->cqe;
	atomic_inc(&tcq->owner->verbs_cqs);
	return 0;
}

static int tbv_destroy_cq(struct ib_cq *cq, struct ib_udata *udata)
{
	struct tbv_cq *tcq = container_of(cq, struct tbv_cq, base);

	if (tcq->owner)
		atomic_dec(&tcq->owner->verbs_cqs);
	kfree(tcq->entries);
	return 0;
}

static int tbv_create_qp(struct ib_qp *qp, struct ib_qp_init_attr *init_attr,
			 struct ib_udata *udata)
{
	struct tbv_qp *tqp = container_of(qp, struct tbv_qp, base);
	struct tbv_state *state = tbv_ibdev_state(qp->device);
	unsigned long flags;
	int qpn;
	int ret;

	if (!init_attr || init_attr->srq)
		return -EOPNOTSUPP;
	if (init_attr->qp_type != IB_QPT_RC && init_attr->qp_type != IB_QPT_UC)
		return -EOPNOTSUPP;
	if (tbv_state_apple_only(state) && init_attr->qp_type != IB_QPT_UC)
		return -EOPNOTSUPP;
	if (init_attr->cap.max_send_wr > TBV_IBDEV_MAX_QP_WR ||
	    init_attr->cap.max_recv_wr > TBV_IBDEV_MAX_QP_WR ||
	    init_attr->cap.max_send_sge > TBV_IBDEV_MAX_SGE ||
	    init_attr->cap.max_recv_sge > TBV_IBDEV_MAX_SGE)
		return -EINVAL;

	qpn = tbv_alloc_qpn(state);
	if (qpn < 0)
		return qpn;

	if (init_attr->cap.max_recv_wr) {
		tqp->recvq = kcalloc(init_attr->cap.max_recv_wr,
				     sizeof(*tqp->recvq), GFP_KERNEL);
		if (!tqp->recvq) {
			ida_free(&tbv_qpn_ida, qpn);
			return -ENOMEM;
		}
		tqp->recvq_size = init_attr->cap.max_recv_wr;
	}

	if (tbv_state_apple_only(state) && READ_ONCE(apple_rx_pending_bytes) &&
	    READ_ONCE(apple_rx_pending_total_bytes)) {
		u32 slots = min_t(u32, READ_ONCE(apple_rx_pending_slots),
				  TBV_APPLE_PENDING_RX_MAX_SLOTS);

		if (slots) {
			tqp->apple_pending =
				kvcalloc(slots, sizeof(*tqp->apple_pending),
					 GFP_KERNEL);
			if (!tqp->apple_pending) {
				kfree(tqp->recvq);
				ida_free(&tbv_qpn_ida, qpn);
				return -ENOMEM;
			}
			tqp->apple_pending_slot_count = slots;
		}
	}

	tqp->init_attr = *init_attr;
	tqp->owner = state;
	spin_lock_init(&tqp->lock);
	mutex_init(&tqp->rx_lock);
	init_waitqueue_head(&tqp->credit_wait);
	init_waitqueue_head(&tqp->apple_tx_wait);
	refcount_set(&tqp->refs, 1);
	atomic_set(&tqp->apple_tx_inflight, 0);
	atomic_set(&tqp->apple_tx_inflight_frames, 0);
	atomic_set(&tqp->apple_tx_pace_queued_frames, 0);
	init_completion(&tqp->refs_zero);
	INIT_LIST_HEAD(&tqp->pending_sends);
	INIT_LIST_HEAD(&tqp->pending_reads);
	INIT_LIST_HEAD(&tqp->rx_reorder);
	tqp->apple_pending_active = -1;
	tqp->state = IB_QPS_RESET;
	tqp->type = init_attr->qp_type;
	tqp->qpn_allocated = true;
	qp->qp_num = qpn;
	init_attr->cap.max_inline_data = 0;
	xa_lock_irqsave(&tqp->owner->verbs_qps_xa, flags);
	ret = __xa_insert(&tqp->owner->verbs_qps_xa, qpn, tqp, GFP_KERNEL);
	xa_unlock_irqrestore(&tqp->owner->verbs_qps_xa, flags);
	if (ret) {
		kvfree(tqp->apple_pending);
		kfree(tqp->recvq);
		ida_free(&tbv_qpn_ida, qpn);
		tqp->qpn_allocated = false;
		return ret;
	}
	atomic_inc(&tqp->owner->verbs_qps);
	return 0;
}

static int tbv_destroy_qp(struct ib_qp *qp, struct ib_udata *udata)
{
	struct tbv_qp *tqp = container_of(qp, struct tbv_qp, base);
	LIST_HEAD(flush);
	unsigned long flags;
	u32 pending;
	u32 i;

	spin_lock_irqsave(&tqp->lock, flags);
	tqp->closing = true;
	spin_unlock_irqrestore(&tqp->lock, flags);
	wake_up_all(&tqp->credit_wait);
	wake_up_all(&tqp->apple_tx_wait);

	if (tqp->owner) {
		xa_lock_irqsave(&tqp->owner->verbs_qps_xa, flags);
		__xa_erase(&tqp->owner->verbs_qps_xa, qp->qp_num);
		xa_unlock_irqrestore(&tqp->owner->verbs_qps_xa, flags);
	}

	tbv_qp_flush_sends(tqp, &flush);
	while (!list_empty(&flush)) {
		struct tbv_send_ctx *send =
			list_first_entry(&flush, struct tbv_send_ctx, node);
		u32 canceled;

		list_del_init(&send->node);
		canceled = tbv_cancel_send_ctx_packets(tqp->owner, send);
		while (canceled--)
			tbv_send_ctx_put(send);
		tbv_send_complete(send, -ECANCELED);
		tbv_send_ctx_put(send);
	}

	tbv_qp_flush_reads(tqp, &flush);
	while (!list_empty(&flush)) {
		struct tbv_read_ctx *read =
			list_first_entry(&flush, struct tbv_read_ctx, node);
		u32 canceled;

		list_del_init(&read->node);
		canceled = tbv_cancel_read_ctx_packets(tqp->owner, read);
		while (canceled--)
			tbv_read_ctx_put(read);
		tbv_read_complete(read, -ECANCELED);
		tbv_read_ctx_put(read);
	}

	tbv_qp_put(tqp);
	wait_for_completion(&tqp->refs_zero);
	tbv_qp_flush_reorder(tqp);
	tbv_qp_flush_apple_pending(tqp);
	for (i = 0; i < tqp->apple_pending_slot_count; i++)
		kvfree(tqp->apple_pending[i].buf);
	kvfree(tqp->apple_pending);

	pending = tqp->recv_count;
	if (tqp->owner && pending)
		atomic_sub(pending, &tqp->owner->verbs_recv_wqes);
	for (i = 0; i < pending; i++) {
		tqp->recv_head = (tqp->recv_head + 1) % tqp->recvq_size;
	}
	kfree(tqp->recvq);
	if (tqp->qpn_allocated) {
		ida_free(&tbv_qpn_ida, qp->qp_num);
		tqp->qpn_allocated = false;
	}
	if (tqp->owner)
		atomic_dec(&tqp->owner->verbs_qps);
	return 0;
}

static int tbv_modify_qp(struct ib_qp *qp, struct ib_qp_attr *attr,
			 int attr_mask, struct ib_udata *udata)
{
	struct tbv_qp *tqp = container_of(qp, struct tbv_qp, base);

	if (!attr)
		return -EINVAL;

	if (attr_mask & IB_QP_STATE) {
		tqp->state = attr->qp_state;
		tqp->attr.qp_state = attr->qp_state;
	}
	if (attr_mask & IB_QP_PKEY_INDEX)
		tqp->attr.pkey_index = attr->pkey_index;
	if (attr_mask & IB_QP_PORT)
		tqp->attr.port_num = attr->port_num;
	if (attr_mask & IB_QP_ACCESS_FLAGS)
		tqp->attr.qp_access_flags = attr->qp_access_flags;
	if (attr_mask & IB_QP_AV)
		tqp->attr.ah_attr = attr->ah_attr;
	if (attr_mask & IB_QP_PATH_MTU)
		tqp->attr.path_mtu = attr->path_mtu;
	if (attr_mask & IB_QP_DEST_QPN)
		tqp->attr.dest_qp_num = attr->dest_qp_num;
	if (attr_mask & IB_QP_RQ_PSN) {
		tqp->rx_expected_psn = attr->rq_psn & TBV_PSN_MASK;
		tqp->attr.rq_psn = attr->rq_psn & TBV_PSN_MASK;
	}
	if ((attr_mask & IB_QP_MAX_DEST_RD_ATOMIC) &&
	    attr->max_dest_rd_atomic > TBV_IBDEV_MAX_READ_CTX)
		return -EINVAL;
	if (attr_mask & IB_QP_MAX_DEST_RD_ATOMIC)
		tqp->attr.max_dest_rd_atomic = attr->max_dest_rd_atomic;
	if (attr_mask & IB_QP_MIN_RNR_TIMER)
		tqp->attr.min_rnr_timer = attr->min_rnr_timer;
	if (attr_mask & IB_QP_SQ_PSN) {
		tqp->send_psn = attr->sq_psn & TBV_PSN_MASK;
		tqp->attr.sq_psn = attr->sq_psn & TBV_PSN_MASK;
	}
	if (attr_mask & IB_QP_TIMEOUT)
		tqp->attr.timeout = attr->timeout;
	if (attr_mask & IB_QP_RETRY_CNT)
		tqp->attr.retry_cnt = attr->retry_cnt;
	if (attr_mask & IB_QP_RNR_RETRY)
		tqp->attr.rnr_retry = attr->rnr_retry;
	if ((attr_mask & IB_QP_MAX_QP_RD_ATOMIC) &&
	    attr->max_rd_atomic > TBV_IBDEV_MAX_READ_CTX)
		return -EINVAL;
	if (attr_mask & IB_QP_MAX_QP_RD_ATOMIC)
		tqp->attr.max_rd_atomic = attr->max_rd_atomic;
	if (attr_mask & (IB_QP_STATE | IB_QP_DEST_QPN))
		tbv_qp_advertise_recv_credits(tqp);
	return 0;
}

static int tbv_query_qp(struct ib_qp *qp, struct ib_qp_attr *attr,
			int attr_mask, struct ib_qp_init_attr *init_attr)
{
	struct tbv_qp *tqp = container_of(qp, struct tbv_qp, base);

	if (attr)
		*attr = tqp->attr;
	if (attr)
		attr->qp_state = tqp->state;
	if (init_attr)
		*init_attr = tqp->init_attr;
	return 0;
}

static void tbv_send_ctx_get(struct tbv_send_ctx *send)
{
	refcount_inc(&send->refs);
}

static void tbv_send_ctx_put(struct tbv_send_ctx *send)
{
	if (refcount_dec_and_test(&send->refs)) {
		atomic64_dec(&send->tqp->owner->data_wr_live);
		tbv_qp_put(send->tqp);
		kfree(send);
	}
}

static void tbv_qp_release_apple_tx_window(struct tbv_qp *tqp,
					   bool wr_acquired,
					   u32 frames_acquired)
{
	if (wr_acquired)
		atomic_dec(&tqp->apple_tx_inflight);
	if (frames_acquired)
		atomic_sub(frames_acquired, &tqp->apple_tx_inflight_frames);
	if (atomic_read(&tqp->apple_tx_inflight) <= 0 &&
	    atomic_read(&tqp->apple_tx_inflight_frames) <= 0)
		atomic_set(&tqp->apple_tx_pace_queued_frames, 0);
	if (wr_acquired || frames_acquired)
		wake_up_all(&tqp->apple_tx_wait);
}

static bool tbv_send_complete(struct tbv_send_ctx *send, int status)
{
	struct tbv_qp *tqp = send->tqp;
	unsigned long flags;
	bool complete = false;

	spin_lock_irqsave(&send->lock, flags);
	if (!send->completed) {
		send->completed = true;
		complete = true;
	}
	spin_unlock_irqrestore(&send->lock, flags);
	if (!complete)
		return false;

	if (send->apple_window_acquired) {
		tbv_qp_release_apple_tx_window(tqp,
						send->apple_window_wr_acquired,
						send->apple_window_frames);
		send->apple_window_acquired = false;
	}

	if (send->signaled) {
		struct tbv_cq *send_cq =
			container_of(tqp->base.send_cq, struct tbv_cq, base);
		struct ib_wc wc = {};

		wc.wr_id = send->wr_id;
		wc.status = status ? IB_WC_WR_FLUSH_ERR : IB_WC_SUCCESS;
		wc.opcode = send->wc_opcode;
		wc.qp = &tqp->base;
		wc.port_num = 1;
		tbv_cq_push(send_cq, &wc);
	}

	return true;
}

static void tbv_read_ctx_get(struct tbv_read_ctx *read)
{
	refcount_inc(&read->refs);
}

static void tbv_release_read_segments(struct tbv_read_segment *segs, int nsegs)
{
	int i;

	for (i = 0; i < nsegs; i++)
		tbv_mr_put(segs[i].mr);
}

static void tbv_read_ctx_put(struct tbv_read_ctx *read)
{
	if (refcount_dec_and_test(&read->refs)) {
		tbv_release_read_segments(read->segs, read->nsegs);
		atomic64_dec(&read->tqp->owner->data_wr_live);
		tbv_qp_put(read->tqp);
		kfree(read);
	}
}

static enum ib_wc_status tbv_read_wc_status(int status)
{
	if (!status)
		return IB_WC_SUCCESS;
	if (status == -ECANCELED)
		return IB_WC_WR_FLUSH_ERR;
	if (status == -EACCES || status == -EFAULT || status == -EPERM)
		return IB_WC_REM_ACCESS_ERR;
	return IB_WC_GENERAL_ERR;
}

static bool tbv_read_complete(struct tbv_read_ctx *read, int status)
{
	struct tbv_qp *tqp = read->tqp;
	unsigned long flags;
	bool complete = false;

	spin_lock_irqsave(&read->lock, flags);
	if (!read->completed) {
		read->completed = true;
		complete = true;
	}
	spin_unlock_irqrestore(&read->lock, flags);
	if (!complete)
		return false;

	if (read->signaled) {
		struct tbv_cq *send_cq =
			container_of(tqp->base.send_cq, struct tbv_cq, base);
		struct ib_wc wc = {};

		wc.wr_id = read->wr_id;
		wc.status = tbv_read_wc_status(status);
		wc.opcode = IB_WC_RDMA_READ;
		wc.qp = &tqp->base;
		wc.byte_len = status ? 0 : read->total_len;
		wc.port_num = 1;
		tbv_cq_push(send_cq, &wc);
	}

	return true;
}

static void tbv_read_tx_done(void *ctx, int status)
{
	struct tbv_read_ctx *read = ctx;
	struct tbv_qp *tqp = read->tqp;

	if (!status) {
		tbv_read_ctx_put(read);
		return;
	}

	if (tbv_qp_unqueue_read(tqp, read)) {
		tbv_read_complete(read, status);
		tbv_read_ctx_put(read);
	}
	tbv_read_ctx_put(read);
}

static void tbv_send_tx_done(void *ctx, int status)
{
	struct tbv_send_ctx *send = ctx;
	struct tbv_qp *tqp = send->tqp;

	if (!status) {
		tbv_send_ctx_put(send);
		return;
	}

	if (tbv_qp_unqueue_send(tqp, send)) {
		tbv_send_complete(send, status);
		tbv_send_ctx_put(send);
	}
	tbv_send_ctx_put(send);
}

static void tbv_release_send_segments(struct tbv_send_segment *segs, int nsegs)
{
	int i;

	for (i = 0; i < nsegs; i++)
		tbv_mr_put(segs[i].mr);
}

static int tbv_prepare_send_segments(struct tbv_qp *tqp,
				     const struct ib_send_wr *wr,
				     struct tbv_send_segment *segs,
				     int *nsegs_out, u32 *length_out)
{
	u32 total = 0;
	int nsegs = 0;
	int ret = 0;
	int i;

	if (wr->send_flags & IB_SEND_INLINE)
		return -EOPNOTSUPP;
	if (wr->num_sge > TBV_IBDEV_MAX_SGE)
		return -EINVAL;
	if (wr->num_sge && !wr->sg_list)
		return -EINVAL;

	for (i = 0; i < wr->num_sge; i++) {
		const struct ib_sge *sge = &wr->sg_list[i];
		struct tbv_mr *mr;
		u64 mr_end;
		u64 end;

		if (!sge->length)
			continue;
		if (check_add_overflow(total, sge->length, &total) ||
		    total > tbv_qp_max_msg_size(tqp)) {
			ret = -EMSGSIZE;
			goto err_release;
		}
		if (check_add_overflow(sge->addr, (u64)sge->length, &end)) {
			ret = -EINVAL;
			goto err_release;
		}

		mr = tbv_mr_get(tqp->owner, sge->lkey);
		if (!mr) {
			ret = -EINVAL;
			goto err_release;
		}
		if (check_add_overflow(mr->start, mr->length, &mr_end) ||
		    sge->addr < mr->start || end > mr_end) {
			tbv_mr_put(mr);
			ret = -EFAULT;
			goto err_release;
		}

		segs[nsegs].mr = mr;
		segs[nsegs].addr = sge->addr;
		segs[nsegs].length = sge->length;
		nsegs++;
	}

	*nsegs_out = nsegs;
	*length_out = total;
	return 0;

err_release:
	tbv_release_send_segments(segs, nsegs);
	return ret;
}

static int tbv_prepare_read_segments(struct tbv_qp *tqp,
				     const struct ib_send_wr *wr,
				     struct tbv_read_segment *segs,
				     int *nsegs_out, u32 *length_out)
{
	struct tbv_send_segment send_segs[TBV_IBDEV_MAX_SGE];
	u32 total = 0;
	int nsegs = 0;
	int copied = 0;
	int ret;
	int i;

	ret = tbv_prepare_send_segments(tqp, wr, send_segs, &nsegs, &total);
	if (ret)
		return ret;

	for (i = 0; i < nsegs; i++) {
		if (!(send_segs[i].mr->access & IB_ACCESS_LOCAL_WRITE)) {
			ret = -EACCES;
			goto err_release;
		}
		segs[i].mr = send_segs[i].mr;
		segs[i].addr = send_segs[i].addr;
		segs[i].length = send_segs[i].length;
		memset(&send_segs[i], 0, sizeof(send_segs[i]));
		copied++;
	}

	*nsegs_out = nsegs;
	*length_out = total;
	return 0;

err_release:
	tbv_release_read_segments(segs, copied);
	tbv_release_send_segments(send_segs, nsegs);
	return ret;
}

static int tbv_copy_send_range(const struct tbv_send_segment *segs, int nsegs,
			       u32 offset, void *dst, u32 length)
{
	u32 skipped = 0;
	u32 copied = 0;
	int i;

	if (!length)
		return 0;

	for (i = 0; i < nsegs; i++) {
		const struct tbv_send_segment *seg = &segs[i];
		u32 seg_off = 0;
		u32 chunk;
		int ret;

		if (offset >= skipped + seg->length) {
			skipped += seg->length;
			continue;
		}
		if (offset > skipped)
			seg_off = offset - skipped;

		chunk = min_t(u32, seg->length - seg_off, length - copied);
		ret = ib_umem_copy_from((u8 *)dst + copied, seg->mr->umem,
					seg->addr + seg_off - seg->mr->start,
					chunk);
		if (ret)
			return ret;
		copied += chunk;
		if (copied == length)
			return 0;
		skipped += seg->length;
	}

	return -EFAULT;
}

static void tbv_apple_send_tx_done(void *ctx, int status)
{
	struct tbv_send_ctx *send = ctx;
	struct tbv_qp *tqp = send->tqp;
	bool last;

	last = atomic_dec_and_test(&send->apple_pending);
	if (status) {
		if (tbv_qp_unqueue_send(tqp, send)) {
			tbv_send_complete(send, status);
			tbv_send_ctx_put(send);
		}
	} else if (last) {
		if (tbv_qp_unqueue_send(tqp, send)) {
			tbv_send_complete(send, 0);
			tbv_send_ctx_put(send);
		}
	}

	tbv_send_ctx_put(send);
}

static int tbv_apple_send_fill(void *ctx, void *dst, u32 len)
{
	struct tbv_apple_send_fill *fill = ctx;
	int ret;

	ret = tbv_copy_send_range(fill->segs, fill->nsegs, fill->offset, dst,
				  len);
	if (ret)
		atomic64_inc(&fill->tqp->owner->data_wr_copy_error);
	return ret;
}

static int tbv_post_apple_send(struct tbv_qp *tqp, const struct ib_send_wr *wr)
{
	struct tbv_send_segment segs[TBV_IBDEV_MAX_SGE];
	struct tbv_send_ctx *ctx = NULL;
	struct tbv_path *path = NULL;
	unsigned long flags;
	u32 total_len = 0;
	u32 offset = 0;
	u32 nfrags;
	u32 remaining;
	int nsegs = 0;
	bool apple_wr_acquired = false;
	u32 apple_frames_acquired = 0;
	bool sent_any = false;
	int ret;

	if (tqp->type != IB_QPT_UC)
		return -EOPNOTSUPP;
	if (wr->opcode != IB_WR_SEND)
		return -EOPNOTSUPP;
	if (!tqp->attr.dest_qp_num)
		return -EINVAL;

	atomic64_inc(&tqp->owner->data_wr_send);
	atomic64_inc(&tqp->owner->data_wr_op_send);
	if (!tbv_qp_get_live(tqp))
		return -EINVAL;
	atomic64_inc(&tqp->owner->data_wr_live);

	ret = tbv_prepare_send_segments(tqp, wr, segs, &nsegs, &total_len);
	if (ret) {
		atomic64_inc(&tqp->owner->data_wr_copy_error);
		goto err_put_qp;
	}
	if (!total_len) {
		ret = -EMSGSIZE;
		goto err_release_segs;
	}

	mutex_lock(&tqp->owner->lock);
	path = tbv_first_active_apple_path_locked(tqp->owner);
	mutex_unlock(&tqp->owner->lock);
	if (!path) {
		atomic64_inc(&tqp->owner->data_wr_no_path);
		ret = -ENOTCONN;
		goto err_release_segs;
	}

	nfrags = DIV_ROUND_UP(total_len, TBV_APPLE_FRAME_SIZE);

	ret = tbv_qp_acquire_apple_tx_window(tqp, nfrags,
					     &apple_wr_acquired,
					     &apple_frames_acquired);
	if (ret)
		goto err_put_path;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		tbv_qp_release_apple_tx_window(tqp, apple_wr_acquired,
					       apple_frames_acquired);
		ret = -ENOMEM;
		goto err_put_path;
	}
	ctx->tqp = tqp;
	refcount_set(&ctx->refs, 1);
	spin_lock_init(&ctx->lock);
	ctx->wr_id = wr->wr_id;
	ctx->signaled = !!(wr->send_flags & IB_SEND_SIGNALED);
	ctx->wc_opcode = IB_WC_SEND;
	ctx->apple_window_wr_acquired = apple_wr_acquired;
	ctx->apple_window_frames = apple_frames_acquired;
	ctx->apple_window_acquired = apple_wr_acquired || apple_frames_acquired;
	INIT_LIST_HEAD(&ctx->node);

	spin_lock_irqsave(&tqp->lock, flags);
	ctx->psn = tqp->send_psn & TBV_PSN_MASK;
	tqp->send_psn = tbv_psn_next(ctx->psn);
	spin_unlock_irqrestore(&tqp->lock, flags);

	remaining = nfrags;
	atomic_set(&ctx->apple_pending, nfrags);
	tbv_qp_queue_send(tqp, ctx);

	ret = tbv_path_reserve_data(path, nfrags);
	if (ret)
		goto err_unqueue_ctx;

	while (offset < total_len) {
		u32 payload_len = min_t(u32, total_len - offset,
					TBV_APPLE_FRAME_SIZE);
		bool last = offset + payload_len == total_len;
		u32 frame_index;
		unsigned int pace_us;
		unsigned int pace_interval;
		unsigned int pace_after;
		int queued_frames = 0;
		struct tbv_apple_send_fill fill = {
			.segs = segs,
			.tqp = tqp,
			.offset = offset,
			.nsegs = nsegs,
		};

		tbv_send_ctx_get(ctx);
		atomic64_inc(&tqp->owner->data_wr_path_send);
		ret = tbv_path_send_marked_fill(path, payload_len,
						1, last ? 3 : 2,
						TBV_PATH_SEND_DEFER,
						tbv_apple_send_fill, &fill,
						tbv_apple_send_tx_done, ctx);
		if (ret) {
			tbv_send_ctx_put(ctx);
			atomic64_inc(&tqp->owner->data_wr_path_send_error);
			goto err_release_reservation;
		}

		remaining--;
		frame_index = nfrags - remaining;
		sent_any = true;
		offset += payload_len;

		pace_us = READ_ONCE(apple_tx_pace_us);
		pace_interval = READ_ONCE(apple_tx_pace_interval_frames);
		pace_after = READ_ONCE(apple_tx_pace_after_frames);
		if (!pace_interval)
			pace_interval = 1;
		if (pace_us)
			queued_frames =
				atomic_inc_return(&tqp->apple_tx_pace_queued_frames);
		if (offset < total_len && pace_us &&
		    (unsigned int)queued_frames > pace_after &&
		    frame_index % pace_interval == 0)
			usleep_range(pace_us, pace_us + 10);
	}

	tbv_path_kick_tx(path);
	tbv_release_path_refs(&path, 1);
	tbv_release_send_segments(segs, nsegs);
	atomic64_inc(&tqp->owner->data_tx_accepted);
	return 0;

err_release_reservation:
	tbv_path_release_data_reservation(path, remaining);
	if (sent_any)
		tbv_path_kick_tx(path);
err_unqueue_ctx:
	tbv_qp_unqueue_send(tqp, ctx);
	tbv_send_complete(ctx, ret);
	tbv_send_ctx_put(ctx);
	tbv_release_path_refs(&path, 1);
	tbv_release_send_segments(segs, nsegs);
	return ret;
err_put_path:
	tbv_release_path_refs(&path, 1);
err_release_segs:
	tbv_release_send_segments(segs, nsegs);
err_put_qp:
	atomic64_dec(&tqp->owner->data_wr_live);
	tbv_qp_put(tqp);
	return ret;
}

static void tbv_send_page_stream_put(struct tbv_send_page_stream *stream)
{
	if (refcount_dec_and_test(&stream->refs)) {
		tbv_release_send_segments(stream->segs, stream->nsegs);
		kfree(stream);
	}
}

static void tbv_send_page_stream_done(void *ctx, int status)
{
	struct tbv_send_page_stream *stream = ctx;

	tbv_send_tx_done(stream->send, status);
	tbv_send_page_stream_put(stream);
}

static int tbv_send_page_stream_next(void *ctx, struct page **page,
				     u32 *page_off, u32 *length,
				     tbv_path_tx_done_fn *done,
				     void **done_ctx)
{
	struct tbv_send_page_stream *stream = ctx;
	u32 skipped = 0;
	int i;

	for (i = 0; i < stream->nsegs; i++) {
		struct tbv_send_segment *seg = &stream->segs[i];
		u32 seg_off = 0;
		u32 remaining;
		int ret;

		if (stream->offset >= skipped + seg->length) {
			skipped += seg->length;
			continue;
		}
		if (stream->offset > skipped)
			seg_off = stream->offset - skipped;

		remaining = min_t(u32, seg->length - seg_off,
				  stream->total_len - stream->offset);
		ret = tbv_umem_page_from_addr(seg->mr, seg->addr + seg_off,
					      remaining, page, page_off,
					      length);
		if (ret)
			return ret;

		stream->offset += *length;
		refcount_inc(&stream->refs);
		tbv_send_ctx_get(stream->send);
		*done = tbv_send_page_stream_done;
		*done_ctx = stream;
		return 0;
	}

	return -EFAULT;
}

static bool tbv_should_zcopy_payload(u32 len)
{
	return len && zcopy_min_bytes && len >= zcopy_min_bytes;
}

static bool tbv_page_zcopy_safe(struct page *page)
{
	/*
	 * Thunderbolt NHI DMA can stream ordinary system RAM. GPU/HMM/device
	 * pages need the copied path unless/until this driver grows a real
	 * peer-direct contract with the GPU driver.
	 */
	return !is_zone_device_page(page);
}

static bool tbv_send_segments_zcopy_safe(struct tbv_send_segment *segs,
					 int nsegs, u32 total_len)
{
	u32 offset = 0;

	while (offset < total_len) {
		u32 skipped = 0;
		bool found = false;
		int i;

		for (i = 0; i < nsegs; i++) {
			struct tbv_send_segment *seg = &segs[i];
			struct page *page;
			u32 page_off;
			u32 len;
			u32 seg_off = 0;
			u32 remaining;
			int ret;

			if (offset >= skipped + seg->length) {
				skipped += seg->length;
				continue;
			}
			if (offset > skipped)
				seg_off = offset - skipped;

			remaining = min_t(u32, seg->length - seg_off,
					  total_len - offset);
			ret = tbv_umem_page_from_addr(seg->mr,
						      seg->addr + seg_off,
						      remaining, &page,
						      &page_off, &len);
			if (ret || !len)
				return false;
			if (!tbv_page_zcopy_safe(page))
				return false;

			offset += len;
			found = true;
			break;
		}

		if (!found)
			return false;
	}

	return true;
}

static int tbv_post_rdma_read(struct tbv_qp *tqp, const struct ib_send_wr *wr)
{
	struct tbv_read_segment segs[TBV_IBDEV_MAX_SGE];
	const struct ib_rdma_wr *rwr = rdma_wr(wr);
	struct tbv_native_data_header hdr = {};
	struct tbv_read_ctx *ctx;
	struct tbv_path *path;
	unsigned long flags;
	u32 total_len = 0;
	u64 remote_end;
	u8 *frame;
	u32 psn;
	int nsegs = 0;
	int len;
	int ret;

	if (!tqp->attr.dest_qp_num)
		return -EINVAL;
	if (check_add_overflow(rwr->remote_addr, (u64)0, &remote_end))
		return -EINVAL;

	atomic64_inc(&tqp->owner->data_wr_send);
	if (!tbv_qp_get_live(tqp))
		return -EINVAL;
	atomic64_inc(&tqp->owner->data_wr_live);

	ret = tbv_prepare_read_segments(tqp, wr, segs, &nsegs, &total_len);
	if (ret) {
		atomic64_inc(&tqp->owner->data_wr_copy_error);
		goto err_put_qp;
	}
	if (check_add_overflow(rwr->remote_addr, (u64)total_len,
			       &remote_end)) {
		ret = -EINVAL;
		goto err_release_segs;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ret = -ENOMEM;
		goto err_release_segs;
	}
	ctx->tqp = tqp;
	refcount_set(&ctx->refs, 1);
	spin_lock_init(&ctx->lock);
	ctx->wr_id = wr->wr_id;
	ctx->signaled = !!(wr->send_flags & IB_SEND_SIGNALED);
	ctx->total_len = total_len;
	ctx->nsegs = nsegs;
	memcpy(ctx->segs, segs, sizeof(ctx->segs));
	memset(segs, 0, sizeof(segs));
	INIT_LIST_HEAD(&ctx->node);

	spin_lock_irqsave(&tqp->lock, flags);
	psn = tqp->send_psn & TBV_PSN_MASK;
	tqp->send_psn = tbv_psn_next(psn);
	spin_unlock_irqrestore(&tqp->lock, flags);
	ctx->psn = psn;

	frame = kzalloc(TBV_NATIVE_DATA_HDR_SIZE, GFP_KERNEL);
	if (!frame) {
		ret = -ENOMEM;
		goto err_put_ctx;
	}

	hdr.opcode = TBV_NATIVE_DATA_OP_RDMA_READ_REQ;
	hdr.flags = TBV_NATIVE_DATA_F_LAST;
	hdr.dest_qp = tqp->attr.dest_qp_num;
	hdr.src_qp = tqp->base.qp_num;
	hdr.psn = psn;
	hdr.imm_data = total_len;
	hdr.remote_addr = rwr->remote_addr;
	hdr.rkey = rwr->rkey;

	len = tbv_native_data_build_header(frame, TBV_NATIVE_DATA_HDR_SIZE,
					   &hdr);
	if (len < 0) {
		kfree(frame);
		ret = len;
		goto err_put_ctx;
	}

	mutex_lock(&tqp->owner->lock);
	path = tbv_select_native_data_path_get_locked(tqp->owner,
						     tqp->base.qp_num ^ psn);
	mutex_unlock(&tqp->owner->lock);
	if (!path) {
		kfree(frame);
		atomic64_inc(&tqp->owner->data_wr_no_path);
		ret = -ENOTCONN;
		goto err_put_ctx;
	}

	tbv_qp_queue_read(tqp, ctx);
	tbv_read_ctx_get(ctx);
	atomic64_inc(&tqp->owner->data_wr_path_send);
	ret = tbv_path_send_owned(path, frame, len, 0, tbv_read_tx_done, ctx);
	tbv_release_path_refs(&path, 1);
	if (ret) {
		tbv_read_ctx_put(ctx);
		tbv_qp_unqueue_read(tqp, ctx);
		atomic64_inc(&tqp->owner->data_wr_path_send_error);
		goto err_put_ctx;
	}

	atomic64_inc(&tqp->owner->data_tx_accepted);
	return 0;

err_put_ctx:
	tbv_read_ctx_put(ctx);
	return ret;
err_release_segs:
	tbv_release_read_segments(segs, nsegs);
err_put_qp:
	atomic64_dec(&tqp->owner->data_wr_live);
	tbv_qp_put(tqp);
	return ret;
}

static int tbv_post_send_one(struct tbv_qp *tqp, const struct ib_send_wr *wr)
{
	struct tbv_send_segment segs[TBV_IBDEV_MAX_SGE];
	struct tbv_native_data_header hdr = {};
	struct tbv_send_ctx *ctx;
	struct tbv_path *path = NULL;
	struct tbv_path *paths[TBV_NATIVE_MAX_LANES] = {};
	u32 reservations[TBV_NATIVE_MAX_LANES] = {};
	unsigned long flags;
	u32 total_len = 0;
	u32 offset = 0;
	u32 psn;
	u32 nfrags;
	u32 frag_idx = 0;
	u32 path_count = 0;
	int nsegs = 0;
	bool fragment_striping;
	bool wr_striping;
	bool credit_consumed = false;
	bool sent_any = false;
	bool is_send = wr->opcode == IB_WR_SEND ||
		       wr->opcode == IB_WR_SEND_WITH_IMM;
	bool send_with_imm = wr->opcode == IB_WR_SEND_WITH_IMM;
	bool is_write = wr->opcode == IB_WR_RDMA_WRITE ||
			wr->opcode == IB_WR_RDMA_WRITE_WITH_IMM;
	bool write_with_imm = wr->opcode == IB_WR_RDMA_WRITE_WITH_IMM;
	int ret;

	if (tbv_qp_uses_apple_transport(tqp))
		return tbv_post_apple_send(tqp, wr);

	if (wr->opcode == IB_WR_RDMA_READ)
		return tbv_post_rdma_read(tqp, wr);

	if (!is_send && !is_write) {
		atomic64_inc(&tqp->owner->data_wr_op_unsupported);
		return -EOPNOTSUPP;
	}
	if (is_write && !tqp->attr.dest_qp_num)
		return -EINVAL;
	atomic64_inc(&tqp->owner->data_wr_send);
	if (send_with_imm)
		atomic64_inc(&tqp->owner->data_wr_op_send_imm);
	else if (is_send)
		atomic64_inc(&tqp->owner->data_wr_op_send);
	else if (write_with_imm)
		atomic64_inc(&tqp->owner->data_wr_op_write_imm);
	else
		atomic64_inc(&tqp->owner->data_wr_op_write);
	if (!tbv_qp_get_live(tqp))
		return -EINVAL;
	atomic64_inc(&tqp->owner->data_wr_live);

	ret = tbv_prepare_send_segments(tqp, wr, segs, &nsegs, &total_len);
	if (ret) {
		atomic64_inc(&tqp->owner->data_wr_copy_error);
		goto err_put_qp;
	}
	nfrags = total_len ? DIV_ROUND_UP(total_len,
					  TBV_NATIVE_DATA_MAX_PAYLOAD) : 1;
	if (is_write) {
		const struct ib_rdma_wr *rwr = rdma_wr(wr);
		u64 remote_end;

		if (check_add_overflow(rwr->remote_addr, (u64)total_len,
				       &remote_end)) {
			ret = -EINVAL;
			goto err_release_segs;
		}
	}

	mutex_lock(&tqp->owner->lock);
	path = tbv_first_active_native_path_locked(tqp->owner);
	mutex_unlock(&tqp->owner->lock);
	if (!path) {
		atomic64_inc(&tqp->owner->data_wr_no_path);
		ret = -ENOTCONN;
		goto err_release_segs;
	}
	path = NULL;

	if (is_send) {
		ret = tbv_qp_consume_remote_recv_credit(tqp);
		if (ret)
			goto err_release_segs;
		credit_consumed = true;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ret = -ENOMEM;
		goto err_return_credit;
	}
	ctx->tqp = tqp;
	refcount_set(&ctx->refs, 1);
	spin_lock_init(&ctx->lock);
	ctx->wr_id = wr->wr_id;
	ctx->signaled = !!(wr->send_flags & IB_SEND_SIGNALED);
	ctx->wc_opcode = is_write ? IB_WC_RDMA_WRITE : IB_WC_SEND;
	INIT_LIST_HEAD(&ctx->node);

	spin_lock_irqsave(&tqp->lock, flags);
	psn = tqp->send_psn & TBV_PSN_MASK;
	tqp->send_psn = tbv_psn_next(psn);
	spin_unlock_irqrestore(&tqp->lock, flags);
	ctx->psn = psn;

	tbv_qp_queue_send(tqp, ctx);
	fragment_striping = tqp->owner->native_fragment_striping;
	wr_striping = tqp->owner->native_wr_striping;

	if (tbv_should_zcopy_payload(total_len) && fragment_striping)
		atomic64_inc(&tqp->owner->data_wr_zcopy_fallback);

	if (!fragment_striping && tbv_should_zcopy_payload(total_len) &&
	    tbv_send_segments_zcopy_safe(segs, nsegs, total_len)) {
		struct tbv_send_page_stream *stream;

		atomic64_inc(&tqp->owner->data_wr_zcopy);
		memset(&hdr, 0, sizeof(hdr));
		if (send_with_imm)
			hdr.opcode = TBV_NATIVE_DATA_OP_SEND_IMM;
		else if (is_send)
			hdr.opcode = TBV_NATIVE_DATA_OP_SEND;
		else if (write_with_imm)
			hdr.opcode = TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM;
		else
			hdr.opcode = TBV_NATIVE_DATA_OP_RDMA_WRITE;
		if (wr->send_flags & IB_SEND_SOLICITED)
			hdr.flags = TBV_NATIVE_DATA_F_SOLICITED;
		hdr.dest_qp = tqp->attr.dest_qp_num;
		hdr.src_qp = tqp->base.qp_num;
		hdr.psn = psn;
		hdr.imm_data = write_with_imm ? be32_to_cpu(wr->ex.imm_data) :
						total_len;
		if (is_send) {
			hdr.remote_addr = 0;
			hdr.rkey = send_with_imm ?
				   be32_to_cpu(wr->ex.imm_data) : 0;
		} else {
			const struct ib_rdma_wr *rwr = rdma_wr(wr);

			hdr.remote_addr = rwr->remote_addr;
			hdr.rkey = rwr->rkey;
		}

		mutex_lock(&tqp->owner->lock);
		path = tbv_select_native_data_path_get_locked(
			tqp->owner, wr_striping ? psn : tqp->base.qp_num);
		mutex_unlock(&tqp->owner->lock);
		if (!path) {
			atomic64_inc(&tqp->owner->data_wr_no_path);
			ret = -ENOTCONN;
			goto err_unqueue_ctx;
		}

		stream = kzalloc(sizeof(*stream), GFP_KERNEL);
		if (!stream) {
			tbv_release_path_refs(&path, 1);
			ret = -ENOMEM;
			goto err_unqueue_ctx;
		}
		refcount_set(&stream->refs, 1);
		stream->send = ctx;
		stream->total_len = total_len;
		stream->nsegs = nsegs;
		memcpy(stream->segs, segs, sizeof(stream->segs));
		memset(segs, 0, sizeof(segs));
		nsegs = 0;

		tbv_send_ctx_get(ctx);
		tbv_send_ctx_get(ctx);
		atomic64_inc(&tqp->owner->data_wr_path_send);
		ret = tbv_path_send_page_stream(path, &hdr, total_len, 0,
						tbv_send_tx_done, ctx,
						tbv_send_page_stream_next,
						stream);
		tbv_release_path_refs(&path, 1);
		tbv_send_page_stream_put(stream);
		tbv_send_ctx_put(ctx);
		if (ret) {
			atomic64_inc(&tqp->owner->data_wr_path_send_error);
			if (credit_consumed)
				tbv_qp_return_remote_recv_credit(tqp);
			return ret;
		}

		atomic64_inc(&tqp->owner->data_tx_accepted);
		return 0;
	}
	if (!fragment_striping && tbv_should_zcopy_payload(total_len))
		atomic64_inc(&tqp->owner->data_wr_zcopy_fallback);

	atomic64_inc(&tqp->owner->data_wr_copied);
	mutex_lock(&tqp->owner->lock);
	if (fragment_striping) {
		u32 i;

		path_count = tbv_collect_native_data_paths_get_locked(
			tqp->owner, paths, ARRAY_SIZE(paths));
		if (!path_count) {
			ret = -ENOTCONN;
			goto out_unlock_paths;
		}
		for (i = 0; i < nfrags; i++)
			reservations[(psn + i) % path_count]++;
	} else {
		path = tbv_select_native_data_path_get_locked(
			tqp->owner, wr_striping ? psn : tqp->base.qp_num);
		if (!path) {
			ret = -ENOTCONN;
			goto out_unlock_paths;
		}
		paths[0] = path;
		path_count = 1;
		reservations[0] = nfrags;
	}

	for (frag_idx = 0; frag_idx < path_count; frag_idx++) {
		u32 count = reservations[frag_idx];

		reservations[frag_idx] = 0;
		if (!count)
			continue;
		ret = tbv_path_reserve_data(paths[frag_idx], count);
		if (ret)
			goto out_release_reservations;
		reservations[frag_idx] = count;
	}
	frag_idx = 0;

out_release_reservations:
	if (ret)
		tbv_release_path_reservations(paths, reservations, path_count);
out_unlock_paths:
	mutex_unlock(&tqp->owner->lock);
	if (ret) {
		tbv_release_path_refs(paths, path_count);
		atomic64_inc(&tqp->owner->data_wr_no_path);
		goto err_unqueue_ctx;
	}

	do {
		u32 payload_len = min_t(u32, total_len - offset,
					TBV_NATIVE_DATA_MAX_PAYLOAD);
		bool last = offset + payload_len == total_len;
		u32 path_idx = fragment_striping ?
			       (psn + frag_idx) % path_count : 0;
		u32 packet_len = TBV_NATIVE_DATA_HDR_SIZE + payload_len;
		u8 *frame;

		frame = kmalloc(packet_len, GFP_KERNEL);
		if (!frame) {
			ret = -ENOMEM;
			goto err_release_paths_unqueue_ctx;
		}

		ret = tbv_copy_send_range(segs, nsegs, offset,
					  frame + TBV_NATIVE_DATA_HDR_SIZE,
					  payload_len);
		if (ret) {
			kfree(frame);
			atomic64_inc(&tqp->owner->data_wr_copy_error);
			goto err_release_paths_unqueue_ctx;
		}

		memset(&hdr, 0, sizeof(hdr));
		if (send_with_imm)
			hdr.opcode = TBV_NATIVE_DATA_OP_SEND_IMM;
		else if (is_send)
			hdr.opcode = TBV_NATIVE_DATA_OP_SEND;
		else if (write_with_imm)
			hdr.opcode = TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM;
		else
			hdr.opcode = TBV_NATIVE_DATA_OP_RDMA_WRITE;
		hdr.flags = last ? TBV_NATIVE_DATA_F_LAST : 0;
		if (last && (wr->send_flags & IB_SEND_SOLICITED))
			hdr.flags |= TBV_NATIVE_DATA_F_SOLICITED;
		hdr.dest_qp = tqp->attr.dest_qp_num;
		hdr.src_qp = tqp->base.qp_num;
		hdr.psn = psn;
		hdr.length = payload_len;
		hdr.imm_data = write_with_imm ? be32_to_cpu(wr->ex.imm_data) :
						total_len;
		if (is_send) {
			hdr.remote_addr = offset;
			hdr.rkey = send_with_imm ?
				   be32_to_cpu(wr->ex.imm_data) : 0;
		} else {
			const struct ib_rdma_wr *rwr = rdma_wr(wr);

			hdr.remote_addr = rwr->remote_addr + offset;
			hdr.rkey = rwr->rkey;
		}
		ret = tbv_native_data_build_header(frame, packet_len,
						   &hdr);
		if (ret < 0) {
			kfree(frame);
			goto err_release_paths_unqueue_ctx;
		}

		atomic64_inc(&tqp->owner->data_wr_path_send);
		tbv_send_ctx_get(ctx);
		ret = tbv_path_send_owned(paths[path_idx], frame, packet_len,
					  TBV_PATH_SEND_DEFER,
					  tbv_send_tx_done, ctx);
		if (ret)
			tbv_send_ctx_put(ctx);
		if (ret) {
			atomic64_inc(&tqp->owner->data_wr_path_send_error);
			goto err_release_paths_unqueue_ctx;
		}

		reservations[path_idx]--;
		sent_any = true;
		offset += payload_len;
		frag_idx++;
	} while (offset < total_len);

	tbv_kick_paths(paths, path_count);
	tbv_release_path_refs(paths, path_count);
	atomic64_inc(&tqp->owner->data_tx_accepted);
	tbv_release_send_segments(segs, nsegs);
	return 0;

err_release_paths_unqueue_ctx:
	tbv_release_path_reservations(paths, reservations, path_count);
	if (sent_any)
		tbv_kick_paths(paths, path_count);
	tbv_release_path_refs(paths, path_count);
err_unqueue_ctx:
	tbv_qp_unqueue_send(tqp, ctx);
	if (credit_consumed && !sent_any)
		tbv_qp_return_remote_recv_credit(tqp);
	tbv_send_ctx_put(ctx);
	tbv_release_send_segments(segs, nsegs);
	return ret;
err_return_credit:
	if (credit_consumed)
		tbv_qp_return_remote_recv_credit(tqp);
err_release_segs:
	tbv_release_send_segments(segs, nsegs);
err_put_qp:
	atomic64_dec(&tqp->owner->data_wr_live);
	tbv_qp_put(tqp);
	return ret;
}

static int tbv_post_send(struct ib_qp *qp, const struct ib_send_wr *wr,
			 const struct ib_send_wr **bad_wr)
{
	struct tbv_qp *tqp = container_of(qp, struct tbv_qp, base);
	const struct ib_send_wr *cur;
	int ret;

	for (cur = wr; cur; cur = cur->next) {
		ret = tbv_post_send_one(tqp, cur);
		if (ret) {
			if (bad_wr)
				*bad_wr = cur;
			return ret;
		}
	}

	return 0;
}

static int tbv_validate_recv_sge(struct tbv_qp *tqp, const struct ib_sge *sge)
{
	struct tbv_mr *mr;
	u64 mr_end;
	u64 end;
	int ret = 0;

	if (!sge->length)
		return 0;
	if (check_add_overflow(sge->addr, (u64)sge->length, &end))
		return -EINVAL;

	mr = tbv_mr_get(tqp->owner, sge->lkey);
	if (!mr)
		return -EINVAL;
	if (!(mr->access & IB_ACCESS_LOCAL_WRITE)) {
		ret = -EACCES;
		goto err_put;
	}
	if (check_add_overflow(mr->start, mr->length, &mr_end)) {
		ret = -EINVAL;
		goto err_put;
	}
	if (sge->addr < mr->start || end > mr_end) {
		ret = -EFAULT;
		goto err_put;
	}

	tbv_mr_put(mr);
	return 0;

err_put:
	tbv_mr_put(mr);
	return ret;
}

static int tbv_post_recv(struct ib_qp *qp, const struct ib_recv_wr *wr,
			 const struct ib_recv_wr **bad_wr)
{
	struct tbv_qp *tqp = container_of(qp, struct tbv_qp, base);
	unsigned long flags;
	const struct ib_recv_wr *cur;
	u32 posted = 0;
	int ret;

	for (cur = wr; cur; cur = cur->next) {
		const struct ib_sge *sge = NULL;

		if (cur->num_sge > 1) {
			ret = -EINVAL;
			goto err_bad;
		}
		if (cur->num_sge == 1) {
			sge = cur->sg_list;
			if (!sge) {
				ret = -EINVAL;
				goto err_bad;
			}
			ret = tbv_validate_recv_sge(tqp, sge);
			if (ret)
				goto err_bad;
		}

		spin_lock_irqsave(&tqp->lock, flags);
		if (tqp->closing) {
			spin_unlock_irqrestore(&tqp->lock, flags);
			ret = -EINVAL;
			goto err_bad;
		}
		if (tqp->recv_count == tqp->recvq_size) {
			spin_unlock_irqrestore(&tqp->lock, flags);
			ret = -ENOMEM;
			goto err_bad;
		}

		tqp->recvq[tqp->recv_tail].wr_id = cur->wr_id;
		tqp->recvq[tqp->recv_tail].addr = sge ? sge->addr : 0;
		tqp->recvq[tqp->recv_tail].length = sge ? sge->length : 0;
		tqp->recvq[tqp->recv_tail].lkey = sge ? sge->lkey : 0;
		tqp->recv_tail = (tqp->recv_tail + 1) % tqp->recvq_size;
		tqp->recv_count++;
		posted++;
		atomic_inc(&tqp->owner->verbs_recv_wqes);
		spin_unlock_irqrestore(&tqp->lock, flags);
	}

	if (posted)
		tbv_qp_advertise_recv_credits(tqp);
	if (posted) {
		mutex_lock(&tqp->rx_lock);
		if (tbv_qp_uses_apple_transport(tqp))
			tbv_apple_rx_drain_pending_locked(tqp->owner, tqp);
		tbv_rx_drain_reorder_locked(tqp->owner, tqp);
		mutex_unlock(&tqp->rx_lock);
	}
	return 0;

err_bad:
	if (posted)
		tbv_qp_advertise_recv_credits(tqp);
	if (bad_wr)
		*bad_wr = cur;
	return ret;
}

static bool tbv_qp_pop_recv(struct tbv_qp *tqp, struct tbv_recv_wqe *wqe)
{
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	if (!tqp->recv_count || tqp->closing) {
		spin_unlock_irqrestore(&tqp->lock, flags);
		return false;
	}

	*wqe = tqp->recvq[tqp->recv_head];
	memset(&tqp->recvq[tqp->recv_head], 0,
	       sizeof(tqp->recvq[tqp->recv_head]));
	tqp->recv_head = (tqp->recv_head + 1) % tqp->recvq_size;
	tqp->recv_count--;
	if (tqp->recv_credits_advertised)
		tqp->recv_credits_advertised--;
	atomic_dec(&tqp->owner->verbs_recv_wqes);
	spin_unlock_irqrestore(&tqp->lock, flags);
	return true;
}

static int tbv_cq_push(struct tbv_cq *tcq, const struct ib_wc *wc)
{
	unsigned long flags;
	bool notify = false;
	int ret = 0;

	spin_lock_irqsave(&tcq->lock, flags);
	if (tcq->count == tcq->cqe) {
		if (tcq->owner)
			atomic64_inc(&tcq->owner->data_cq_overflow);
		ret = -ENOSPC;
		goto out;
	}

	tcq->entries[tcq->tail] = *wc;
	tcq->tail = (tcq->tail + 1) % tcq->cqe;
	tcq->count++;
	if (tcq->notify_armed) {
		tcq->notify_armed = false;
		notify = true;
	}
out:
	spin_unlock_irqrestore(&tcq->lock, flags);
	if (notify && tcq->base.comp_handler)
		tcq->base.comp_handler(&tcq->base, tcq->base.cq_context);
	return ret;
}

static void tbv_apple_pending_reset(struct tbv_apple_pending_rx *p)
{
	p->delivered = 0;
	p->status = IB_WC_SUCCESS;
	p->active = false;
	p->ready = false;
}

static void tbv_apple_pending_release(struct tbv_qp *tqp,
				      struct tbv_apple_pending_rx *p)
{
	if (p->delivered) {
		if (tqp->apple_pending_bytes >= p->delivered)
			tqp->apple_pending_bytes -= p->delivered;
		else
			tqp->apple_pending_bytes = 0;
	}
	tbv_apple_pending_reset(p);
}

static void tbv_qp_flush_apple_pending(struct tbv_qp *tqp)
{
	u32 i;

	if (!tqp->apple_pending) {
		tqp->apple_pending_head = 0;
		tqp->apple_pending_tail = 0;
		tqp->apple_pending_ready_count = 0;
		tqp->apple_pending_bytes = 0;
		tqp->apple_pending_active = -1;
		return;
	}

	for (i = 0; i < tqp->apple_pending_slot_count; i++) {
		struct tbv_apple_pending_rx *p = &tqp->apple_pending[i];

		if ((p->active || p->ready) && tqp->owner)
			atomic64_inc(&tqp->owner->data_rx_pending_discarded);
		tbv_apple_pending_release(tqp, p);
	}
	tqp->apple_pending_head = 0;
	tqp->apple_pending_tail = 0;
	tqp->apple_pending_ready_count = 0;
	tqp->apple_pending_bytes = 0;
	tqp->apple_pending_active = -1;
}

static struct tbv_apple_pending_rx *
tbv_apple_pending_active_locked(struct tbv_state *state, struct tbv_qp *tqp)
{
	struct tbv_apple_pending_rx *p;

	if (tqp->apple_pending_active >= 0)
		return &tqp->apple_pending[tqp->apple_pending_active];
	if (!tqp->apple_pending || !tqp->apple_pending_slot_count)
		return NULL;
	if (tqp->apple_pending_ready_count >= tqp->apple_pending_slot_count)
		return NULL;
	if (!READ_ONCE(apple_rx_pending_bytes))
		return NULL;
	if (!READ_ONCE(apple_rx_pending_total_bytes))
		return NULL;

	p = &tqp->apple_pending[tqp->apple_pending_tail];
	tbv_apple_pending_reset(p);
	p->active = true;
	tqp->apple_pending_active = tqp->apple_pending_tail;
	atomic64_inc(&state->data_rx_reorder_buffered);
	return p;
}

static void tbv_apple_pending_finish_locked(struct tbv_qp *tqp)
{
	struct tbv_apple_pending_rx *p;

	if (tqp->apple_pending_active < 0)
		return;

	p = &tqp->apple_pending[tqp->apple_pending_active];
	p->active = false;
	p->ready = true;
	tqp->apple_pending_active = -1;
	tqp->apple_pending_ready_count++;
	tqp->apple_pending_tail = (tqp->apple_pending_tail + 1) %
				  tqp->apple_pending_slot_count;
}

static void tbv_apple_rx_push_wc(struct tbv_qp *tqp,
				 const struct tbv_recv_wqe *wqe, u32 byte_len,
				 int status)
{
	struct tbv_cq *recv_cq;
	struct ib_wc wc = {};

	recv_cq = container_of(tqp->base.recv_cq, struct tbv_cq, base);
	wc.wr_id = wqe->wr_id;
	wc.status = status;
	wc.opcode = IB_WC_RECV;
	wc.qp = &tqp->base;
	wc.byte_len = byte_len;
	wc.src_qp = tqp->attr.dest_qp_num;
	wc.pkey_index = 0;
	wc.port_num = 1;
	tbv_cq_push(recv_cq, &wc);
}

static void tbv_apple_rx_drain_pending_locked(struct tbv_state *state,
					      struct tbv_qp *tqp)
{
	while (tqp->apple_pending_ready_count) {
		struct tbv_apple_pending_rx *p;
		struct tbv_recv_wqe wqe;
		u32 copy_len;
		u32 delivered = 0;
		int status;
		int ret;

		if (!tbv_qp_pop_recv(tqp, &wqe))
			return;

		p = &tqp->apple_pending[tqp->apple_pending_head];
		copy_len = min_t(u32, p->delivered, wqe.length);
		status = p->status;
		ret = tbv_rx_copy_to_wqe(state, &wqe, 0, p->buf, copy_len,
					 &delivered);
		if (ret)
			status = IB_WC_LOC_PROT_ERR;
		else if (p->delivered > wqe.length)
			status = IB_WC_LOC_LEN_ERR;

		tbv_apple_rx_push_wc(tqp, &wqe, delivered, status);
		atomic64_inc(&state->data_rx_send);
		atomic64_inc(&state->data_rx_op_send);
		atomic64_inc(&state->data_rx_reorder_delivered);

		tbv_apple_pending_release(tqp, p);
		tqp->apple_pending_head = (tqp->apple_pending_head + 1) %
					  tqp->apple_pending_slot_count;
		tqp->apple_pending_ready_count--;
	}
}

static int tbv_apple_rx_copy_piece(struct tbv_state *state,
				   const struct tbv_recv_wqe *wqe,
				   u32 dst_off, const void *src, u32 len,
				   u32 *delivered, u32 *user_len)
{
	int ret;

	if (!len)
		return 0;

	ret = tbv_rx_copy_to_wqe(state, wqe, dst_off + *user_len, src, len,
				 delivered);
	if (ret)
		return ret;
	*user_len += len;
	return 0;
}

static int tbv_apple_rx_copy_piece_to_buf(struct tbv_qp *tqp,
					  struct tbv_apple_pending_rx *p,
					  const void *src, u32 len,
					  u32 *user_len)
{
	u32 max_bytes;
	u32 required;
	u32 total_limit;
	u32 total_required;

	if (!len)
		return 0;
	if (check_add_overflow(p->delivered, len, &required))
		return -EMSGSIZE;

	max_bytes = min_t(u32, READ_ONCE(apple_rx_pending_bytes),
			  TBV_APPLE_MAX_MSG_SIZE);
	if (!max_bytes || required > max_bytes)
		return -EMSGSIZE;

	total_limit = READ_ONCE(apple_rx_pending_total_bytes);
	if (!total_limit ||
	    check_add_overflow(tqp->apple_pending_bytes, len, &total_required) ||
	    total_required > total_limit)
		return -ENOSPC;

	if (required > p->capacity) {
		u32 new_capacity = p->capacity ? p->capacity : PAGE_SIZE;
		void *buf;

		while (new_capacity < required) {
			u32 doubled;

			if (check_mul_overflow(new_capacity, 2u, &doubled) ||
			    doubled > max_bytes) {
				new_capacity = max_bytes;
				break;
			}
			new_capacity = doubled;
		}
		if (new_capacity < required)
			return -EMSGSIZE;

		buf = kvzalloc(new_capacity, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
		if (p->buf && p->delivered)
			memcpy(buf, p->buf, p->delivered);
		kvfree(p->buf);
		p->buf = buf;
		p->capacity = new_capacity;
	}
	memcpy((u8 *)p->buf + p->delivered, src, len);
	p->delivered += len;
	tqp->apple_pending_bytes += len;
	*user_len += len;
	return 0;
}

static int tbv_apple_rx_copy_frame(struct tbv_state *state,
				   const struct tbv_recv_wqe *wqe,
				   u32 dst_off, const void *payload, u32 len,
				   u32 *delivered, u32 *out_user_len)
{
	u32 num_slots = len / TBV_APPLE_SLOT_WIRE_SIZE;
	u32 tail = len - num_slots * TBV_APPLE_SLOT_WIRE_SIZE;
	u32 normal_slots = num_slots;
	u32 user_len = 0;
	const u8 *p = payload;
	u32 i;
	int ret;

	if (!tail && num_slots)
		normal_slots--;

	for (i = 0; i < normal_slots; i++) {
		ret = tbv_apple_rx_copy_piece(state, wqe, dst_off,
					      p + i * TBV_APPLE_SLOT_WIRE_SIZE,
					      TBV_APPLE_FRAME_SLOT_USER_SIZE,
					      delivered, &user_len);
		if (ret)
			return ret;
	}

	if (!tail && num_slots) {
		const u8 *slot = p + normal_slots * TBV_APPLE_SLOT_WIRE_SIZE;

		ret = tbv_apple_rx_copy_piece(state, wqe, dst_off, slot,
					      TBV_APPLE_FRAME_SPLIT_USER_SIZE,
					      delivered, &user_len);
		if (ret)
			return ret;
		ret = tbv_apple_rx_copy_piece(state, wqe, dst_off,
					      slot + TBV_APPLE_FRAME_SPLIT_USER_SIZE + 4,
					      TBV_APPLE_TAIL_USER_SIZE,
					      delivered, &user_len);
		if (ret)
			return ret;
	} else if (tail > TBV_APPLE_TAIL_USER_SIZE) {
		const u8 *frag = p + normal_slots * TBV_APPLE_SLOT_WIRE_SIZE;
		u32 prefix = tail - TBV_APPLE_TAIL_USER_SIZE;

		/* Short terminal descriptors carry user bytes followed by the
		 * four silicon trailer bytes. Longer terminal descriptors are
		 * split as [prefix user][trailer][240 tail user].
		 */
		if (prefix <= 4) {
			ret = tbv_apple_rx_copy_piece(state, wqe, dst_off,
						      frag, tail - 4,
						      delivered, &user_len);
			if (ret)
				return ret;
			goto out;
		}

		prefix -= 4;
		if (prefix > TBV_APPLE_FRAME_SPLIT_USER_SIZE)
			return -EINVAL;

		ret = tbv_apple_rx_copy_piece(state, wqe, dst_off, frag,
					      prefix, delivered, &user_len);
		if (ret)
			return ret;
		ret = tbv_apple_rx_copy_piece(state, wqe, dst_off,
					      frag + prefix + 4,
					      TBV_APPLE_TAIL_USER_SIZE,
					      delivered, &user_len);
		if (ret)
			return ret;
	} else if (tail) {
		ret = tbv_apple_rx_copy_piece(state, wqe, dst_off,
					      p + normal_slots * TBV_APPLE_SLOT_WIRE_SIZE,
					      tail, delivered, &user_len);
		if (ret)
			return ret;
	}

out:
	*out_user_len = user_len;
	return 0;
}

static int tbv_apple_rx_copy_frame_to_buf(struct tbv_qp *tqp,
					  struct tbv_apple_pending_rx *p,
					  const void *payload, u32 len,
					  u32 *out_user_len)
{
	u32 num_slots = len / TBV_APPLE_SLOT_WIRE_SIZE;
	u32 tail = len - num_slots * TBV_APPLE_SLOT_WIRE_SIZE;
	u32 normal_slots = num_slots;
	u32 user_len = 0;
	const u8 *data = payload;
	u32 i;
	int ret;

	if (!tail && num_slots)
		normal_slots--;

	for (i = 0; i < normal_slots; i++) {
		ret = tbv_apple_rx_copy_piece_to_buf(tqp, p,
						     data + i * TBV_APPLE_SLOT_WIRE_SIZE,
						     TBV_APPLE_FRAME_SLOT_USER_SIZE,
						     &user_len);
		if (ret)
			return ret;
	}

	if (!tail && num_slots) {
		const u8 *slot = data + normal_slots * TBV_APPLE_SLOT_WIRE_SIZE;

		ret = tbv_apple_rx_copy_piece_to_buf(tqp, p, slot,
						     TBV_APPLE_FRAME_SPLIT_USER_SIZE,
						     &user_len);
		if (ret)
			return ret;
		ret = tbv_apple_rx_copy_piece_to_buf(tqp, p,
						     slot + TBV_APPLE_FRAME_SPLIT_USER_SIZE + 4,
						     TBV_APPLE_TAIL_USER_SIZE,
						     &user_len);
		if (ret)
			return ret;
	} else if (tail > TBV_APPLE_TAIL_USER_SIZE) {
		const u8 *frag = data + normal_slots * TBV_APPLE_SLOT_WIRE_SIZE;
		u32 prefix = tail - TBV_APPLE_TAIL_USER_SIZE;

		/* See tbv_apple_rx_copy_frame(). */
		if (prefix <= 4) {
			ret = tbv_apple_rx_copy_piece_to_buf(tqp, p, frag,
							     tail - 4,
							     &user_len);
			if (ret)
				return ret;
			goto out;
		}

		prefix -= 4;
		if (prefix > TBV_APPLE_FRAME_SPLIT_USER_SIZE)
			return -EINVAL;

		ret = tbv_apple_rx_copy_piece_to_buf(tqp, p, frag, prefix,
						     &user_len);
		if (ret)
			return ret;
		ret = tbv_apple_rx_copy_piece_to_buf(tqp, p, frag + prefix + 4,
						     TBV_APPLE_TAIL_USER_SIZE,
						     &user_len);
		if (ret)
			return ret;
	} else if (tail) {
		ret = tbv_apple_rx_copy_piece_to_buf(tqp, p,
						     data + normal_slots * TBV_APPLE_SLOT_WIRE_SIZE,
						     tail, &user_len);
		if (ret)
			return ret;
	}

out:
	*out_user_len = user_len;
	return 0;
}

void tbv_ibdev_rx_apple_frame(struct tbv_state *state,
			      const struct tbv_path *path,
			      const void *payload, u32 len, u8 eof)
{
	struct tbv_rx_message *msg;
	struct tbv_qp *tqp;
	u32 qpn;
	u32 user_len = 0;
	int ret;

	if (!state || !state->verbs_registered)
		return;
	if (!payload || !len || len > TBV_APPLE_FRAME_SIZE) {
		atomic64_inc(&state->data_rx_bad_frame);
		return;
	}

	qpn = tbv_apple_qpn_from_path(path);
	tqp = tbv_qp_get_by_num(state, qpn);
	if (!tqp) {
		atomic64_inc(&state->data_rx_no_qp);
		return;
	}

	mutex_lock(&tqp->rx_lock);
	msg = &tqp->rx_msg;
	if (!msg->active) {
		if (!tbv_qp_pop_recv(tqp, &msg->wqe)) {
			struct tbv_apple_pending_rx *pending;

			pending = tbv_apple_pending_active_locked(state, tqp);
			if (!pending) {
				atomic64_inc(&state->data_rx_no_recv);
				atomic64_inc(&state->data_rx_reorder_dropped);
				mutex_unlock(&tqp->rx_lock);
				tbv_qp_put(tqp);
				return;
			}
			ret = tbv_apple_rx_copy_frame_to_buf(tqp, pending,
							     payload, len,
							     &user_len);
			if (ret) {
				atomic64_inc(&state->data_rx_bad_frame);
				pending->status = (ret == -EMSGSIZE ||
						   ret == -ENOSPC) ?
					IB_WC_LOC_LEN_ERR : IB_WC_LOC_PROT_ERR;
			}
			if (eof == 3) {
				tbv_apple_pending_finish_locked(tqp);
				tbv_apple_rx_drain_pending_locked(state, tqp);
			}
			mutex_unlock(&tqp->rx_lock);
			tbv_qp_put(tqp);
			return;
		}
		msg->active = true;
		msg->src_qp = tqp->attr.dest_qp_num;
		msg->status = IB_WC_SUCCESS;
	}

	ret = tbv_apple_rx_copy_frame(state, &msg->wqe, msg->received,
				      payload, len, &msg->delivered,
				      &user_len);
	if (ret) {
		atomic64_inc(&state->data_rx_bad_frame);
		msg->status = IB_WC_LOC_PROT_ERR;
	}

	msg->received += user_len;
	if (msg->received > msg->wqe.length)
		msg->status = IB_WC_LOC_LEN_ERR;

	if (eof == 3) {
		tbv_apple_rx_push_wc(tqp, &msg->wqe, msg->delivered,
				     msg->status);
		memset(msg, 0, sizeof(*msg));
		atomic64_inc(&state->data_rx_send);
		atomic64_inc(&state->data_rx_op_send);
	}
	mutex_unlock(&tqp->rx_lock);
	tbv_qp_put(tqp);
}

static int tbv_send_control_frame(struct tbv_state *state, const void *frame,
				  u32 len)
{
	int ret = -ENOTCONN;
	struct tbv_path *path;

	mutex_lock(&state->lock);
	path = tbv_first_active_native_path_locked(state);
	if (path)
		ret = tbv_path_send(path, frame, len, TBV_PATH_SEND_CONTROL,
				    NULL, NULL);
	mutex_unlock(&state->lock);

	return ret;
}

static void tbv_send_ack(struct tbv_state *state, u32 dest_qp, u32 src_qp,
			 u32 psn, int status)
{
	struct tbv_native_data_header hdr = {};
	u8 frame[TBV_NATIVE_DATA_HDR_SIZE];
	int len;

	hdr.opcode = TBV_NATIVE_DATA_OP_SEND_ACK;
	hdr.dest_qp = dest_qp;
	hdr.src_qp = src_qp;
	hdr.psn = psn;
	hdr.imm_data = status ? 1 : 0;

	len = tbv_native_data_build_header(frame, sizeof(frame), &hdr);
	if (len < 0)
		return;

	tbv_send_control_frame(state, frame, len);
}

static void tbv_send_read_status(struct tbv_state *state, u32 dest_qp,
				 u32 src_qp, u32 psn, u32 total_len,
				 int status)
{
	struct tbv_native_data_header hdr = {};
	u8 frame[TBV_NATIVE_DATA_HDR_SIZE];
	int len;

	hdr.opcode = TBV_NATIVE_DATA_OP_RDMA_READ_RESP;
	hdr.flags = TBV_NATIVE_DATA_F_LAST;
	hdr.dest_qp = dest_qp;
	hdr.src_qp = src_qp;
	hdr.psn = psn;
	hdr.imm_data = total_len;
	hdr.rkey = status ? 1 : 0;

	len = tbv_native_data_build_header(frame, sizeof(frame), &hdr);
	if (len < 0)
		return;

	tbv_send_control_frame(state, frame, len);
}

static int tbv_send_recv_credit(struct tbv_state *state, u32 dest_qp,
				u32 src_qp, u32 credits)
{
	struct tbv_native_data_header hdr = {};
	u8 frame[TBV_NATIVE_DATA_HDR_SIZE];
	int len;

	if (!credits || !dest_qp)
		return 0;

	hdr.opcode = TBV_NATIVE_DATA_OP_RECV_CREDIT;
	hdr.dest_qp = dest_qp;
	hdr.src_qp = src_qp;
	hdr.imm_data = credits;

	len = tbv_native_data_build_header(frame, sizeof(frame), &hdr);
	if (len < 0)
		return len;

	return tbv_send_control_frame(state, frame, len);
}

static void tbv_qp_advertise_recv_credits(struct tbv_qp *tqp)
{
	unsigned long flags;
	u32 credits;
	u32 dest_qp;
	int ret;

	if (tbv_qp_uses_apple_transport(tqp))
		return;

	spin_lock_irqsave(&tqp->lock, flags);
	if (tqp->closing || !tqp->attr.dest_qp_num ||
	    tqp->recv_count <= tqp->recv_credits_advertised) {
		spin_unlock_irqrestore(&tqp->lock, flags);
		return;
	}
	credits = tqp->recv_count - tqp->recv_credits_advertised;
	tqp->recv_credits_advertised += credits;
	dest_qp = tqp->attr.dest_qp_num;
	spin_unlock_irqrestore(&tqp->lock, flags);

	ret = tbv_send_recv_credit(tqp->owner, dest_qp, tqp->base.qp_num,
				   credits);
	if (!ret)
		return;

	spin_lock_irqsave(&tqp->lock, flags);
	if (tqp->recv_credits_advertised >= credits)
		tqp->recv_credits_advertised -= credits;
	else
		tqp->recv_credits_advertised = 0;
	spin_unlock_irqrestore(&tqp->lock, flags);
}

static bool tbv_qp_try_consume_remote_recv_credit(struct tbv_qp *tqp,
						  int *ret)
{
	unsigned long flags;
	bool done = false;

	spin_lock_irqsave(&tqp->lock, flags);
	if (tqp->closing) {
		*ret = -ECANCELED;
		done = true;
	} else if (tqp->remote_recv_credits) {
		tqp->remote_recv_credits--;
		*ret = 0;
		done = true;
	}
	spin_unlock_irqrestore(&tqp->lock, flags);

	return done;
}

static int tbv_qp_consume_remote_recv_credit(struct tbv_qp *tqp)
{
	int ret = -ETIMEDOUT;
	long waited;

	waited = wait_event_timeout(tqp->credit_wait,
				    tbv_qp_try_consume_remote_recv_credit(tqp,
									  &ret),
				    msecs_to_jiffies(5000));
	return waited ? ret : -ETIMEDOUT;
}

static void tbv_qp_return_remote_recv_credit(struct tbv_qp *tqp)
{
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	if (!tqp->closing)
		tqp->remote_recv_credits++;
	spin_unlock_irqrestore(&tqp->lock, flags);
	wake_up_all(&tqp->credit_wait);
}

static u32 tbv_apple_tx_frame_charge(u32 frames, unsigned int max_frames)
{
	if (!max_frames)
		return 0;
	return min_t(u32, frames, max_frames);
}

static bool tbv_qp_try_acquire_apple_tx_window(struct tbv_qp *tqp, u32 frames,
					       bool *wr_acquired,
					       u32 *frames_acquired,
					       int *ret)
{
	unsigned int max_wr = READ_ONCE(apple_tx_max_inflight_wr);
	unsigned int max_frames = READ_ONCE(apple_tx_max_inflight_frames);
	u32 frame_charge = tbv_apple_tx_frame_charge(frames, max_frames);
	unsigned long flags;
	bool acquired = false;

	if (!max_wr && !max_frames) {
		*wr_acquired = false;
		*frames_acquired = 0;
		*ret = 0;
		return true;
	}

	spin_lock_irqsave(&tqp->lock, flags);
	if (tqp->closing) {
		*ret = -ECANCELED;
		acquired = true;
	} else {
		int cur_wr = atomic_read(&tqp->apple_tx_inflight);
		int cur_frames = atomic_read(&tqp->apple_tx_inflight_frames);
		bool wr_ok = !max_wr || cur_wr < max_wr;
		bool frames_ok = !max_frames ||
			cur_frames + frame_charge <= max_frames;

		if (wr_ok && frames_ok) {
			if (max_wr)
				atomic_inc(&tqp->apple_tx_inflight);
			if (frame_charge)
				atomic_add(frame_charge,
					   &tqp->apple_tx_inflight_frames);
			*wr_acquired = !!max_wr;
			*frames_acquired = frame_charge;
			*ret = 0;
			acquired = true;
		}
	}
	spin_unlock_irqrestore(&tqp->lock, flags);
	return acquired;
}

static int tbv_qp_acquire_apple_tx_window(struct tbv_qp *tqp, u32 frames,
					  bool *wr_acquired,
					  u32 *frames_acquired)
{
	int ret = -ETIMEDOUT;
	long waited;

	*wr_acquired = false;
	*frames_acquired = 0;
	waited = wait_event_timeout(tqp->apple_tx_wait,
				    tbv_qp_try_acquire_apple_tx_window(tqp,
								       frames,
								       wr_acquired,
								       frames_acquired,
								       &ret),
				    msecs_to_jiffies(5000));
	return waited ? ret : -ETIMEDOUT;
}

static int tbv_umem_copy_to(struct tbv_mr *mr, u64 addr, const void *src,
			    size_t len)
{
	struct sg_table *sgt = &mr->umem->sgt_append.sgt;
	struct scatterlist *sg;
	size_t offset;
	size_t copied = 0;
	u64 mr_end;
	u64 end;
	unsigned int i;

	if (!len)
		return 0;
	if (check_add_overflow(addr, (u64)len, &end))
		return -EINVAL;
	if (check_add_overflow(mr->start, mr->length, &mr_end))
		return -EINVAL;
	if (addr < mr->start || end > mr_end)
		return -EFAULT;

	offset = ib_umem_offset(mr->umem) + addr - mr->start;
	for_each_sgtable_sg(sgt, sg, i) {
		size_t seg_len = sg->length;
		size_t seg_off;

		if (offset >= seg_len) {
			offset -= seg_len;
			continue;
		}

		seg_off = sg->offset + offset;
		seg_len -= offset;
		offset = 0;

		while (seg_len && copied < len) {
			struct page *page;
			size_t page_off = offset_in_page(seg_off);
			size_t chunk = min_t(size_t, PAGE_SIZE - page_off,
					     seg_len);
			void *kaddr;

			chunk = min_t(size_t, chunk, len - copied);

			page = pfn_to_page(page_to_pfn(sg_page(sg)) +
					   (seg_off >> PAGE_SHIFT));
			kaddr = kmap_local_page(page);
			memcpy((u8 *)kaddr + page_off, (const u8 *)src + copied,
			       chunk);
			flush_dcache_page(page);
			kunmap_local(kaddr);

			copied += chunk;
			seg_off += chunk;
			seg_len -= chunk;
		}

		if (copied == len)
			return 0;
	}

	return -EFAULT;
}

static int tbv_umem_copy_to_iova(struct tbv_mr *mr, u64 iova,
				 const void *src, size_t len)
{
	u64 iova_end;
	u64 mr_iova_end;
	u64 addr;

	if (!len)
		return 0;
	if (check_add_overflow(iova, (u64)len, &iova_end))
		return -EINVAL;
	if (check_add_overflow(mr->virt_addr, mr->length, &mr_iova_end))
		return -EINVAL;
	if (iova < mr->virt_addr || iova_end > mr_iova_end)
		return -EFAULT;
	if (check_add_overflow(mr->start, iova - mr->virt_addr, &addr))
		return -EINVAL;

	return tbv_umem_copy_to(mr, addr, src, len);
}

static int tbv_umem_iova_to_addr(struct tbv_mr *mr, u64 iova, size_t len,
				 u64 *addr_out)
{
	u64 iova_end;
	u64 mr_iova_end;
	u64 addr;

	if (check_add_overflow(iova, (u64)len, &iova_end))
		return -EINVAL;
	if (check_add_overflow(mr->virt_addr, mr->length, &mr_iova_end))
		return -EINVAL;
	if (iova < mr->virt_addr || iova_end > mr_iova_end)
		return -EFAULT;
	if (check_add_overflow(mr->start, iova - mr->virt_addr, &addr))
		return -EINVAL;

	*addr_out = addr;
	return 0;
}

static int tbv_umem_copy_from_iova(struct tbv_mr *mr, u64 iova,
				   void *dst, size_t len)
{
	u64 addr;
	int ret;

	if (!len)
		return 0;
	ret = tbv_umem_iova_to_addr(mr, iova, len, &addr);
	if (ret)
		return ret;

	return ib_umem_copy_from(dst, mr->umem, addr - mr->start, len);
}

static int tbv_umem_page_from_addr(struct tbv_mr *mr, u64 addr, u32 max_len,
				   struct page **page_out,
				   u32 *page_off_out, u32 *len_out)
{
	struct sg_table *sgt = &mr->umem->sgt_append.sgt;
	struct scatterlist *sg;
	size_t offset;
	u64 end;
	u64 mr_end;
	unsigned int i;

	if (!max_len)
		return -EINVAL;
	if (check_add_overflow(addr, (u64)max_len, &end))
		return -EINVAL;
	if (check_add_overflow(mr->start, mr->length, &mr_end))
		return -EINVAL;
	if (addr < mr->start || end > mr_end)
		return -EFAULT;

	offset = ib_umem_offset(mr->umem) + addr - mr->start;
	for_each_sgtable_sg(sgt, sg, i) {
		size_t seg_len = sg->length;
		size_t seg_off;
		size_t page_off;
		size_t chunk;

		if (offset >= seg_len) {
			offset -= seg_len;
			continue;
		}

		seg_off = sg->offset + offset;
		page_off = offset_in_page(seg_off);
		chunk = min_t(size_t, PAGE_SIZE - page_off, seg_len - offset);
		chunk = min_t(size_t, chunk, max_len);
		chunk = min_t(size_t, chunk, TBV_NATIVE_DATA_FRAME_SIZE);
		if (!chunk)
			return -EFAULT;

		*page_out = pfn_to_page(page_to_pfn(sg_page(sg)) +
					(seg_off >> PAGE_SHIFT));
		*page_off_out = page_off;
		*len_out = chunk;
		return 0;
	}

	return -EFAULT;
}

static int tbv_umem_page_from_iova(struct tbv_mr *mr, u64 iova, u32 max_len,
				   struct page **page_out,
				   u32 *page_off_out, u32 *len_out)
{
	u64 addr;
	int ret;

	ret = tbv_umem_iova_to_addr(mr, iova, max_len, &addr);
	if (ret)
		return ret;

	return tbv_umem_page_from_addr(mr, addr, max_len, page_out,
				       page_off_out, len_out);
}

static int tbv_copy_to_read_segments(struct tbv_read_ctx *read, u32 offset,
				     const void *payload, u32 len)
{
	u32 skipped = 0;
	u32 copied = 0;
	int i;

	if (!len)
		return 0;

	for (i = 0; i < read->nsegs; i++) {
		struct tbv_read_segment *seg = &read->segs[i];
		u32 seg_off = 0;
		u32 chunk;
		int ret;

		if (offset >= skipped + seg->length) {
			skipped += seg->length;
			continue;
		}
		if (offset > skipped)
			seg_off = offset - skipped;

		chunk = min_t(u32, seg->length - seg_off, len - copied);
		ret = tbv_umem_copy_to(seg->mr, seg->addr + seg_off,
				       (const u8 *)payload + copied, chunk);
		if (ret)
			return ret;

		copied += chunk;
		if (copied == len)
			return 0;
		skipped += seg->length;
	}

	return -EFAULT;
}

static int tbv_rx_copy_to_wqe(struct tbv_state *state,
			      const struct tbv_recv_wqe *wqe, u32 offset,
			      const void *payload, u32 len, u32 *delivered)
{
	struct tbv_mr *mr;
	u32 copy_len = 0;
	int ret;

	if (offset < wqe->length)
		copy_len = min_t(u32, len, wqe->length - offset);
	if (!copy_len)
		return 0;

	mr = tbv_mr_get(state, wqe->lkey);
	if (!mr) {
		atomic64_inc(&state->data_rx_copy_error);
		return -EINVAL;
	}

	ret = tbv_umem_copy_to(mr, wqe->addr + offset, payload, copy_len);
	tbv_mr_put(mr);
	if (ret) {
		atomic64_inc(&state->data_rx_copy_error);
		return ret;
	}

	*delivered += copy_len;
	return 0;
}

static void tbv_rx_reorder_free_msg(struct tbv_rx_reorder_msg *msg)
{
	if (!msg)
		return;
	kvfree(msg->buf);
	kfree(msg);
}

static void tbv_qp_flush_reorder(struct tbv_qp *tqp)
{
	struct tbv_rx_reorder_msg *msg;
	struct tbv_rx_reorder_msg *tmp;

	list_for_each_entry_safe(msg, tmp, &tqp->rx_reorder, node) {
		list_del(&msg->node);
		tbv_rx_reorder_free_msg(msg);
	}
	tqp->rx_reorder_count = 0;
	tqp->rx_reorder_bytes = 0;
}

static struct tbv_rx_reorder_msg *
tbv_rx_reorder_find(struct tbv_qp *tqp, u32 psn)
{
	struct tbv_rx_reorder_msg *msg;

	list_for_each_entry(msg, &tqp->rx_reorder, node) {
		if (msg->psn == psn)
			return msg;
	}

	return NULL;
}

static bool tbv_rx_fragment_shape(u32 total_len, u32 offset, u32 len,
				  bool last, u32 *frag_idx, u32 *frag_count)
{
	u32 idx;
	u32 count;
	u32 expected_len;

	if (!total_len) {
		if (offset || len || !last)
			return false;
		*frag_idx = 0;
		*frag_count = 1;
		return true;
	}

	if (offset % TBV_NATIVE_DATA_MAX_PAYLOAD)
		return false;
	idx = offset / TBV_NATIVE_DATA_MAX_PAYLOAD;
	count = DIV_ROUND_UP(total_len, TBV_NATIVE_DATA_MAX_PAYLOAD);
	if (idx >= count || count > TBV_RX_REORDER_MAX_FRAGS)
		return false;

	expected_len = idx == count - 1 ?
			      total_len - idx * TBV_NATIVE_DATA_MAX_PAYLOAD :
			      TBV_NATIVE_DATA_MAX_PAYLOAD;
	if (len != expected_len)
		return false;
	if (last != (idx == count - 1))
		return false;

	*frag_idx = idx;
	*frag_count = count;
	return true;
}

static void tbv_rx_finish_send(struct tbv_state *state, struct tbv_qp *tqp)
{
	struct tbv_rx_message *msg = &tqp->rx_msg;
	struct tbv_cq *recv_cq = container_of(tqp->base.recv_cq,
					      struct tbv_cq, base);
	struct ib_wc wc = {};
	u32 src_qp = msg->src_qp;
	u32 psn = msg->psn;
	int ack_status = msg->status == IB_WC_SUCCESS ? 0 : 1;

	wc.wr_id = msg->wqe.wr_id;
	wc.status = msg->status;
	wc.opcode = IB_WC_RECV;
	wc.qp = &tqp->base;
	wc.byte_len = msg->delivered;
	wc.src_qp = msg->src_qp;
	wc.pkey_index = 0;
	wc.port_num = 1;
	if (msg->with_imm) {
		wc.wc_flags = IB_WC_WITH_IMM;
		wc.ex.imm_data = cpu_to_be32(msg->imm_data);
	}
	if (tbv_cq_push(recv_cq, &wc))
		ack_status = 1;

	memset(msg, 0, sizeof(*msg));
	tqp->rx_expected_psn = tbv_psn_next(psn);
	tbv_send_ack(state, src_qp, tqp->base.qp_num, psn, ack_status);
}

static void tbv_rx_fail_active_send(struct tbv_state *state, struct tbv_qp *tqp,
				    enum ib_wc_status status)
{
	if (!tqp->rx_msg.active)
		return;
	tqp->rx_msg.status = status;
	tbv_rx_finish_send(state, tqp);
}

static bool tbv_rx_deliver_reorder_msg_locked(struct tbv_state *state,
					      struct tbv_qp *tqp,
					      struct tbv_rx_reorder_msg *msg)
{
	struct tbv_cq *recv_cq = container_of(tqp->base.recv_cq,
					      struct tbv_cq, base);
	struct tbv_recv_wqe wqe;
	struct ib_wc wc = {};
	u32 delivered = 0;
	int ack_status = 0;
	int status;

	if (!tbv_qp_pop_recv(tqp, &wqe)) {
		atomic64_inc(&state->data_rx_no_recv);
		return false;
	}

	list_del(&msg->node);
	tqp->rx_reorder_count--;
	tqp->rx_reorder_bytes -= msg->total_len;

	status = msg->total_len > wqe.length ? IB_WC_LOC_LEN_ERR :
					       IB_WC_SUCCESS;
	if (msg->total_len && tbv_rx_copy_to_wqe(state, &wqe, 0, msg->buf,
						 msg->total_len, &delivered))
		status = IB_WC_LOC_PROT_ERR;

	wc.wr_id = wqe.wr_id;
	wc.status = status;
	wc.opcode = IB_WC_RECV;
	wc.qp = &tqp->base;
	wc.byte_len = delivered;
	wc.src_qp = msg->src_qp;
	wc.pkey_index = 0;
	wc.port_num = 1;
	if (msg->with_imm) {
		wc.wc_flags = IB_WC_WITH_IMM;
		wc.ex.imm_data = cpu_to_be32(msg->imm_data);
	}
	if (tbv_cq_push(recv_cq, &wc))
		ack_status = 1;
	if (status != IB_WC_SUCCESS)
		ack_status = 1;

	tqp->rx_expected_psn = tbv_psn_next(msg->psn);
	tbv_send_ack(state, msg->src_qp, tqp->base.qp_num, msg->psn,
		     ack_status);
	atomic64_inc(&state->data_rx_reorder_delivered);
	tbv_rx_reorder_free_msg(msg);
	return true;
}

static void tbv_rx_drain_reorder_locked(struct tbv_state *state,
					struct tbv_qp *tqp)
{
	struct tbv_rx_reorder_msg *msg;

	while (!tqp->rx_msg.active) {
		msg = tbv_rx_reorder_find(tqp, tqp->rx_expected_psn);
		if (!msg || !msg->complete)
			return;
		if (!tbv_rx_deliver_reorder_msg_locked(state, tqp, msg))
			return;
	}
}

static void tbv_rx_drop_reorder_msg_locked(struct tbv_state *state,
					   struct tbv_qp *tqp,
					   struct tbv_rx_reorder_msg *msg)
{
	list_del(&msg->node);
	tqp->rx_reorder_count--;
	tqp->rx_reorder_bytes -= msg->total_len;
	atomic64_inc(&state->data_rx_reorder_dropped);
	tbv_rx_reorder_free_msg(msg);
}

static void tbv_rx_buffer_fragment_locked(struct tbv_state *state,
					  struct tbv_qp *tqp,
					  const struct tbv_native_data_header *hdr,
					  u32 psn, u32 total_len, u32 offset,
					  bool last, const void *payload)
{
	struct tbv_rx_reorder_msg *msg;
	s32 delta = tbv_psn_delta(psn, tqp->rx_expected_psn);
	bool with_imm = hdr->opcode == TBV_NATIVE_DATA_OP_SEND_IMM;
	u32 imm_data = with_imm ? hdr->rkey : 0;
	u32 frag_idx;
	u32 frag_count;

	if (delta < 0) {
		tbv_send_ack(state, hdr->src_qp, hdr->dest_qp, psn, 1);
		return;
	}
	if (delta >= TBV_RX_REORDER_MAX_MESSAGES) {
		atomic64_inc(&state->data_rx_reorder_window);
		tbv_send_ack(state, hdr->src_qp, hdr->dest_qp, psn, 1);
		return;
	}
	if (!tbv_rx_fragment_shape(total_len, offset, hdr->length, last,
				   &frag_idx, &frag_count)) {
		tbv_send_ack(state, hdr->src_qp, hdr->dest_qp, psn, 1);
		return;
	}

	msg = tbv_rx_reorder_find(tqp, psn);
	if (!msg) {
		u32 new_bytes = tqp->rx_reorder_bytes;

		if (tqp->rx_reorder_count >= TBV_RX_REORDER_MAX_MESSAGES ||
		    check_add_overflow(tqp->rx_reorder_bytes, total_len,
				       &new_bytes) ||
		    new_bytes > TBV_RX_REORDER_MAX_BYTES) {
			atomic64_inc(&state->data_rx_reorder_window);
			tbv_send_ack(state, hdr->src_qp, hdr->dest_qp, psn, 1);
			return;
		}

		msg = kzalloc(sizeof(*msg), GFP_KERNEL);
		if (!msg) {
			tbv_send_ack(state, hdr->src_qp, hdr->dest_qp, psn, 1);
			return;
		}
		if (total_len) {
			msg->buf = kvzalloc(total_len, GFP_KERNEL);
			if (!msg->buf) {
				kfree(msg);
				tbv_send_ack(state, hdr->src_qp, hdr->dest_qp,
					     psn, 1);
				return;
			}
		}

		msg->src_qp = hdr->src_qp;
		msg->psn = psn;
		msg->total_len = total_len;
		msg->with_imm = with_imm;
		msg->imm_data = imm_data;
		msg->frag_count = frag_count;
		msg->solicited = hdr->flags & TBV_NATIVE_DATA_F_SOLICITED;
		list_add_tail(&msg->node, &tqp->rx_reorder);
		tqp->rx_reorder_count++;
		tqp->rx_reorder_bytes = new_bytes;
		atomic64_inc(&state->data_rx_reorder_buffered);
	} else if (msg->src_qp != hdr->src_qp ||
		   msg->total_len != total_len ||
		   msg->with_imm != with_imm ||
		   msg->imm_data != imm_data ||
		   msg->frag_count != frag_count ||
		   test_bit(frag_idx, msg->frag_seen)) {
		tbv_rx_drop_reorder_msg_locked(state, tqp, msg);
		tbv_send_ack(state, hdr->src_qp, hdr->dest_qp, psn, 1);
		return;
	}

	if (hdr->length && msg->buf)
		memcpy((u8 *)msg->buf + offset, payload, hdr->length);
	set_bit(frag_idx, msg->frag_seen);
	msg->frags_received++;
	msg->received += hdr->length;
	if (msg->frags_received == msg->frag_count)
		msg->complete = true;
	if (msg->complete)
		tbv_rx_drain_reorder_locked(state, tqp);
}

static void tbv_rx_handle_send_fragment(struct tbv_state *state,
					struct tbv_qp *tqp,
					const struct tbv_native_data_header *hdr,
					const void *payload)
{
	struct tbv_rx_message *msg = &tqp->rx_msg;
	u32 total_len = hdr->imm_data;
	u32 psn = hdr->psn & TBV_PSN_MASK;
	u64 frag_end64;
	u32 offset;
	bool last = hdr->flags & TBV_NATIVE_DATA_F_LAST;
	bool with_imm = hdr->opcode == TBV_NATIVE_DATA_OP_SEND_IMM;
	u32 imm_data = with_imm ? hdr->rkey : 0;

	if (hdr->remote_addr > U32_MAX ||
	    check_add_overflow(hdr->remote_addr, (u64)hdr->length,
			       &frag_end64) ||
	    frag_end64 > total_len ||
	    total_len > TBV_NATIVE_DATA_MAX_MSG_SIZE ||
	    (!last && !hdr->length) ||
	    (hdr->flags & ~(TBV_NATIVE_DATA_F_LAST |
			    TBV_NATIVE_DATA_F_SOLICITED)) ||
	    (!with_imm && hdr->rkey) ||
	    last != (frag_end64 == total_len)) {
		tbv_send_ack(state, hdr->src_qp, hdr->dest_qp, psn, 1);
		return;
	}
	offset = (u32)hdr->remote_addr;

	mutex_lock(&tqp->rx_lock);
	if (state->native_fragment_striping) {
		tbv_rx_buffer_fragment_locked(state, tqp, hdr, psn, total_len,
					      offset, last, payload);
		mutex_unlock(&tqp->rx_lock);
		return;
	}

	if (msg->active) {
		if (msg->src_qp != hdr->src_qp || msg->psn != psn ||
		    msg->total_len != total_len ||
		    msg->with_imm != with_imm ||
		    msg->imm_data != imm_data ||
		    msg->received != offset) {
			if (tbv_psn_delta(psn, tqp->rx_expected_psn) > 0) {
				tbv_rx_buffer_fragment_locked(state, tqp, hdr,
							      psn, total_len,
							      offset, last,
							      payload);
				mutex_unlock(&tqp->rx_lock);
				return;
			}

			tbv_rx_fail_active_send(state, tqp,
						IB_WC_LOC_PROT_ERR);
			mutex_unlock(&tqp->rx_lock);
			tbv_send_ack(state, hdr->src_qp, hdr->dest_qp, psn, 1);
			return;
		}
	} else if (tbv_rx_reorder_find(tqp, psn) ||
		   psn != tqp->rx_expected_psn) {
		tbv_rx_buffer_fragment_locked(state, tqp, hdr, psn, total_len,
					      offset, last, payload);
		mutex_unlock(&tqp->rx_lock);
		return;
	} else {
		if (offset) {
			mutex_unlock(&tqp->rx_lock);
			tbv_send_ack(state, hdr->src_qp, hdr->dest_qp, psn, 1);
			return;
		}
		if (!tbv_qp_pop_recv(tqp, &msg->wqe)) {
			atomic64_inc(&state->data_rx_no_recv);
			tbv_rx_buffer_fragment_locked(state, tqp, hdr, psn,
						      total_len, offset, last,
						      payload);
			mutex_unlock(&tqp->rx_lock);
			return;
		}

		msg->active = true;
		msg->src_qp = hdr->src_qp;
		msg->psn = psn;
		msg->total_len = total_len;
		msg->with_imm = with_imm;
		msg->imm_data = imm_data;
		msg->status = total_len > msg->wqe.length ?
				      IB_WC_LOC_LEN_ERR :
				      IB_WC_SUCCESS;
		msg->solicited = hdr->flags & TBV_NATIVE_DATA_F_SOLICITED;
	}

	if (tbv_rx_copy_to_wqe(state, &msg->wqe, offset, payload, hdr->length,
			       &msg->delivered))
		msg->status = IB_WC_LOC_PROT_ERR;

	msg->received += hdr->length;
	if (last) {
		tbv_rx_finish_send(state, tqp);
		tbv_rx_drain_reorder_locked(state, tqp);
	}
	mutex_unlock(&tqp->rx_lock);
}

static int tbv_rx_complete_write_imm(struct tbv_state *state,
				     struct tbv_qp *tqp,
				     const struct tbv_native_data_header *hdr)
{
	struct tbv_cq *recv_cq = container_of(tqp->base.recv_cq,
					      struct tbv_cq, base);
	struct tbv_recv_wqe wqe;
	struct ib_wc wc = {};

	if (!tbv_qp_pop_recv(tqp, &wqe)) {
		atomic64_inc(&state->data_rx_no_recv);
		return -ENODATA;
	}

	wc.wr_id = wqe.wr_id;
	wc.status = IB_WC_SUCCESS;
	wc.opcode = IB_WC_RECV_RDMA_WITH_IMM;
	wc.qp = &tqp->base;
	wc.src_qp = hdr->src_qp;
	wc.pkey_index = 0;
	wc.port_num = 1;
	wc.wc_flags = IB_WC_WITH_IMM;
	wc.ex.imm_data = cpu_to_be32(hdr->imm_data);
	return tbv_cq_push(recv_cq, &wc);
}

static void tbv_rx_handle_rdma_write_fragment(struct tbv_state *state,
					      struct tbv_qp *tqp,
					      const struct tbv_native_data_header *hdr,
					      const void *payload)
{
	struct tbv_mr *mr;
	bool last = hdr->flags & TBV_NATIVE_DATA_F_LAST;
	bool with_imm = hdr->opcode == TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM;
	int ret;

	if ((hdr->flags & ~(TBV_NATIVE_DATA_F_LAST |
			    TBV_NATIVE_DATA_F_SOLICITED)) ||
	    (!last && !hdr->length) ||
	    !(tqp->attr.qp_access_flags & IB_ACCESS_REMOTE_WRITE)) {
		atomic64_inc(&state->data_rx_bad_header);
		tbv_send_ack(state, hdr->src_qp, hdr->dest_qp, hdr->psn, 1);
		return;
	}

	mr = tbv_mr_get(state, hdr->rkey);
	if (!mr) {
		atomic64_inc(&state->data_rx_copy_error);
		tbv_send_ack(state, hdr->src_qp, hdr->dest_qp, hdr->psn, 1);
		return;
	}

	if (!(mr->access & IB_ACCESS_REMOTE_WRITE)) {
		atomic64_inc(&state->data_rx_copy_error);
		tbv_mr_put(mr);
		tbv_send_ack(state, hdr->src_qp, hdr->dest_qp, hdr->psn, 1);
		return;
	}

	ret = tbv_umem_copy_to_iova(mr, hdr->remote_addr, payload,
				    hdr->length);
	tbv_mr_put(mr);
	if (ret) {
		atomic64_inc(&state->data_rx_copy_error);
		tbv_send_ack(state, hdr->src_qp, hdr->dest_qp, hdr->psn, 1);
		return;
	}

	if (last) {
		if (with_imm)
			ret = tbv_rx_complete_write_imm(state, tqp, hdr);
		tbv_send_ack(state, hdr->src_qp, hdr->dest_qp, hdr->psn,
			     ret ? 1 : 0);
	}
}

static void tbv_read_resp_stream_put(struct tbv_read_resp_stream *stream)
{
	if (refcount_dec_and_test(&stream->refs)) {
		tbv_mr_put(stream->mr);
		kfree(stream);
	}
}

static void tbv_read_resp_stream_done(void *ctx, int status)
{
	tbv_read_resp_stream_put(ctx);
}

static int tbv_read_resp_next_page(void *ctx, struct page **page,
				   u32 *page_off, u32 *length,
				   tbv_path_tx_done_fn *done,
				   void **done_ctx)
{
	struct tbv_read_resp_stream *stream = ctx;
	u32 remaining = stream->total_len - stream->offset;
	int ret;

	ret = tbv_umem_page_from_iova(stream->mr, stream->iova + stream->offset,
				      remaining, page, page_off, length);
	if (ret)
		return ret;

	stream->offset += *length;
	refcount_inc(&stream->refs);
	*done = tbv_read_resp_stream_done;
	*done_ctx = stream;
	return 0;
}

static int tbv_send_read_response_zcopy(struct tbv_state *state,
					const struct tbv_native_data_header *req,
					struct tbv_mr *mr)
{
	struct tbv_read_resp_stream *stream;
	struct tbv_native_data_header resp = {};
	struct tbv_path *path;
	int ret;

	stream = kzalloc(sizeof(*stream), GFP_KERNEL);
	if (!stream)
		return -ENOMEM;
	refcount_inc(&mr->refs);
	stream->mr = mr;
	refcount_set(&stream->refs, 1);
	stream->iova = req->remote_addr;
	stream->total_len = req->imm_data;

	resp.opcode = TBV_NATIVE_DATA_OP_RDMA_READ_RESP;
	resp.dest_qp = req->src_qp;
	resp.src_qp = req->dest_qp;
	resp.psn = req->psn;
	resp.imm_data = req->imm_data;

	mutex_lock(&state->lock);
	path = tbv_select_native_data_path_get_locked(state,
						     req->src_qp ^ req->psn);
	mutex_unlock(&state->lock);
	if (!path) {
		ret = -ENOTCONN;
		goto out_put_stream;
	}

	ret = tbv_path_send_page_stream(path, &resp, req->imm_data, 0,
					NULL, NULL,
					tbv_read_resp_next_page, stream);
	tbv_release_path_refs(&path, 1);

out_put_stream:
	tbv_read_resp_stream_put(stream);
	return ret;
}

static int tbv_send_read_response_copy(struct tbv_state *state,
				       const struct tbv_native_data_header *req,
				       struct tbv_mr *mr)
{
	struct tbv_path *path;
	u32 total_len = req->imm_data;
	u32 nfrags = total_len ? DIV_ROUND_UP(total_len,
					      TBV_NATIVE_DATA_MAX_PAYLOAD) : 1;
	u32 offset = 0;
	u32 remaining = nfrags;
	bool sent_any = false;
	int ret = 0;

	mutex_lock(&state->lock);
	path = tbv_select_native_data_path_get_locked(state,
						     req->src_qp ^ req->psn);
	mutex_unlock(&state->lock);
	if (!path)
		return -ENOTCONN;

	ret = tbv_path_reserve_data(path, nfrags);
	if (ret)
		goto out_put_path;

	do {
		struct tbv_native_data_header resp = {};
		u32 payload_len = min_t(u32, total_len - offset,
					TBV_NATIVE_DATA_MAX_PAYLOAD);
		u32 packet_len = TBV_NATIVE_DATA_HDR_SIZE + payload_len;
		bool last = offset + payload_len == total_len;
		u8 *frame;
		int len;

		frame = kmalloc(packet_len, GFP_KERNEL);
		if (!frame) {
			ret = -ENOMEM;
			goto out_release_reservation;
		}

		if (payload_len) {
			ret = tbv_umem_copy_from_iova(mr, req->remote_addr + offset,
						      frame + TBV_NATIVE_DATA_HDR_SIZE,
						      payload_len);
			if (ret) {
				kfree(frame);
				goto out_release_reservation;
			}
		}

		resp.opcode = TBV_NATIVE_DATA_OP_RDMA_READ_RESP;
		resp.flags = last ? TBV_NATIVE_DATA_F_LAST : 0;
		resp.dest_qp = req->src_qp;
		resp.src_qp = req->dest_qp;
		resp.psn = req->psn;
		resp.length = payload_len;
		resp.imm_data = total_len;
		resp.remote_addr = offset;

		len = tbv_native_data_build_header(frame, packet_len, &resp);
		if (len < 0) {
			kfree(frame);
			ret = len;
			goto out_release_reservation;
		}

		ret = tbv_path_send_owned(path, frame, packet_len,
					  TBV_PATH_SEND_DEFER, NULL, NULL);
		if (ret)
			goto out_release_reservation;

		remaining--;
		sent_any = true;
		offset += payload_len;
	} while (offset < total_len);

	tbv_path_kick_tx(path);
	tbv_release_path_refs(&path, 1);
	return 0;

out_release_reservation:
	tbv_path_release_data_reservation(path, remaining);
	if (sent_any)
		tbv_path_kick_tx(path);
out_put_path:
	tbv_release_path_refs(&path, 1);
	return ret;
}

static void tbv_read_req_workfn(struct work_struct *work)
{
	struct tbv_read_req_work *req_work =
		container_of(work, struct tbv_read_req_work, work);
	struct tbv_native_data_header *req = &req_work->hdr;
	struct tbv_state *state = req_work->state;
	struct tbv_qp *tqp;
	struct tbv_mr *mr;
	u64 addr;
	int ret = 0;

	if (!READ_ONCE(state->verbs_registered))
		goto out_free;

	tqp = tbv_qp_get_by_num(state, req->dest_qp);
	if (!tqp) {
		atomic64_inc(&state->data_rx_no_qp);
		tbv_send_read_status(state, req->src_qp, req->dest_qp,
				     req->psn, req->imm_data, -EINVAL);
		goto out_free;
	}

	if (!(tqp->attr.qp_access_flags & IB_ACCESS_REMOTE_READ)) {
		ret = -EACCES;
		goto out_put_qp_status;
	}

	mr = tbv_mr_get(state, req->rkey);
	if (!mr) {
		ret = -EACCES;
		goto out_put_qp_status;
	}
	if (!(mr->access & IB_ACCESS_REMOTE_READ)) {
		ret = -EACCES;
		goto out_put_mr_status;
	}
	if (req->imm_data > TBV_NATIVE_DATA_MAX_MSG_SIZE) {
		ret = -EMSGSIZE;
		goto out_put_mr_status;
	}
	ret = tbv_umem_iova_to_addr(mr, req->remote_addr, req->imm_data,
				    &addr);
	if (ret)
		goto out_put_mr_status;

	if (!req->imm_data) {
		tbv_send_read_status(state, req->src_qp, req->dest_qp,
				     req->psn, 0, 0);
		goto out_put_mr;
	}

	if (zcopy_min_bytes && req->imm_data < zcopy_min_bytes)
		ret = tbv_send_read_response_copy(state, req, mr);
	else
		ret = tbv_send_read_response_zcopy(state, req, mr);
	if (ret)
		tbv_send_read_status(state, req->src_qp, req->dest_qp,
				     req->psn, req->imm_data, ret);

out_put_mr:
	tbv_mr_put(mr);
	tbv_qp_put(tqp);
	goto out_free;

out_put_mr_status:
	tbv_mr_put(mr);
out_put_qp_status:
	tbv_send_read_status(state, req->src_qp, req->dest_qp, req->psn,
			     req->imm_data, ret);
	tbv_qp_put(tqp);
out_free:
	kfree(req_work);
}

static void tbv_rx_handle_rdma_read_req(struct tbv_state *state,
					const struct tbv_native_data_header *hdr)
{
	struct tbv_read_req_work *work;

	if (hdr->length || !(hdr->flags & TBV_NATIVE_DATA_F_LAST)) {
		atomic64_inc(&state->data_rx_bad_header);
		tbv_send_read_status(state, hdr->src_qp, hdr->dest_qp,
				     hdr->psn, hdr->imm_data, -EINVAL);
		return;
	}

	work = kzalloc(sizeof(*work), GFP_ATOMIC);
	if (!work) {
		tbv_send_read_status(state, hdr->src_qp, hdr->dest_qp,
				     hdr->psn, hdr->imm_data, -ENOMEM);
		return;
	}
	INIT_WORK(&work->work, tbv_read_req_workfn);
	work->state = state;
	work->hdr = *hdr;
	queue_work(system_unbound_wq, &work->work);
}

static void tbv_rx_handle_rdma_read_resp(struct tbv_state *state,
					 const struct tbv_native_data_header *hdr,
					 const void *payload)
{
	struct tbv_read_ctx *read;
	struct tbv_qp *tqp;
	u32 next_received;
	bool last = hdr->flags & TBV_NATIVE_DATA_F_LAST;
	int ret = 0;

	tqp = tbv_qp_get_by_num(state, hdr->dest_qp);
	if (!tqp) {
		atomic64_inc(&state->data_rx_no_qp);
		return;
	}

	read = tbv_qp_find_read_get(tqp, hdr->psn);
	if (!read) {
		atomic64_inc(&state->data_rx_bad_header);
		tbv_qp_put(tqp);
		return;
	}

	if (hdr->rkey) {
		ret = -EIO;
		goto complete;
	}
	if (hdr->remote_addr > U32_MAX ||
	    hdr->imm_data != read->total_len ||
	    check_add_overflow((u32)hdr->remote_addr, hdr->length,
			       &next_received) ||
	    next_received > read->total_len ||
	    hdr->remote_addr != read->received) {
		ret = -EINVAL;
		goto complete;
	}

	ret = tbv_copy_to_read_segments(read, hdr->remote_addr, payload,
					hdr->length);
	if (ret)
		goto complete;

	read->received = next_received;
	if (!last) {
		tbv_read_ctx_put(read);
		tbv_qp_put(tqp);
		return;
	}
	if (read->received != read->total_len)
		ret = -EINVAL;

complete:
	if (tbv_qp_unqueue_read(tqp, read)) {
		tbv_read_complete(read, ret);
		tbv_read_ctx_put(read);
	}
	tbv_read_ctx_put(read);
	tbv_qp_put(tqp);
}

static int tbv_poll_cq(struct ib_cq *cq, int num_entries, struct ib_wc *wc)
{
	struct tbv_cq *tcq = container_of(cq, struct tbv_cq, base);
	unsigned long flags;
	int polled = 0;

	if (num_entries <= 0 || !wc)
		return 0;

	spin_lock_irqsave(&tcq->lock, flags);
	while (polled < num_entries && tcq->count) {
		wc[polled++] = tcq->entries[tcq->head];
		tcq->head = (tcq->head + 1) % tcq->cqe;
		tcq->count--;
	}
	spin_unlock_irqrestore(&tcq->lock, flags);

	return polled;
}

static int tbv_req_notify_cq(struct ib_cq *cq, enum ib_cq_notify_flags flags)
{
	struct tbv_cq *tcq = container_of(cq, struct tbv_cq, base);
	unsigned long irq_flags;
	int ret;

	spin_lock_irqsave(&tcq->lock, irq_flags);
	tcq->notify_armed = true;
	ret = tcq->count ? 1 : 0;
	spin_unlock_irqrestore(&tcq->lock, irq_flags);
	return ret;
}

void tbv_ibdev_rx_native_frame(struct tbv_state *state,
			       const struct tbv_native_data_header *hdr,
			       const void *payload)
{
	struct tbv_qp *tqp;

	if (!state || !state->verbs_registered)
		return;

	if (hdr->opcode == TBV_NATIVE_DATA_OP_SEND_ACK) {
		struct tbv_send_ctx *send;

		atomic64_inc(&state->data_rx_ack);
		tqp = tbv_qp_get_by_num(state, hdr->dest_qp);
		if (!tqp) {
			atomic64_inc(&state->data_rx_no_qp);
			return;
		}

		send = tbv_qp_take_send(tqp, hdr->psn);
		if (send) {
			tbv_send_complete(send, hdr->imm_data ? -EIO : 0);
			tbv_send_ctx_put(send);
		}
		tbv_qp_put(tqp);
		return;
	}

	if (hdr->opcode == TBV_NATIVE_DATA_OP_RECV_CREDIT) {
		unsigned long flags;
		u32 new_credits;

		atomic64_inc(&state->data_rx_ack);
		if (!hdr->imm_data) {
			atomic64_inc(&state->data_rx_bad_header);
			return;
		}

		tqp = tbv_qp_get_by_num(state, hdr->dest_qp);
		if (!tqp) {
			atomic64_inc(&state->data_rx_no_qp);
			return;
		}

		spin_lock_irqsave(&tqp->lock, flags);
		if (!check_add_overflow(tqp->remote_recv_credits,
					hdr->imm_data, &new_credits))
			tqp->remote_recv_credits = new_credits;
		spin_unlock_irqrestore(&tqp->lock, flags);
		wake_up_all(&tqp->credit_wait);
		tbv_qp_put(tqp);
		return;
	}

	if (hdr->opcode == TBV_NATIVE_DATA_OP_RDMA_READ_REQ) {
		tbv_rx_handle_rdma_read_req(state, hdr);
		return;
	}

	if (hdr->opcode == TBV_NATIVE_DATA_OP_RDMA_READ_RESP) {
		tbv_rx_handle_rdma_read_resp(state, hdr, payload);
		return;
	}

	if (hdr->opcode != TBV_NATIVE_DATA_OP_SEND &&
	    hdr->opcode != TBV_NATIVE_DATA_OP_SEND_IMM &&
	    hdr->opcode != TBV_NATIVE_DATA_OP_RDMA_WRITE &&
	    hdr->opcode != TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM) {
		atomic64_inc(&state->data_rx_bad_header);
		return;
	}
	atomic64_inc(&state->data_rx_send);
	if (hdr->opcode == TBV_NATIVE_DATA_OP_SEND)
		atomic64_inc(&state->data_rx_op_send);
	else if (hdr->opcode == TBV_NATIVE_DATA_OP_SEND_IMM)
		atomic64_inc(&state->data_rx_op_send_imm);
	else if (hdr->opcode == TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM)
		atomic64_inc(&state->data_rx_op_write_imm);
	else
		atomic64_inc(&state->data_rx_op_write);

	tqp = tbv_qp_get_by_num(state, hdr->dest_qp);
	if (!tqp) {
		atomic64_inc(&state->data_rx_no_qp);
		tbv_send_ack(state, hdr->src_qp, hdr->dest_qp, hdr->psn, 1);
		return;
	}

	if (hdr->opcode == TBV_NATIVE_DATA_OP_SEND ||
	    hdr->opcode == TBV_NATIVE_DATA_OP_SEND_IMM)
		tbv_rx_handle_send_fragment(state, tqp, hdr, payload);
	else
		tbv_rx_handle_rdma_write_fragment(state, tqp, hdr, payload);
	tbv_qp_put(tqp);
}

void tbv_ibdev_rx_frame(struct tbv_state *state, const void *data, u32 len)
{
	struct tbv_native_data_header hdr;
	const u8 *payload;
	int ret;

	if (!state || !state->verbs_registered)
		return;

	ret = tbv_native_data_parse_header(data, len, &hdr);
	if (ret) {
		atomic64_inc(&state->data_rx_bad_header);
		return;
	}
	if (hdr.length > len - TBV_NATIVE_DATA_HDR_SIZE) {
		atomic64_inc(&state->data_rx_bad_header);
		return;
	}

	payload = (const u8 *)data + TBV_NATIVE_DATA_HDR_SIZE;
	tbv_ibdev_rx_native_frame(state, &hdr, payload);
}

static struct ib_mr *tbv_reg_user_mr(struct ib_pd *pd, u64 start, u64 length,
				     u64 virt_addr, int access,
				     struct ib_dmah *dmah,
				     struct ib_udata *udata)
{
	struct tbv_mr *mr;
	unsigned long flags;
	u32 key;
	int ret;

	if (!length)
		return ERR_PTR(-EINVAL);

	mr = kzalloc(sizeof(*mr), GFP_KERNEL);
	if (!mr)
		return ERR_PTR(-ENOMEM);

	mr->umem = ib_umem_get(pd->device, start, length, access);
	if (IS_ERR(mr->umem)) {
		struct ib_umem *umem = mr->umem;

		kfree(mr);
		return ERR_CAST(umem);
	}

	key = atomic_inc_return(&tbv_mr_key);
	mr->base.lkey = key;
	mr->base.rkey = key;
	mr->owner = tbv_ibdev_state(pd->device);
	refcount_set(&mr->refs, 1);
	init_completion(&mr->refs_zero);
	mr->start = start;
	mr->length = length;
	mr->virt_addr = virt_addr;
	mr->access = access;
	xa_lock_irqsave(&mr->owner->verbs_mrs_xa, flags);
	ret = __xa_insert(&mr->owner->verbs_mrs_xa, key, mr, GFP_KERNEL);
	xa_unlock_irqrestore(&mr->owner->verbs_mrs_xa, flags);
	if (ret) {
		ib_umem_release(mr->umem);
		kfree(mr);
		return ERR_PTR(ret);
	}
	atomic_inc(&mr->owner->verbs_mrs);
	return &mr->base;
}

static int tbv_dereg_mr(struct ib_mr *ibmr, struct ib_udata *udata)
{
	struct tbv_mr *mr = container_of(ibmr, struct tbv_mr, base);
	unsigned long flags;

	if (mr->owner) {
		xa_lock_irqsave(&mr->owner->verbs_mrs_xa, flags);
		mr->closing = true;
		__xa_erase(&mr->owner->verbs_mrs_xa, ibmr->lkey);
		xa_unlock_irqrestore(&mr->owner->verbs_mrs_xa, flags);
	}
	tbv_mr_put(mr);
	wait_for_completion(&mr->refs_zero);
	if (mr->owner)
		atomic_dec(&mr->owner->verbs_mrs);
	ib_umem_release(mr->umem);
	kfree(mr);
	return 0;
}

static const struct ib_device_ops tbv_ibdev_ops = {
	.owner = THIS_MODULE,
	.driver_id = RDMA_DRIVER_UNKNOWN,
	.uverbs_abi_ver = TBV_IBDEV_ABI_VERSION,
	.uverbs_no_driver_id_binding = 1,

	.query_device = tbv_query_device,
	.query_port = tbv_query_port,
	.query_gid = tbv_query_gid,
	.query_pkey = tbv_query_pkey,
	.get_port_immutable = tbv_get_port_immutable,
	.get_link_layer = tbv_get_link_layer,
	.get_netdev = tbv_get_netdev,
	.add_gid = tbv_add_gid,
	.del_gid = tbv_del_gid,

	.alloc_ucontext = tbv_alloc_ucontext,
	.dealloc_ucontext = tbv_dealloc_ucontext,
	.alloc_pd = tbv_alloc_pd,
	.dealloc_pd = tbv_dealloc_pd,
	.create_cq = tbv_create_cq,
	.destroy_cq = tbv_destroy_cq,
	.create_qp = tbv_create_qp,
	.destroy_qp = tbv_destroy_qp,
	.modify_qp = tbv_modify_qp,
	.query_qp = tbv_query_qp,
	.post_send = tbv_post_send,
	.post_recv = tbv_post_recv,
	.poll_cq = tbv_poll_cq,
	.req_notify_cq = tbv_req_notify_cq,
	.reg_user_mr = tbv_reg_user_mr,
	.dereg_mr = tbv_dereg_mr,

	INIT_RDMA_OBJ_SIZE(ib_ucontext, tbv_ucontext, base),
	INIT_RDMA_OBJ_SIZE(ib_pd, tbv_pd, base),
	INIT_RDMA_OBJ_SIZE(ib_cq, tbv_cq, base),
	INIT_RDMA_OBJ_SIZE(ib_qp, tbv_qp, base),
};

int tbv_ibdev_start(struct tbv_state *state, bool register_verbs)
{
	struct tbv_ibdev *dev;
	struct device *dma_device;
	int ret;

	state->register_verbs = register_verbs;
	if (!register_verbs)
		return 0;

	dev = ib_alloc_device(tbv_ibdev, base);
	if (!dev)
		return -ENOMEM;

	dev->state = state;
	dev->base.phys_port_cnt = TBV_IBDEV_PORTS;
	dev->base.num_comp_vectors = num_possible_cpus();
	dev->base.local_dma_lkey = 0;
	dev->base.node_type = RDMA_NODE_IB_CA;
	dev->base.node_guid = cpu_to_be64(0x0200544256524253ULL);
	dev->base.uverbs_cmd_mask |=
		BIT_ULL(IB_USER_VERBS_CMD_POST_SEND) |
		BIT_ULL(IB_USER_VERBS_CMD_POST_RECV) |
		BIT_ULL(IB_USER_VERBS_CMD_POLL_CQ) |
		BIT_ULL(IB_USER_VERBS_CMD_REQ_NOTIFY_CQ);

	ib_set_device_ops(&dev->base, &tbv_ibdev_ops);

	dma_device = tbv_state_get_verbs_parent(state);
	dev->base.dev.parent = dma_device;
	ret = ib_register_device(&dev->base, "usb4_rdma%d", dma_device);
	if (ret) {
		if (dma_device)
			put_device(dma_device);
		ib_dealloc_device(&dev->base);
		return ret;
	}

	state->ibdev = dev;
	state->verbs_registered = true;
	pr_info("registered ib_device %s dma_device=%s\n",
		dev_name(&dev->base.dev),
		dma_device ? dev_name(dma_device) : "<none>");
	if (dma_device)
		put_device(dma_device);
	return 0;
}

void tbv_ibdev_stop(struct tbv_state *state)
{
	struct tbv_ibdev *dev = state->ibdev;

	if (!dev)
		return;

	state->verbs_registered = false;
	flush_workqueue(system_unbound_wq);
	state->ibdev = NULL;
	ib_unregister_device(&dev->base);
	ib_dealloc_device(&dev->base);
	ida_destroy(&tbv_qpn_ida);
	pr_info("unregistered ib_device\n");
}
