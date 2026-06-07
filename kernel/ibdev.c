// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/crc32.h>
#include <linux/crc32c.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/highmem.h>
#include <linux/idr.h>
#include <linux/in6.h>
#include <linux/ip.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mmzone.h>
#include <linux/netdevice.h>
#include <linux/overflow.h>
#include <linux/refcount.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/thunderbolt.h>
#include <linux/version.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <rdma/ib_cache.h>
#include <rdma/ib_mad.h>
#include <rdma/ib_umem.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/ib_verbs.h>

#include "../proto/native_data.h"
#include "../proto/reliability.h"
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
/*
 * Native SEND receives can observe future PSNs when Thunderbolt paths deliver
 * fragments out of order under load.  The provider advertises
 * TBV_IBDEV_MAX_QP_WR outstanding WRs, so the receive-side reorder window must
 * cover that contract instead of failing legal traffic at an arbitrary smaller
 * depth.
 */
#define TBV_RX_REORDER_MAX_MESSAGES TBV_IBDEV_MAX_QP_WR
#define TBV_RX_REORDER_MAX_BYTES (64u * 1024u * 1024u)
#define TBV_RX_REORDER_MAX_FRAGS TBV_NATIVE_DATA_MAX_FRAGS
#define TBV_ACK_HISTORY_SIZE TBV_IBDEV_MAX_QP_WR
#define TBV_IBDEV_GID_TBL_LEN 8
#define TBV_APPLE_PENDING_RX_DEFAULT_SLOTS 4096
#define TBV_APPLE_PENDING_RX_MAX_SLOTS 16384
#define TBV_APPLE_PENDING_RX_TOTAL_BYTES_DEFAULT (64u * 1024u * 1024u)
#define TBV_APPLE_RAW_SLOT_USER_SIZE 256
#define TBV_APPLE_RAW_SPLIT_USER_SIZE 16
#define TBV_APPLE_RAW_TAIL_USER_SIZE 240
#define TBV_APPLE_RAW_DESCS_PER_CHUNK 17
#define TBV_QP_TIMEOUT_DEFAULT_MS 5000
#define TBV_QP_TIMEOUT_WORK_INTERVAL_MS 1000
#define TBV_SEND_MAX_RETRIES 7
#define TBV_SEND_RNR_RETRIES_INFINITE ((u8)~0u)
#define TBV_READ_RESP_RETRY_MS 100
#define TBV_READ_RESP_MAX_RETRIES 3
#define TBV_GSI_QPN 1
#define TBV_GSI_MAD_META_SIZE 40
#define TBV_GSI_MAD_META_SGID_OFF 0
#define TBV_GSI_MAD_META_DGID_OFF 16
#define TBV_GSI_MAD_META_PKEY_OFF 32
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
		 "Minimum native bytes before raw zero-copy streaming is requested; retryable native RC WRITE falls back to framed copies");

static uint qp_timeout_ms = TBV_QP_TIMEOUT_DEFAULT_MS;
module_param(qp_timeout_ms, uint, 0644);
MODULE_PARM_DESC(qp_timeout_ms,
		 "Fallback milliseconds for pending native/Apple WRs and partial native receives "
		 "when a QP has no verbs ACK timeout; 0 disables fallback timeout work");

static uint apple_tx_max_inflight_wr = 16;
module_param(apple_tx_max_inflight_wr, uint, 0644);
MODULE_PARM_DESC(apple_tx_max_inflight_wr,
		 "Maximum Apple-compatible UC SEND work requests in flight per QP; 0 disables the software window");

static uint apple_tx_max_inflight_frames = 64;
module_param(apple_tx_max_inflight_frames, uint, 0644);
MODULE_PARM_DESC(apple_tx_max_inflight_frames,
		 "Maximum Apple-compatible 4 KiB FA57 frames posted per SEND group; 0 disables the frame window");

static uint apple_tx_completion_delay_us;
module_param(apple_tx_completion_delay_us, uint, 0644);
MODULE_PARM_DESC(apple_tx_completion_delay_us,
		 "Microseconds to delay successful Apple-compatible UC SEND completions after local frame TX completion; 0 disables the delay");

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

static uint apple_rx_trace;
module_param(apple_rx_trace, uint, 0644);
MODULE_PARM_DESC(apple_rx_trace,
		 "Print the first N Apple RX callbacks with SOF/EOF and assembly state");

static bool tbv_apple_rx_trace_take(void)
{
	u32 remaining;

	do {
		remaining = READ_ONCE(apple_rx_trace);
		if (!remaining)
			return false;
	} while (cmpxchg(&apple_rx_trace, remaining, remaining - 1) !=
		 remaining);

	return true;
}

static void tbv_rx_bad_header_context(const struct tbv_path *rx_path,
				      u32 *peer_id, u32 *rail_id,
				      u32 *path_id, u64 *route)
{
	*peer_id = U32_MAX;
	*rail_id = U32_MAX;
	*path_id = 0;
	*route = 0;
	if (!rx_path || !rx_path->rail)
		return;

	*rail_id = rx_path->rail->rail_id;
	*path_id = rx_path->rail->key.path_id;
	*route = rx_path->rail->key.route;
	if (rx_path->rail->peer)
		*peer_id = rx_path->rail->peer->peer_id;
}

static void tbv_rx_bad_header_note(struct tbv_state *state,
				   struct tbv_path *rx_path,
				   atomic64_t *reason_counter,
				   const char *reason,
				   const struct tbv_native_data_header *hdr,
				   u32 frame_len, int ret)
{
	u32 peer_id;
	u32 rail_id;
	u32 path_id;
	u64 route;

	atomic64_inc(&state->data_rx_bad_header);
	if (reason_counter)
		atomic64_inc(reason_counter);

	tbv_rx_bad_header_context(rx_path, &peer_id, &rail_id, &path_id, &route);
	if (hdr) {
		pr_warn_ratelimited("native RX bad header reason=%s ret=%d frame_len=%u op=%u flags=0x%x len=%u imm=%u psn=%u dst=%u src=%u off=%u peer=%u rail=%u path_id=%u route=0x%llx\n",
				    reason, ret, frame_len, hdr->opcode,
				    hdr->flags, hdr->length, hdr->imm_data,
				    hdr->psn, hdr->dest_qp, hdr->src_qp,
				    hdr->frag_offset, peer_id, rail_id, path_id,
				    (unsigned long long)route);
	} else {
		pr_warn_ratelimited("native RX bad header reason=%s ret=%d frame_len=%u peer=%u rail=%u path_id=%u route=0x%llx\n",
				    reason, ret, frame_len, peer_id, rail_id,
				    path_id, (unsigned long long)route);
	}
}

static void tbv_rx_bad_header_parse_note(struct tbv_state *state,
					 struct tbv_path *rx_path,
					 const void *data, u32 len, int ret)
{
	const tbv_wire_u8 *p = data;
	u32 peer_id;
	u32 rail_id;
	u32 path_id;
	u64 route;
	u32 magic = 0;
	u16 version = 0;
	u16 header_size = 0;
	u8 opcode = 0;
	u8 flags = 0;

	atomic64_inc(&state->data_rx_bad_header);
	atomic64_inc(&state->data_rx_bad_header_parse);

	if (p) {
		if (len >= 4)
			magic = tbv_wire_get_le32(p);
		if (len >= 6)
			version = tbv_wire_get_le16(p + 4);
		if (len >= 8)
			header_size = tbv_wire_get_le16(p + 6);
		if (len >= 9)
			opcode = p[8];
		if (len >= 10)
			flags = p[9];
	}

	tbv_rx_bad_header_context(rx_path, &peer_id, &rail_id, &path_id, &route);
	pr_warn_ratelimited("native RX bad header reason=parse ret=%d frame_len=%u magic=0x%x version=%u hdr_size=%u op=%u flags=0x%x peer=%u rail=%u path_id=%u route=0x%llx\n",
			    ret, len, magic, version, header_size, opcode, flags,
			    peer_id, rail_id, path_id, (unsigned long long)route);
}

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
	bool overflowed;
};

struct tbv_recv_wqe {
	u64 wr_id;
	struct ib_cqe *wr_cqe;
	u64 addr;
	u32 length;
	u32 lkey;
	bool kernel_cqe;
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
	unsigned long started_jiffies;
	u32 src_qp;
	u32 psn;
	u32 total_len;
	u32 imm_data;
	u32 received;
	u32 delivered;
	u16 frag_count;
	u16 frags_received;
	DECLARE_BITMAP(frag_seen, TBV_RX_REORDER_MAX_FRAGS);
	u32 first_rail_id;
	u32 last_rail_id;
	u64 first_route;
	u64 last_route;
	u32 first_path_id;
	u32 last_path_id;
	u32 last_offset;
	u32 last_len;
	int status;
	bool active;
	bool with_imm;
	bool solicited;
};

struct tbv_rx_write {
	struct tbv_recv_wqe imm_wqe;
	unsigned long started_jiffies;
	u64 remote_addr;
	u32 src_qp;
	u32 psn;
	u32 rkey;
	u32 imm_data;
	u32 received;
	bool active;
	bool with_imm;
	bool solicited;
};

struct tbv_rx_reorder_frag {
	struct list_head node;
	u32 offset;
	u32 len;
	u8 data[];
};

enum tbv_rx_reorder_kind {
	TBV_RX_REORDER_SEND,
	TBV_RX_REORDER_WRITE,
	TBV_RX_REORDER_READ_REQ,
};

struct tbv_rx_reorder_msg {
	struct list_head node;
	struct list_head frags;
	unsigned long first_jiffies;
	enum tbv_rx_reorder_kind kind;
	u64 remote_addr;
	u32 src_qp;
	u32 psn;
	u32 total_len;
	u32 imm_data;
	u32 rkey;
	u32 received;
	u32 buffered_bytes;
	u16 frag_count;
	u16 frags_received;
	DECLARE_BITMAP(frag_seen, TBV_RX_REORDER_MAX_FRAGS);
	bool complete;
	bool with_imm;
	bool solicited;
};

struct tbv_apple_pending_rx {
	void *buf;
	u32 capacity;
	u32 delivered;
	int status;
	bool active;
	bool ready;
};

struct tbv_ack_history_entry {
	u32 psn;
	int status;
	bool valid;
};

enum tbv_send_post_reason {
	TBV_SEND_POST_INITIAL,
	TBV_SEND_POST_RETRY_TIMEOUT,
	TBV_SEND_POST_RETRY_RNR,
};

struct tbv_send_ctx;

struct tbv_apple_sq_entry {
	struct list_head node;
	struct tbv_send_ctx *send;
	void *payload;
	u32 length;
};

struct tbv_qp {
	struct ib_qp base;
	struct tbv_state *owner;
	enum tbv_backend_type backend;
	/*
	 * QP rail invariant: every QP is bound to exactly one rail at
	 * create time and unbound at destroy time. Native non-GSI QPs may be
	 * assigned to any ready rail for the same peer so multi-QP workloads
	 * can use all cables while preserving per-QP FIFO ordering.
	 */
	struct tbv_rail *rail;
	spinlock_t lock;
	struct mutex rx_lock;
	wait_queue_head_t credit_wait;
	wait_queue_head_t apple_tx_wait;
	wait_queue_head_t refs_wait;
	refcount_t refs;
	struct completion refs_zero;
	struct list_head pending_sends;
	struct list_head pending_reads;
	struct list_head pending_read_resps;
	struct list_head apple_sq;
	struct work_struct apple_sq_work;
	struct delayed_work timeout_work;
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
	u32 early_remote_recv_credit_src_qp;
	u32 sendq_count;
	atomic_t apple_tx_inflight;
	atomic_t apple_tx_inflight_frames;
	u32 send_psn;
	u32 rx_expected_psn;
	u32 rx_rnr_psn;
	u32 rx_rnr_src_qp;
	u32 rx_rnr_frag_offset;
	u64 rx_rnr_remote_addr;
	struct tbv_rx_message rx_msg;
	struct tbv_rx_write rx_write;
	struct list_head rx_reorder;
	struct tbv_ack_history_entry ack_history[TBV_ACK_HISTORY_SIZE];
	u32 rx_reorder_count;
	u32 rx_reorder_bytes;
	struct tbv_apple_pending_rx *apple_pending;
	u32 apple_pending_slot_count;
	u32 apple_pending_head;
	u32 apple_pending_tail;
	u32 apple_pending_ready_count;
	u32 apple_pending_bytes;
	int apple_pending_active;
	u32 apple_sq_outstanding;
	bool qpn_allocated;
	bool rail_binding_counted;
	bool dest_qp_known;
	bool ack_timeout_set;
	bool early_remote_recv_credit_src_known;
	bool rx_rnr_active;
	bool closing;
	bool timeout_work_armed;
};

struct tbv_mr {
	struct ib_mr base;
	struct tbv_state *owner;
	struct ib_umem *umem;
	refcount_t refs;
	struct work_struct free_work;
	u64 start;
	u64 length;
	u64 virt_addr;
	int access;
	bool closing;
	bool dma_mr;
};

struct tbv_ah {
	struct ib_ah base;
	struct rdma_ah_attr attr;
};

struct tbv_ibdev {
	struct ib_device base;
	struct tbv_state *state;
	enum tbv_backend_type backend;
	struct net_device *netdev;
	struct tbv_qp *gsi_qp;
	/*
	 * Per-rail device identity. Apple and GSI QPs stay on this rail.
	 * Native data QPs created on this device may be assigned to sibling
	 * rails for the same peer by tbv_select_qp_rail_locked().
	 */
	struct tbv_rail *rail;
};

struct tbv_send_ctx {
	struct list_head node;
	struct list_head retry_node;
	struct tbv_qp *tqp;
	refcount_t refs;
	spinlock_t lock;
	unsigned long queued_jiffies;
	unsigned long first_queued_jiffies;
	u64 wr_id;
	struct tbv_send_segment segs[TBV_IBDEV_MAX_SGE];
	int nsegs;
	u64 remote_addr;
	u32 total_len;
	u32 rkey;
	u32 imm_data;
	u32 psn;
	enum tbv_native_data_op opcode;
	enum ib_wc_opcode wc_opcode;
	atomic_t apple_pending;
	atomic_t tx_pending;
	u8 retries;
	u8 max_retries;
	u8 rnr_retries;
	u8 max_rnr_retries;
	enum tbv_send_post_reason retry_reason;
	int completion_status;
	bool signaled;
	bool completed;
	bool ready;
	bool pending;
	bool retryable;
	bool retrying;
	bool rnr_waiting;
	bool recv_credit_required;
	bool solicited;
	bool sq_counted;
	bool apple_window_acquired;
	bool apple_window_wr_acquired;
	bool apple_sq_counted;
	u32 apple_window_frames;
	struct delayed_work apple_complete_work;
	int apple_complete_status;
};

struct tbv_read_ctx {
	struct list_head node;
	struct tbv_qp *tqp;
	refcount_t refs;
	spinlock_t lock;
	struct mutex data_lock;
	unsigned long queued_jiffies;
	u64 wr_id;
	u32 psn;
	u32 total_len;
	u32 received;
	u32 resp_buffered_bytes;
	int nsegs;
	int completion_status;
	bool signaled;
	bool completed;
	bool ready;
	bool sq_counted;
	struct list_head resp_frags;
	struct tbv_read_segment segs[TBV_IBDEV_MAX_SGE];
};

struct tbv_read_resp_ctx {
	struct list_head node;
	struct list_head retry_node;
	struct tbv_qp *tqp;
	struct tbv_mr *mr;
	struct tbv_path *rx_path;
	refcount_t refs;
	/* Stable RDMA_READ payload snapshot; retries must not re-read the MR. */
	void *data;
	u32 data_len;
	unsigned long queued_jiffies;
	u8 retries;
	bool response_sent;
	bool retrying;
	bool closing;
	struct tbv_native_data_header req;
};

struct tbv_read_req_work {
	struct work_struct work;
	struct tbv_state *state;
	struct tbv_qp *tqp;
	struct tbv_path *rx_path;
	struct tbv_native_data_header hdr;
};

struct tbv_send_page_stream {
	struct tbv_send_segment segs[TBV_IBDEV_MAX_SGE];
	struct tbv_send_ctx *send;
	refcount_t refs;
	u32 offset;
	u32 total_len;
	u32 max_chunk;
	int nsegs;
};

struct tbv_apple_send_fill {
	const void *payload;
	u32 payload_len;
	u32 crc;
	bool append_crc;
};

struct tbv_gsi_send_ctx {
	struct tbv_qp *tqp;
	struct ib_cqe *wr_cqe;
};

static DEFINE_IDA(tbv_qpn_ida);
static atomic_t tbv_mr_key = ATOMIC_INIT(1);

static int tbv_cq_push(struct tbv_cq *tcq, const struct ib_wc *wc);
static void tbv_send_ctx_put(struct tbv_send_ctx *send);
static bool tbv_send_complete(struct tbv_send_ctx *send, int status);
static void tbv_send_tx_done(void *ctx, int status);
static void tbv_apple_send_tx_done(void *ctx, int status);
static void tbv_apple_sq_work(struct work_struct *work);
static void tbv_qp_flush_apple_sq(struct tbv_qp *tqp);
static void tbv_read_ctx_put(struct tbv_read_ctx *read);
static bool tbv_read_complete(struct tbv_read_ctx *read, int status);
static void tbv_read_tx_done(void *ctx, int status);
static void tbv_read_resp_ctx_get(struct tbv_read_resp_ctx *ctx);
static void tbv_read_resp_ctx_put(struct tbv_read_resp_ctx *ctx);
static int tbv_send_read_response_ctx(struct tbv_read_resp_ctx *ctx);
static int tbv_umem_page_from_addr(struct tbv_mr *mr, u64 addr, u32 max_len,
				   struct page **page_out,
				   u32 *page_off_out, u32 *len_out);
static int tbv_rx_copy_to_wqe(struct tbv_state *state,
			      const struct tbv_recv_wqe *wqe, u32 offset,
			      const void *payload, u32 len, u32 *delivered);
static void tbv_qp_flush_reorder(struct tbv_qp *tqp);
static void tbv_qp_flush_active_rx(struct tbv_qp *tqp);
static void tbv_rx_fail_active_send(struct tbv_state *state, struct tbv_qp *tqp,
				    struct tbv_path *rx_path,
				    enum ib_wc_status status);
static void tbv_rx_fail_active_write_locked(struct tbv_state *state,
						struct tbv_qp *tqp,
						struct tbv_path *rx_path,
						enum ib_wc_status status);
static void tbv_qp_flush_error(struct tbv_qp *tqp);
static void tbv_rx_drop_reorder_msg_locked(struct tbv_state *state,
					   struct tbv_qp *tqp,
					   struct tbv_rx_reorder_msg *msg);
static void tbv_rx_drain_reorder_locked(struct tbv_state *state,
					struct tbv_qp *tqp,
					struct tbv_path *rx_path);
static void tbv_qp_flush_apple_pending(struct tbv_qp *tqp);
static void tbv_apple_rx_drain_pending_locked(struct tbv_state *state,
					      struct tbv_qp *tqp);
static void tbv_qp_advertise_recv_credits(struct tbv_qp *tqp);
static int tbv_qp_consume_remote_recv_credit(struct tbv_qp *tqp);
static void tbv_qp_return_remote_recv_credit(struct tbv_qp *tqp);
static u32 tbv_collect_native_data_paths_for_qp_locked(struct tbv_qp *tqp,
						       struct tbv_path **paths,
						       u32 max_paths);
static void tbv_release_path_refs(struct tbv_path **paths, u32 path_count);
static int tbv_send_ack(struct tbv_qp *tqp, u32 dest_qp, u32 src_qp,
			u32 psn, int status);
static int tbv_send_ack_on_path(struct tbv_qp *tqp,
				struct tbv_path *rx_path, u32 dest_qp,
				u32 src_qp, u32 psn, int status);
static int tbv_send_read_ack_on_path(struct tbv_qp *tqp,
				     struct tbv_path *rx_path, u32 dest_qp,
				     u32 src_qp, u32 psn, int status);
static int tbv_send_read_status_on_path(struct tbv_qp *tqp,
					struct tbv_path *rx_path,
					u32 dest_qp, u32 src_qp, u32 psn,
					u32 total_len, int status);
static void tbv_rx_queue_rdma_read_req_work(
	struct tbv_state *state, struct tbv_qp *tqp,
	const struct tbv_native_data_header *hdr, struct tbv_path *rx_path);
static void tbv_qp_timeout_work(struct work_struct *work);
static void tbv_release_send_segments(struct tbv_send_segment *segs,
				      int nsegs);
static int tbv_native_send_ctx_post_frames(struct tbv_send_ctx *ctx,
					   enum tbv_send_post_reason reason);

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

static bool tbv_backend_is_apple(enum tbv_backend_type backend)
{
	return backend == TBV_BACKEND_APPLE;
}

static bool tbv_qp_uses_apple_transport(const struct tbv_qp *tqp)
{
	return tqp && tbv_backend_is_apple(tqp->backend);
}

static u32 tbv_qp_max_msg_size(const struct tbv_qp *tqp)
{
	return tbv_qp_uses_apple_transport(tqp) ? TBV_APPLE_MAX_MSG_SIZE :
						  TBV_NATIVE_DATA_MAX_MSG_SIZE;
}

static bool tbv_qp_is_kernel_gsi(const struct tbv_qp *tqp)
{
	return tqp && tqp->type == IB_QPT_GSI && !tqp->base.uobject;
}

static bool tbv_qp_accepts_kernel_dma_lkey(const struct tbv_qp *tqp,
					   u32 lkey)
{
	return tbv_qp_is_kernel_gsi(tqp) &&
	       lkey == tqp->base.pd->local_dma_lkey &&
	       ib_uses_virt_dma(tqp->base.device);
}

static u32 tbv_gsi_effective_remote_qkey(const struct tbv_qp *tqp, u32 qkey)
{
	if (qkey == IB_QP_SET_QKEY)
		return tqp->attr.qkey;
	return qkey;
}

static void *tbv_kernel_dma_sge_ptr(const struct tbv_qp *tqp,
				    const struct ib_sge *sge)
{
	if (!sge || !tbv_qp_accepts_kernel_dma_lkey(tqp, sge->lkey))
		return NULL;

	return ib_virt_dma_to_ptr(sge->addr);
}

static void tbv_recv_wqe_set_wr(struct tbv_qp *tqp, struct tbv_recv_wqe *wqe,
				const struct ib_recv_wr *wr)
{
	if (!wqe || !wr)
		return;

	wqe->wr_id = wr->wr_id;
	wqe->wr_cqe = wr->wr_cqe;
	wqe->kernel_cqe = tqp && !tqp->base.uobject;
}

static void tbv_wc_set_recv_wr(struct ib_wc *wc,
			       const struct tbv_recv_wqe *wqe)
{
	if (!wc || !wqe)
		return;

	if (wqe->kernel_cqe)
		wc->wr_cqe = wqe->wr_cqe;
	else
		wc->wr_id = wqe->wr_id;
}

static u32 tbv_apple_qpn_from_path(const struct tbv_path *path)
{
	if (!path || path->cfg.receive_path < 0)
		return TBV_APPLE_PRIMARY_QPN;

	return (u32)path->cfg.receive_path << TBV_APPLE_QPN_SHIFT;
}

static int tbv_alloc_qpn(const struct tbv_state *state,
			 enum tbv_backend_type backend)
{
	if (tbv_backend_is_apple(backend))
		return ida_alloc_range(&tbv_qpn_ida, TBV_APPLE_PRIMARY_QPN,
				       TBV_APPLE_PRIMARY_QPN, GFP_KERNEL);

	return ida_alloc_range(&tbv_qpn_ida,
			       state && state->cfg.apple_enabled ?
			       TBV_IBDEV_QPN_MIN + 0x1000 :
			       TBV_IBDEV_QPN_MIN,
			       TBV_IBDEV_QPN_MAX, GFP_KERNEL);
}

static void tbv_free_qpn(enum tbv_backend_type backend, u32 qpn)
{
	/*
	 * Native QPNs are part of the wire identity. The transport can still
	 * deliver delayed frames after userspace destroys a QP, so recycling a
	 * native QPN immediately lets old frames target a new connection. Keep
	 * native QPNs allocated until module unload; failed creates free their
	 * unpublished QPN at the allocation site.
	 */
	if (tbv_backend_is_apple(backend))
		ida_free(&tbv_qpn_ida, qpn);
}

static struct tbv_ibdev *tbv_to_ibdev(struct ib_device *ibdev)
{
	return container_of(ibdev, struct tbv_ibdev, base);
}

static struct tbv_state *tbv_ibdev_state(struct ib_device *ibdev)
{
	struct tbv_ibdev *dev = tbv_to_ibdev(ibdev);

	return dev->state;
}

static enum tbv_backend_type tbv_ibdev_backend(struct ib_device *ibdev)
{
	return tbv_to_ibdev(ibdev)->backend;
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

static void tbv_mr_free(struct tbv_mr *mr)
{
	if (mr->umem)
		ib_umem_release(mr->umem);
	if (mr->owner)
		atomic_dec(&mr->owner->verbs_mrs);
	kfree(mr);
}

static void tbv_mr_free_work(struct work_struct *work)
{
	struct tbv_mr *mr = container_of(work, struct tbv_mr, free_work);

	tbv_mr_free(mr);
}

static void tbv_mr_put(struct tbv_mr *mr)
{
	if (!mr)
		return;

	if (refcount_dec_and_test(&mr->refs))
		queue_work(mr->owner && mr->owner->workqueue ?
			   mr->owner->workqueue : system_unbound_wq,
			   &mr->free_work);
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
	if (!tqp)
		return;
	if (refcount_dec_and_test(&tqp->refs))
		complete(&tqp->refs_zero);
	else
		wake_up_all(&tqp->refs_wait);
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

static bool tbv_qp_has_dest_qp(const struct tbv_qp *tqp)
{
	return READ_ONCE(tqp->dest_qp_known);
}

static unsigned long tbv_qp_fallback_timeout_jiffies(void)
{
	uint timeout_ms = READ_ONCE(qp_timeout_ms);

	return timeout_ms ? msecs_to_jiffies(timeout_ms) : 0;
}

static unsigned long tbv_verbs_ack_timeout_jiffies(u8 timeout)
{
	unsigned long delay;
	tbv_rel_u64 nsec;

	nsec = tbv_rel_ack_timeout_ns(timeout);
	delay = nsecs_to_jiffies(nsec);
	return delay ? delay : 1;
}

static unsigned long
tbv_qp_tx_timeout_jiffies_locked(const struct tbv_qp *tqp)
{
	if (tqp->type == IB_QPT_RC && tqp->ack_timeout_set)
		return tbv_verbs_ack_timeout_jiffies(tqp->attr.timeout);

	return tbv_qp_fallback_timeout_jiffies();
}

static unsigned long
tbv_qp_rx_timeout_jiffies_locked(const struct tbv_qp *tqp,
				 unsigned long tx_timeout)
{
	unsigned long rx_timeout;
	u8 retry_cnt;

	if (!tx_timeout)
		return 0;

	retry_cnt = tqp->attr.retry_cnt & 0x7;

	if (check_mul_overflow(tx_timeout, (unsigned long)retry_cnt + 1,
			       &rx_timeout))
		return MAX_JIFFY_OFFSET;

	return rx_timeout;
}

static unsigned long
tbv_qp_timeout_interval_jiffies_locked(const struct tbv_qp *tqp)
{
	unsigned long timeout = tbv_qp_tx_timeout_jiffies_locked(tqp);
	unsigned long max_interval =
		msecs_to_jiffies(TBV_QP_TIMEOUT_WORK_INTERVAL_MS);
	unsigned long read_interval = msecs_to_jiffies(TBV_READ_RESP_RETRY_MS);
	unsigned long interval;

	if (!timeout)
		return 0;
	if (!max_interval)
		max_interval = 1;
	if (!read_interval)
		read_interval = 1;

	interval = min3(timeout, max_interval, read_interval);
	return interval ? interval : 1;
}

static unsigned long tbv_send_retry_jiffies(unsigned long qp_timeout,
					    u8 max_retries)
{
	tbv_rel_u64 retry;

	if (!qp_timeout)
		return 0;

	retry = tbv_rel_retry_interval(qp_timeout, max_retries);
	return retry > MAX_JIFFY_OFFSET ? MAX_JIFFY_OFFSET :
					  (unsigned long)retry;
}

static unsigned long tbv_rnr_timer_jiffies(u8 min_rnr_timer)
{
	static const u32 rnr_timer_us[] = {
		655360, 10, 20, 30, 40, 60, 80, 120,
		160, 240, 320, 480, 640, 960, 1280, 1920,
		2560, 3840, 5120, 7680, 10240, 15360, 20480, 30720,
		40960, 61440, 81920, 122880, 163840, 245760, 327680,
		491520,
	};
	unsigned long delay;

	delay = usecs_to_jiffies(rnr_timer_us[min_t(u8, min_rnr_timer, 31)]);
	return delay ? delay : 1;
}

static unsigned long tbv_read_resp_retry_jiffies(unsigned long qp_timeout)
{
	unsigned long retry;

	if (!qp_timeout)
		return 0;

	retry = msecs_to_jiffies(TBV_READ_RESP_RETRY_MS);
	if (!retry)
		retry = 1;

	return min(retry, qp_timeout);
}

static bool tbv_qp_entry_expired(unsigned long queued, unsigned long now,
				 unsigned long timeout)
{
	return timeout && queued && time_after_eq(now, queued + timeout);
}

static void tbv_qp_schedule_timeout_delay_locked(struct tbv_qp *tqp,
						 unsigned long delay,
						 bool replace)
{
	struct workqueue_struct *wq = tqp->owner && tqp->owner->workqueue ?
				      tqp->owner->workqueue : system_wq;

	if (!delay || tqp->closing || (tqp->timeout_work_armed && !replace))
		return;

	tqp->timeout_work_armed = true;
	mod_delayed_work(wq, &tqp->timeout_work, delay);
}

static void tbv_qp_schedule_timeout_locked(struct tbv_qp *tqp)
{
	tbv_qp_schedule_timeout_delay_locked(tqp,
					     tbv_qp_timeout_interval_jiffies_locked(tqp),
					     false);
}

static void tbv_qp_schedule_timeout_now_locked(struct tbv_qp *tqp)
{
	struct workqueue_struct *wq = tqp->owner && tqp->owner->workqueue ?
				      tqp->owner->workqueue : system_wq;

	if (!tbv_qp_tx_timeout_jiffies_locked(tqp) || tqp->closing)
		return;

	tqp->timeout_work_armed = true;
	mod_delayed_work(wq, &tqp->timeout_work, 0);
}

static void tbv_qp_schedule_timeout_now(struct tbv_qp *tqp)
{
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	tbv_qp_schedule_timeout_now_locked(tqp);
	spin_unlock_irqrestore(&tqp->lock, flags);
}

static void tbv_qp_schedule_timeout(struct tbv_qp *tqp)
{
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	tbv_qp_schedule_timeout_locked(tqp);
	spin_unlock_irqrestore(&tqp->lock, flags);
}

static bool tbv_qp_mark_error(struct tbv_qp *tqp)
{
	unsigned long flags;
	bool changed = false;

	spin_lock_irqsave(&tqp->lock, flags);
	if (!tqp->closing && tqp->state != IB_QPS_ERR) {
		tqp->state = IB_QPS_ERR;
		tqp->attr.qp_state = IB_QPS_ERR;
		changed = true;
	}
	spin_unlock_irqrestore(&tqp->lock, flags);

	if (changed) {
		wake_up_all(&tqp->credit_wait);
		wake_up_all(&tqp->apple_tx_wait);
		tbv_qp_flush_error(tqp);
	}

	return changed;
}

static bool tbv_qp_allows_post(struct tbv_qp *tqp)
{
	unsigned long flags;
	bool allowed;

	spin_lock_irqsave(&tqp->lock, flags);
	allowed = !tqp->closing && tqp->state != IB_QPS_RESET &&
		  tqp->state != IB_QPS_ERR;
	spin_unlock_irqrestore(&tqp->lock, flags);
	return allowed;
}

enum tbv_rx_endpoint_status {
	TBV_RX_ENDPOINT_OK,
	TBV_RX_ENDPOINT_UNCONNECTED,
	TBV_RX_ENDPOINT_BAD_PEER,
	TBV_RX_ENDPOINT_QP_ERROR,
};

static enum tbv_rx_endpoint_status
tbv_qp_validate_native_endpoint(struct tbv_qp *tqp,
				const struct tbv_native_data_header *hdr)
{
	enum tbv_rx_endpoint_status status = TBV_RX_ENDPOINT_OK;
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	if (tqp->closing || tqp->state == IB_QPS_ERR) {
		status = TBV_RX_ENDPOINT_QP_ERROR;
	} else if (hdr->dest_qp != tqp->base.qp_num) {
		status = TBV_RX_ENDPOINT_BAD_PEER;
	} else if (!tqp->dest_qp_known) {
		status = TBV_RX_ENDPOINT_UNCONNECTED;
	} else if (hdr->src_qp != tqp->attr.dest_qp_num) {
		status = TBV_RX_ENDPOINT_BAD_PEER;
	}
	spin_unlock_irqrestore(&tqp->lock, flags);

	return status;
}

static enum tbv_rx_endpoint_status
tbv_qp_accept_recv_credit(struct tbv_qp *tqp,
			  const struct tbv_native_data_header *hdr)
{
	enum tbv_rx_endpoint_status status = TBV_RX_ENDPOINT_OK;
	unsigned long flags;
	u32 new_credits;

	spin_lock_irqsave(&tqp->lock, flags);
	if (tqp->closing || tqp->state == IB_QPS_ERR) {
		status = TBV_RX_ENDPOINT_QP_ERROR;
	} else if (hdr->dest_qp != tqp->base.qp_num) {
		status = TBV_RX_ENDPOINT_BAD_PEER;
	} else if (tqp->dest_qp_known) {
		if (hdr->src_qp != tqp->attr.dest_qp_num) {
			status = TBV_RX_ENDPOINT_BAD_PEER;
		} else if (!check_add_overflow(tqp->remote_recv_credits,
					       hdr->imm_data, &new_credits)) {
			tqp->remote_recv_credits = new_credits;
		}
	} else if (tqp->early_remote_recv_credit_src_known &&
		   tqp->early_remote_recv_credit_src_qp != hdr->src_qp) {
		status = TBV_RX_ENDPOINT_BAD_PEER;
	} else {
		tqp->early_remote_recv_credit_src_known = true;
		tqp->early_remote_recv_credit_src_qp = hdr->src_qp;
		if (!check_add_overflow(tqp->remote_recv_credits, hdr->imm_data,
					&new_credits))
			tqp->remote_recv_credits = new_credits;
	}
	spin_unlock_irqrestore(&tqp->lock, flags);

	return status;
}

static u8 tbv_qp_send_max_retries(const struct tbv_qp *tqp)
{
	return min_t(u8, tqp->attr.retry_cnt, TBV_SEND_MAX_RETRIES);
}

static u8 tbv_qp_send_max_rnr_retries(const struct tbv_qp *tqp)
{
	tbv_rel_u32 retries =
		tbv_rel_decode_verbs_rnr_retry(tqp->attr.rnr_retry);

	return retries == TBV_REL_RETRY_INFINITE ?
		       TBV_SEND_RNR_RETRIES_INFINITE :
		       (u8)retries;
}

static bool tbv_send_rnr_retries_infinite(const struct tbv_send_ctx *send)
{
	return send->max_rnr_retries == TBV_SEND_RNR_RETRIES_INFINITE;
}

static bool tbv_send_rnr_retry_allowed(const struct tbv_send_ctx *send)
{
	return tbv_send_rnr_retries_infinite(send) ||
	       send->rnr_retries < send->max_rnr_retries;
}

static bool tbv_send_rnr_retry_exhausted(const struct tbv_send_ctx *send)
{
	return !tbv_send_rnr_retries_infinite(send) &&
	       send->rnr_retries >= send->max_rnr_retries;
}

static bool tbv_send_rnr_waits_for_recv_credit(const struct tbv_send_ctx *send)
{
	return send->recv_credit_required && tbv_send_rnr_retries_infinite(send);
}

static void tbv_qp_ack_history_store_locked(struct tbv_qp *tqp, u32 psn,
					    int status)
{
	struct tbv_ack_history_entry *entry;

	entry = &tqp->ack_history[psn % TBV_ACK_HISTORY_SIZE];
	entry->psn = psn & TBV_PSN_MASK;
	entry->status = status;
	entry->valid = true;
}

static bool tbv_qp_ack_history_lookup_locked(struct tbv_qp *tqp, u32 psn,
					     int *status)
{
	const struct tbv_ack_history_entry *entry;

	entry = &tqp->ack_history[psn % TBV_ACK_HISTORY_SIZE];
	if (!entry->valid || entry->psn != (psn & TBV_PSN_MASK))
		return false;
	if (status)
		*status = entry->status;
	return true;
}

static bool tbv_rx_reack_duplicate_locked(struct tbv_state *state,
					  struct tbv_qp *tqp,
					  struct tbv_path *rx_path,
					  u32 dest_qp, u32 src_qp,
					  u32 psn)
{
	int status = 0;

	if (tbv_psn_delta(psn, tqp->rx_expected_psn) >= 0)
		return false;

	if (tbv_qp_ack_history_lookup_locked(tqp, psn, &status)) {
		atomic64_inc(&state->data_rx_duplicate_ack);
	} else {
		/*
		 * rx_expected_psn only advances after this QP has consumed the
		 * PSN. If the bounded history no longer has the exact status,
		 * success is the only non-destructive duplicate response.
		 */
		atomic64_inc(&state->data_rx_ack_history_miss);
		status = TBV_NATIVE_SEND_ACK_OK;
	}
	tbv_send_ack_on_path(tqp, rx_path, dest_qp, src_qp, psn, status);
	return true;
}

static void tbv_send_mark_queued(struct tbv_send_ctx *send,
				 unsigned long now)
{
	send->queued_jiffies = now;
	if (!send->first_queued_jiffies)
		send->first_queued_jiffies = now;
}

static int tbv_qp_reserve_sendq(struct tbv_qp *tqp)
{
	unsigned long flags;
	u32 max_wr;
	int ret = 0;

	spin_lock_irqsave(&tqp->lock, flags);
	max_wr = tqp->init_attr.cap.max_send_wr;
	if (tqp->closing || tqp->state == IB_QPS_RESET ||
	    tqp->state == IB_QPS_ERR) {
		ret = -EINVAL;
	} else if (tqp->sendq_count >= max_wr) {
		ret = -ENOMEM;
	} else {
		tqp->sendq_count++;
	}
	spin_unlock_irqrestore(&tqp->lock, flags);
	return ret;
}

static void tbv_qp_release_sendq_counted_locked(struct tbv_qp *tqp,
						bool *counted)
{
	if (!*counted)
		return;
	if (WARN_ON_ONCE(!tqp->sendq_count))
		tqp->sendq_count = 0;
	else
		tqp->sendq_count--;
	*counted = false;
}

static void tbv_qp_release_sendq_counted(struct tbv_qp *tqp, bool *counted)
{
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	tbv_qp_release_sendq_counted_locked(tqp, counted);
	spin_unlock_irqrestore(&tqp->lock, flags);
}

static void tbv_qp_queue_send(struct tbv_qp *tqp, struct tbv_send_ctx *send)
{
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	send->queued_jiffies = 0;
	send->first_queued_jiffies = 0;
	send->pending = true;
	send->retrying = false;
	send->rnr_waiting = false;
	send->retry_reason = TBV_SEND_POST_INITIAL;
	list_add_tail(&send->node, &tqp->pending_sends);
	spin_unlock_irqrestore(&tqp->lock, flags);
}

static void tbv_qp_queue_read(struct tbv_qp *tqp, struct tbv_read_ctx *read)
{
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	read->queued_jiffies = 0;
	list_add_tail(&read->node, &tqp->pending_reads);
	spin_unlock_irqrestore(&tqp->lock, flags);
}

static void tbv_qp_queue_read_resp(struct tbv_qp *tqp,
				   struct tbv_read_resp_ctx *ctx)
{
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	ctx->queued_jiffies = 0;
	tbv_read_resp_ctx_get(ctx);
	list_add_tail(&ctx->node, &tqp->pending_read_resps);
	spin_unlock_irqrestore(&tqp->lock, flags);
}

static void tbv_qp_arm_send_timeout(struct tbv_qp *tqp,
				    struct tbv_send_ctx *send)
{
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	if (send->pending && !send->queued_jiffies) {
		tbv_send_mark_queued(send, jiffies);
		tbv_qp_schedule_timeout_locked(tqp);
	}
	spin_unlock_irqrestore(&tqp->lock, flags);
}

static void tbv_qp_arm_read_timeout(struct tbv_qp *tqp,
				    struct tbv_read_ctx *read)
{
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	if (!list_empty(&read->node) && !read->queued_jiffies) {
		read->queued_jiffies = jiffies;
		tbv_qp_schedule_timeout_locked(tqp);
	}
	spin_unlock_irqrestore(&tqp->lock, flags);
}

static void tbv_qp_arm_read_resp_timeout(struct tbv_qp *tqp,
					 struct tbv_read_resp_ctx *ctx)
{
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	if (!list_empty(&ctx->node) && !ctx->queued_jiffies) {
		ctx->queued_jiffies = jiffies;
		tbv_qp_schedule_timeout_locked(tqp);
	}
	spin_unlock_irqrestore(&tqp->lock, flags);
}

static void tbv_qp_note_read_resp_sent(struct tbv_qp *tqp,
				       struct tbv_read_resp_ctx *ctx)
{
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	if (!list_empty(&ctx->node) && !ctx->closing) {
		ctx->response_sent = true;
		ctx->queued_jiffies = jiffies;
		tbv_qp_schedule_timeout_locked(tqp);
	}
	spin_unlock_irqrestore(&tqp->lock, flags);
}

static bool tbv_qp_unqueue_send(struct tbv_qp *tqp, struct tbv_send_ctx *send)
{
	struct tbv_send_ctx *pos;
	unsigned long flags;
	bool found = false;

	spin_lock_irqsave(&tqp->lock, flags);
	list_for_each_entry(pos, &tqp->pending_sends, node) {
		if (pos != send)
			continue;
		list_del_init(&send->node);
		send->pending = false;
		send->retrying = false;
		send->rnr_waiting = false;
		tbv_qp_release_sendq_counted_locked(tqp, &send->sq_counted);
		found = true;
		break;
	}
	spin_unlock_irqrestore(&tqp->lock, flags);
	return found;
}

static void tbv_qp_drain_ready_sends_locked(struct tbv_qp *tqp,
					    struct list_head *complete)
{
	struct tbv_send_ctx *send;

	while (!list_empty(&tqp->pending_sends)) {
		send = list_first_entry(&tqp->pending_sends,
					struct tbv_send_ctx, node);
		if (!send->ready)
			break;
		list_move_tail(&send->node, complete);
		send->pending = false;
		send->retrying = false;
		send->rnr_waiting = false;
		tbv_qp_release_sendq_counted_locked(tqp, &send->sq_counted);
	}
}

static bool tbv_qp_complete_send_ordered(struct tbv_qp *tqp, u32 psn,
					 int status, struct list_head *complete,
					 struct tbv_send_ctx **matched_out)
{
	struct tbv_send_ctx *send;
	unsigned long flags;
	bool found = false;

	if (matched_out)
		*matched_out = NULL;

	spin_lock_irqsave(&tqp->lock, flags);
	list_for_each_entry(send, &tqp->pending_sends, node) {
		if (send->psn != (psn & TBV_PSN_MASK))
			continue;

		found = true;
		if (matched_out && refcount_inc_not_zero(&send->refs))
			*matched_out = send;
		if (!send->ready) {
			send->ready = true;
			send->completion_status = status;
			send->retrying = false;
			send->rnr_waiting = false;
		}
		break;
	}
	if (!found) {
		spin_unlock_irqrestore(&tqp->lock, flags);
		return false;
	}

	tbv_qp_drain_ready_sends_locked(tqp, complete);
	spin_unlock_irqrestore(&tqp->lock, flags);
	return true;
}

static bool tbv_qp_unqueue_read(struct tbv_qp *tqp, struct tbv_read_ctx *read)
{
	struct tbv_read_ctx *pos;
	unsigned long flags;
	bool found = false;

	spin_lock_irqsave(&tqp->lock, flags);
	list_for_each_entry(pos, &tqp->pending_reads, node) {
		if (pos != read)
			continue;
		list_del_init(&read->node);
		tbv_qp_release_sendq_counted_locked(tqp, &read->sq_counted);
		found = true;
		break;
	}
	spin_unlock_irqrestore(&tqp->lock, flags);
	return found;
}

static bool tbv_qp_complete_read_ordered(struct tbv_qp *tqp,
					 struct tbv_read_ctx *read,
					 int status)
{
	LIST_HEAD(complete);
	struct tbv_read_ctx *pos;
	unsigned long flags;
	bool found = false;

	spin_lock_irqsave(&tqp->lock, flags);
	list_for_each_entry(pos, &tqp->pending_reads, node) {
		if (pos != read)
			continue;
		if (!read->ready) {
			read->ready = true;
			read->completion_status = status;
		}
		found = true;
		break;
	}
	if (!found) {
		spin_unlock_irqrestore(&tqp->lock, flags);
		return false;
	}

	while (!list_empty(&tqp->pending_reads)) {
		pos = list_first_entry(&tqp->pending_reads,
				       struct tbv_read_ctx, node);
		if (!pos->ready)
			break;
		list_move_tail(&pos->node, &complete);
		tbv_qp_release_sendq_counted_locked(tqp, &pos->sq_counted);
	}
	spin_unlock_irqrestore(&tqp->lock, flags);

	while (!list_empty(&complete)) {
		pos = list_first_entry(&complete, struct tbv_read_ctx, node);

		list_del_init(&pos->node);
		tbv_read_complete(pos, pos->completion_status);
		tbv_read_ctx_put(pos);
	}
	return true;
}

static bool tbv_qp_note_rnr_ack(struct tbv_qp *tqp, u32 psn,
				struct list_head *complete,
				struct tbv_send_ctx **matched_out)
{
	struct tbv_send_ctx *send, *tmp;
	unsigned long flags;
	unsigned long delay;
	bool matched = false;

	if (matched_out)
		*matched_out = NULL;

	spin_lock_irqsave(&tqp->lock, flags);
	delay = tbv_rnr_timer_jiffies(tqp->attr.min_rnr_timer);
	list_for_each_entry_safe(send, tmp, &tqp->pending_sends, node) {
		if (send->psn != (psn & TBV_PSN_MASK))
			continue;

		matched = true;
		if (matched_out && refcount_inc_not_zero(&send->refs))
			*matched_out = send;
		if (tbv_send_rnr_retry_exhausted(send) || tqp->closing ||
		    tqp->state == IB_QPS_ERR) {
			if (!send->ready) {
				send->ready = true;
				send->completion_status = -EAGAIN;
				send->retrying = false;
				send->rnr_waiting = false;
			}
			tbv_qp_drain_ready_sends_locked(tqp, complete);
			break;
		}

		send->rnr_waiting = true;
		send->retrying = false;
		tbv_send_mark_queued(send, jiffies);
		if (tbv_send_rnr_waits_for_recv_credit(send)) {
			if (tqp->remote_recv_credits)
				tbv_qp_schedule_timeout_now_locked(tqp);
		} else {
			tbv_qp_schedule_timeout_delay_locked(tqp, delay, true);
		}
		break;
	}
	spin_unlock_irqrestore(&tqp->lock, flags);
	return matched;
}

static bool tbv_qp_send_retry_pending(struct tbv_qp *tqp,
				      struct tbv_send_ctx *send)
{
	unsigned long flags;
	bool pending;

	spin_lock_irqsave(&tqp->lock, flags);
	pending = send->pending && !tqp->closing && tqp->state != IB_QPS_ERR &&
		  !send->rnr_waiting && !atomic_read(&send->tx_pending);
	if (!pending)
		send->retrying = false;
	spin_unlock_irqrestore(&tqp->lock, flags);
	return pending;
}

static bool tbv_qp_ack_is_late_duplicate(struct tbv_qp *tqp, u32 psn)
{
	unsigned long flags;
	bool late;

	spin_lock_irqsave(&tqp->lock, flags);
	late = tbv_psn_delta(psn & TBV_PSN_MASK, tqp->send_psn) < 0;
	spin_unlock_irqrestore(&tqp->lock, flags);
	return late;
}

static struct tbv_read_ctx *tbv_qp_find_read_get(struct tbv_qp *tqp, u32 psn)
{
	struct tbv_read_ctx *read, *found = NULL;
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	list_for_each_entry(read, &tqp->pending_reads, node) {
		if (read->psn != psn)
			continue;
		if (refcount_inc_not_zero(&read->refs))
			found = read;
		break;
	}
	spin_unlock_irqrestore(&tqp->lock, flags);
	return found;
}

static struct tbv_read_resp_ctx *tbv_qp_take_read_resp(struct tbv_qp *tqp,
						       u32 psn)
{
	struct tbv_read_resp_ctx *ctx, *found = NULL;
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	list_for_each_entry(ctx, &tqp->pending_read_resps, node) {
		if (ctx->req.psn != psn)
			continue;
		list_del_init(&ctx->node);
		ctx->closing = true;
		found = ctx;
		break;
	}
	spin_unlock_irqrestore(&tqp->lock, flags);
	return found;
}

static bool tbv_qp_retry_read_resp(struct tbv_qp *tqp, u32 psn)
{
	struct tbv_read_resp_ctx *ctx;
	unsigned long flags;
	unsigned long timeout;
	bool found = false;

	spin_lock_irqsave(&tqp->lock, flags);
	list_for_each_entry(ctx, &tqp->pending_read_resps, node) {
		if (ctx->req.psn != psn)
			continue;
		if (!ctx->closing) {
			timeout = tbv_read_resp_retry_jiffies(
				tbv_qp_tx_timeout_jiffies_locked(tqp));
			ctx->queued_jiffies = timeout ?
					      jiffies - timeout : 1;
			tbv_qp_schedule_timeout_now_locked(tqp);
			found = true;
		}
		break;
	}
	spin_unlock_irqrestore(&tqp->lock, flags);
	return found;
}

static void tbv_qp_cancel_read_resps(struct tbv_qp *tqp, struct list_head *flush)
{
	struct tbv_read_resp_ctx *ctx;
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	list_for_each_entry(ctx, &tqp->pending_read_resps, node)
		ctx->closing = true;
	list_splice_init(&tqp->pending_read_resps, flush);
	spin_unlock_irqrestore(&tqp->lock, flags);
}

static void tbv_qp_flush_sends(struct tbv_qp *tqp, struct list_head *flush)
{
	struct tbv_send_ctx *send;
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	list_for_each_entry(send, &tqp->pending_sends, node) {
		if (!send->ready) {
			send->ready = true;
			send->completion_status = -ECANCELED;
		}
		send->pending = false;
		send->retrying = false;
		send->rnr_waiting = false;
		tbv_qp_release_sendq_counted_locked(tqp, &send->sq_counted);
	}
	list_splice_init(&tqp->pending_sends, flush);
	spin_unlock_irqrestore(&tqp->lock, flags);
}

static void tbv_qp_flush_reads(struct tbv_qp *tqp, struct list_head *flush)
{
	unsigned long flags;
	struct tbv_read_ctx *read;

	spin_lock_irqsave(&tqp->lock, flags);
	list_for_each_entry(read, &tqp->pending_reads, node)
		tbv_qp_release_sendq_counted_locked(tqp, &read->sq_counted);
	list_splice_init(&tqp->pending_reads, flush);
	spin_unlock_irqrestore(&tqp->lock, flags);
}

static void tbv_cancel_send_ctx_packets(struct tbv_send_ctx *send)
{
	struct tbv_qp *tqp;
	struct tbv_path *paths[TBV_NATIVE_MAX_LANES] = {};
	u32 path_count = 0;
	u32 i;

	if (!send || !send->tqp || !send->tqp->rail)
		return;

	tqp = send->tqp;
	if (!tqp->owner->native_fragment_striping ||
	    tbv_qp_uses_apple_transport(tqp) ||
	    tqp->rail->peer->backend != TBV_BACKEND_NATIVE) {
		/*
		 * Without native SEND striping, a send_ctx's QP is pinned to
		 * exactly one rail and that rail's path is the only place this
		 * send_ctx could have been queued for TX.
		 */
		tbv_path_cancel_data_owner_ctx(&tqp->rail->path, send);
		return;
	}

	mutex_lock(&tqp->owner->lock);
	path_count = tbv_collect_native_data_paths_for_qp_locked(
		tqp, paths, ARRAY_SIZE(paths));
	mutex_unlock(&tqp->owner->lock);

	if (!path_count) {
		tbv_path_cancel_data_owner_ctx(&tqp->rail->path, send);
		return;
	}

	for (i = 0; i < path_count; i++)
		tbv_path_cancel_data_owner_ctx(paths[i], send);
	tbv_release_path_refs(paths, path_count);
}

static void tbv_cancel_read_ctx_packets(struct tbv_read_ctx *read)
{
	/* See tbv_cancel_send_ctx_packets() for the per-rail invariant. */
	if (!read || !read->tqp || !read->tqp->rail)
		return;
	if (read->tqp->rail->peer->backend != TBV_BACKEND_NATIVE)
		return;
	tbv_path_cancel_data_done_ctx(&read->tqp->rail->path,
				      tbv_read_tx_done, read);
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

static u32 tbv_rail_pending_data_frames(struct tbv_rail *rail)
{
	struct tbv_path *path = &rail->path;
	s64 pending;

	pending = atomic64_read(&path->data_tx_enqueued) -
		  atomic64_read(&path->data_tx_completed);
	if (pending <= 0)
		return 0;
	if (pending > U32_MAX)
		return U32_MAX;
	return (u32)pending;
}

static u32 tbv_qp_rr_distance(u32 rail_id, u32 next_rail_id)
{
	if (rail_id >= next_rail_id)
		return rail_id - next_rail_id;
	return U32_MAX - next_rail_id + 1 + rail_id;
}

static struct tbv_rail *
tbv_select_qp_rail_locked(struct tbv_ibdev *dev, enum tbv_backend_type backend,
			  bool gsi, bool *counted)
{
	struct tbv_rail *home = dev->rail;
	struct tbv_peer *peer;
	struct tbv_rail *rail;
	struct tbv_rail *best = NULL;
	u32 best_qps = U32_MAX;
	u32 best_pending = U32_MAX;
	u32 best_rr_distance = U32_MAX;
	u32 best_rail_id = U32_MAX;
	u32 next_rail_id;

	*counted = false;
	if (WARN_ON_ONCE(!home))
		return NULL;
	if (home->removing)
		return NULL;

	if (backend != TBV_BACKEND_NATIVE || gsi) {
		refcount_inc(&home->refcnt);
		return home;
	}

	peer = home->peer;
	if (!peer || peer->backend != TBV_BACKEND_NATIVE)
		return NULL;

	next_rail_id = peer->native_qp_rr_rail_id;
	list_for_each_entry(rail, &peer->rails, node) {
		u32 qps;
		u32 pending;
		u32 rr_distance;

		if (!tbv_rail_data_ready(rail) || rail->removing)
			continue;
		if (!READ_ONCE(rail->ibdev))
			continue;

		qps = max_t(int, atomic_read(&rail->native_qp_bind_count), 0);
		pending = tbv_rail_pending_data_frames(rail);
		rr_distance = tbv_qp_rr_distance(rail->rail_id, next_rail_id);
		if (!best || qps < best_qps ||
		    (qps == best_qps &&
		     (pending < best_pending ||
		      (pending == best_pending &&
		       (rr_distance < best_rr_distance ||
			(rr_distance == best_rr_distance &&
			 rail->rail_id < best_rail_id)))))) {
			best = rail;
			best_qps = qps;
			best_pending = pending;
			best_rr_distance = rr_distance;
			best_rail_id = rail->rail_id;
		}
	}

	if (!best)
		return NULL;

	refcount_inc(&best->refcnt);
	atomic_inc(&best->native_qp_bind_count);
	peer->native_qp_rr_rail_id = best->rail_id + 1;
	*counted = true;
	return best;
}

static void tbv_qp_unbind_rail(struct tbv_qp *tqp)
{
	if (!tqp->rail)
		return;

	if (tqp->rail_binding_counted) {
		atomic_dec(&tqp->rail->native_qp_bind_count);
		tqp->rail_binding_counted = false;
	}
	tbv_rail_put(tqp->rail);
	tqp->rail = NULL;
}

static u32 tbv_collect_native_data_paths_for_qp_locked(struct tbv_qp *tqp,
						       struct tbv_path **paths,
						       u32 max_paths)
{
	struct tbv_peer *peer;
	struct tbv_rail *rail;
	u32 count = 0;

	if (!tqp->rail || max_paths == 0)
		return 0;
	peer = tqp->rail->peer;
	if (!peer || peer->backend != TBV_BACKEND_NATIVE)
		return 0;

	list_for_each_entry(rail, &peer->rails, node) {
		if (count == max_paths)
			break;
		if (!tbv_rail_get_data_ready_locked(rail))
			continue;
		paths[count++] = &rail->path;
	}

	return count;
}

static struct tbv_path *
tbv_select_native_data_path_for_qp_locked(struct tbv_qp *tqp)
{
	struct tbv_rail *rail = tqp->rail;

	if (rail->peer->backend != TBV_BACKEND_NATIVE)
		return NULL;
	if (!tbv_rail_get_data_ready_locked(rail))
		return NULL;
	return &rail->path;
}

static void tbv_native_control_path_score(struct tbv_path *path,
					  u32 *data_score, u32 *control_score)
{
	unsigned long flags;
	s64 data_pending;
	s64 control_pending;
	u32 reserved;

	data_pending = atomic64_read(&path->data_tx_enqueued) -
		       atomic64_read(&path->data_tx_completed);
	control_pending = atomic64_read(&path->control_tx_enqueued) -
			  atomic64_read(&path->control_tx_completed);

	spin_lock_irqsave(&path->tx_lock, flags);
	reserved = path->tx_data_reserved;
	spin_unlock_irqrestore(&path->tx_lock, flags);

	if (data_pending < 0)
		data_pending = 0;
	if (control_pending < 0)
		control_pending = 0;

	*data_score = data_pending > U32_MAX - reserved ?
		      U32_MAX : (u32)data_pending + reserved;
	*control_score = control_pending > U32_MAX ?
			 U32_MAX : (u32)control_pending;
}

static struct tbv_path *
tbv_select_native_control_path_for_qp_locked(struct tbv_qp *tqp,
					     struct tbv_path *rx_path)
{
	struct tbv_peer *peer = tqp->rail->peer;
	struct tbv_rail *rail;
	struct tbv_rail *best = NULL;
	u32 best_data_score = U32_MAX;
	u32 best_control_score = U32_MAX;

	if (peer->backend != TBV_BACKEND_NATIVE)
		return NULL;

	list_for_each_entry(rail, &peer->rails, node) {
		u32 data_score;
		u32 control_score;

		if (!tbv_rail_get_data_ready_locked(rail))
			continue;

		tbv_native_control_path_score(&rail->path, &data_score,
					      &control_score);
		if (!best || data_score < best_data_score ||
		    (data_score == best_data_score &&
		     (control_score < best_control_score ||
		      (control_score == best_control_score && rx_path &&
		       &best->path == rx_path && &rail->path != rx_path)))) {
			if (best)
				tbv_rail_put(best);
			best = rail;
			best_data_score = data_score;
			best_control_score = control_score;
			continue;
		}

		tbv_rail_put(rail);
	}

	return best ? &best->path : NULL;
}

static struct tbv_path *
tbv_first_active_apple_path_for_qp_locked(struct tbv_qp *tqp)
{
	struct tbv_rail *rail = tqp->rail;

	if (rail->peer->backend != TBV_BACKEND_APPLE)
		return NULL;
	if (!tbv_rail_get_apple_data_ready_locked(rail))
		return NULL;
	return &rail->path;
}

static struct tbv_qp *tbv_path_get_gsi_qp(struct tbv_path *path)
{
	struct tbv_state *state;
	struct tbv_ibdev *dev;
	struct tbv_qp *tqp = NULL;

	if (!path || !path->rail)
		return NULL;

	state = path->rail->peer->state;
	mutex_lock(&state->lock);
	dev = path->rail->ibdev;
	if (dev && dev->gsi_qp && tbv_qp_get_live(dev->gsi_qp))
		tqp = dev->gsi_qp;
	mutex_unlock(&state->lock);

	return tqp;
}

static void tbv_release_path_refs(struct tbv_path **paths, u32 path_count)
{
	u32 i;

	/* Per-rail invariant: every path is owned by exactly one rail. */
	for (i = 0; i < path_count; i++) {
		if (paths[i])
			tbv_rail_put(paths[i]->rail);
	}
}

static struct tbv_path *tbv_path_get_for_response(struct tbv_path *path)
{
	struct tbv_rail *rail;

	if (!path)
		return NULL;

	/* Per-rail invariant: every path is owned by exactly one rail. */
	rail = path->rail;
	if (!refcount_inc_not_zero(&rail->refcnt))
		return NULL;

	if (!tbv_rail_data_ready(rail) || rail->removing) {
		tbv_rail_put(rail);
		return NULL;
	}

	return path;
}

static void tbv_path_put_response(struct tbv_path *path)
{
	if (path)
		tbv_rail_put(path->rail);
}

static struct tbv_path *
tbv_select_read_response_path(struct tbv_state *state, struct tbv_qp *tqp,
			      struct tbv_path *rx_path, bool *selected_ref)
{
	struct tbv_path *path;

	*selected_ref = false;

	if (rx_path && tbv_rail_data_ready(rx_path->rail) &&
	    !rx_path->rail->removing)
		return rx_path;

	mutex_lock(&state->lock);
	path = tbv_select_native_data_path_for_qp_locked(tqp);
	mutex_unlock(&state->lock);
	if (path)
		*selected_ref = true;
	return path;
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

static void tbv_release_owned_frame_lists(struct list_head *lists, u32 count)
{
	u32 i;

	for (i = 0; i < count; i++) {
		while (!list_empty(&lists[i])) {
			struct tbv_path_owned_frame *frame =
				list_first_entry(&lists[i],
						 struct tbv_path_owned_frame,
						 node);

			list_del_init(&frame->node);
			kfree(frame->data);
			kfree(frame);
		}
	}
}

static void tbv_release_prepared_packet_lists(struct list_head *lists,
					      u32 count, int status)
{
	u32 i;

	for (i = 0; i < count; i++)
		tbv_path_release_prepared_list_silent(&lists[i], status);
}

static bool tbv_ibdev_port_active(struct tbv_ibdev *dev)
{
	struct tbv_rail *rail = dev->rail;
	bool active;

	/*
	 * Per-rail ib_device: port state must reflect *this* rail only,
	 * never aggregate over sibling rails. Otherwise a device pinned to a
	 * dead lane could report ACTIVE because some other lane on the same
	 * backend is still up, which lies to userspace and to RCCL/UCX.
	 */
	mutex_lock(&dev->state->lock);
	active = !rail->removing &&
		 (rail->peer->backend == TBV_BACKEND_APPLE ?
			tbv_rail_apple_data_ready(rail) :
			tbv_rail_data_ready(rail));
	mutex_unlock(&dev->state->lock);
	return active;
}

static int tbv_query_device(struct ib_device *ibdev,
			    struct ib_device_attr *attr,
			    struct ib_udata *udata)
{
	bool apple = tbv_backend_is_apple(tbv_ibdev_backend(ibdev));

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
	bool apple = tbv_backend_is_apple(dev->backend);
	bool active;

	if (port_num != 1)
		return -EINVAL;

	memset(attr, 0, sizeof(*attr));
	active = tbv_ibdev_port_active(dev);
	attr->state = active ? IB_PORT_ACTIVE : IB_PORT_DOWN;
	attr->phys_state = active ? IB_PORT_PHYS_STATE_LINK_UP :
				    IB_PORT_PHYS_STATE_DISABLED;
	attr->max_mtu = IB_MTU_4096;
	attr->active_mtu = IB_MTU_4096;
	attr->max_msg_sz = apple ? TBV_APPLE_MAX_MSG_SIZE :
				   TBV_NATIVE_DATA_MAX_MSG_SIZE;
	attr->lid = apple ? 1 : 0;
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
	int ret = 0;

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

static const char *tbv_ibdev_netdev_name_for(struct tbv_state *state,
					     enum tbv_backend_type backend)
{
	const char *name = roce_netdev;

	if (tbv_backend_is_apple(backend) &&
	    state->tbnet_identity.gid_netdev_name[0])
		name = state->tbnet_identity.gid_netdev_name;

	if (!name || !*name)
		return NULL;

	return name;
}

static const char *tbv_ibdev_netdev_name(const struct tbv_ibdev *dev)
{
	return tbv_ibdev_netdev_name_for(dev->state, dev->backend);
}

static int tbv_ibdev_attach_netdev(struct tbv_ibdev *dev)
{
	const char *name = tbv_ibdev_netdev_name(dev);
	struct net_device *ndev;
	int ret = 0;

	if (!name)
		return 0;

	ndev = dev_get_by_name(&init_net, name);
	if (!ndev)
		return -EPROBE_DEFER;

	if (ndev->reg_state != NETREG_REGISTERED) {
		dev_put(ndev);
		return -EPROBE_DEFER;
	}

	ret = ib_device_set_netdev(&dev->base, ndev, 1);
	if (ret) {
		dev_put(ndev);
		return ret;
	}

	dev->netdev = ndev;
	dev_put(ndev);
	return 0;
}

static void tbv_ibdev_detach_netdev(struct tbv_ibdev *dev)
{
	if (!dev->netdev)
		return;

	ib_device_set_netdev(&dev->base, NULL, 1);
	dev->netdev = NULL;
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

static int tbv_create_ah(struct ib_ah *ah, struct rdma_ah_init_attr *attr,
			 struct ib_udata *udata)
{
	struct tbv_ah *tah = container_of(ah, struct tbv_ah, base);
	const struct rdma_ah_attr *ah_attr;

	if (!attr || !attr->ah_attr || udata)
		return -EOPNOTSUPP;

	ah_attr = attr->ah_attr;
	if (rdma_ah_get_port_num(ah_attr) != 1 ||
	    ah_attr->type != RDMA_AH_ATTR_TYPE_ROCE ||
	    !(rdma_ah_get_ah_flags(ah_attr) & IB_AH_GRH))
		return -EINVAL;

	tah->attr = *ah_attr;
	tah->attr.grh.sgid_attr = NULL;
	return 0;
}

static int tbv_modify_ah(struct ib_ah *ah, struct rdma_ah_attr *ah_attr)
{
	struct tbv_ah *tah = container_of(ah, struct tbv_ah, base);

	if (!ah_attr ||
	    rdma_ah_get_port_num(ah_attr) != 1 ||
	    ah_attr->type != RDMA_AH_ATTR_TYPE_ROCE ||
	    !(rdma_ah_get_ah_flags(ah_attr) & IB_AH_GRH))
		return -EINVAL;

	tah->attr = *ah_attr;
	tah->attr.grh.sgid_attr = NULL;
	return 0;
}

static int tbv_query_ah(struct ib_ah *ah, struct rdma_ah_attr *ah_attr)
{
	struct tbv_ah *tah = container_of(ah, struct tbv_ah, base);

	if (!ah_attr)
		return -EINVAL;

	*ah_attr = tah->attr;
	ah_attr->grh.sgid_attr = NULL;
	return 0;
}

static int tbv_destroy_ah(struct ib_ah *ah, u32 flags)
{
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
	struct tbv_ibdev *dev = tbv_to_ibdev(qp->device);
	unsigned long flags;
	bool gsi;
	int qpn;
	int ret;

	if (!init_attr || init_attr->srq)
		return -EOPNOTSUPP;
	gsi = init_attr->qp_type == IB_QPT_GSI;
	if (init_attr->qp_type != IB_QPT_RC &&
	    init_attr->qp_type != IB_QPT_UC &&
	    init_attr->qp_type != IB_QPT_GSI)
		return -EOPNOTSUPP;
	tqp->backend = tbv_ibdev_backend(qp->device);
	if (tbv_backend_is_apple(tqp->backend) &&
	    init_attr->qp_type != IB_QPT_UC)
		return -EOPNOTSUPP;
	if (gsi && (udata || init_attr->port_num != 1))
		return -EOPNOTSUPP;

	mutex_lock(&state->lock);
	tqp->rail = tbv_select_qp_rail_locked(dev, tqp->backend, gsi,
					      &tqp->rail_binding_counted);
	if (!tqp->rail) {
		mutex_unlock(&state->lock);
		return -ENOTCONN;
	}
	mutex_unlock(&state->lock);
	if (init_attr->cap.max_send_wr > TBV_IBDEV_MAX_QP_WR ||
	    init_attr->cap.max_recv_wr > TBV_IBDEV_MAX_QP_WR ||
	    init_attr->cap.max_send_sge > TBV_IBDEV_MAX_SGE ||
	    init_attr->cap.max_recv_sge > TBV_IBDEV_MAX_SGE) {
		ret = -EINVAL;
		goto err_put_rail;
	}

	if (gsi) {
		qpn = TBV_GSI_QPN;
	} else {
		qpn = tbv_alloc_qpn(state, tqp->backend);
		if (qpn < 0) {
			ret = qpn;
			goto err_put_rail;
		}
	}

	if (init_attr->cap.max_recv_wr) {
		tqp->recvq = kcalloc(init_attr->cap.max_recv_wr,
				     sizeof(*tqp->recvq), GFP_KERNEL);
		if (!tqp->recvq) {
			if (!gsi)
				ida_free(&tbv_qpn_ida, qpn);
			ret = -ENOMEM;
			goto err_put_rail;
		}
		tqp->recvq_size = init_attr->cap.max_recv_wr;
	}

	if (tbv_backend_is_apple(tqp->backend)) {
		u32 slots = min_t(u32, READ_ONCE(apple_rx_pending_slots),
				  TBV_APPLE_PENDING_RX_MAX_SLOTS);

		if (!slots || !READ_ONCE(apple_rx_pending_bytes) ||
		    !READ_ONCE(apple_rx_pending_total_bytes)) {
			kfree(tqp->recvq);
			if (!gsi)
				ida_free(&tbv_qpn_ida, qpn);
			ret = -EINVAL;
			goto err_put_rail;
		}

		tqp->apple_pending =
			kvcalloc(slots, sizeof(*tqp->apple_pending),
				 GFP_KERNEL);
		if (!tqp->apple_pending) {
			kfree(tqp->recvq);
			if (!gsi)
				ida_free(&tbv_qpn_ida, qpn);
			ret = -ENOMEM;
			goto err_put_rail;
		}
		tqp->apple_pending_slot_count = slots;
	}

	tqp->init_attr = *init_attr;
	tqp->owner = state;
	spin_lock_init(&tqp->lock);
	mutex_init(&tqp->rx_lock);
	init_waitqueue_head(&tqp->credit_wait);
	init_waitqueue_head(&tqp->apple_tx_wait);
	init_waitqueue_head(&tqp->refs_wait);
	refcount_set(&tqp->refs, 1);
	atomic_set(&tqp->apple_tx_inflight, 0);
	atomic_set(&tqp->apple_tx_inflight_frames, 0);
	init_completion(&tqp->refs_zero);
	INIT_LIST_HEAD(&tqp->pending_sends);
	INIT_LIST_HEAD(&tqp->pending_reads);
	INIT_LIST_HEAD(&tqp->pending_read_resps);
	INIT_LIST_HEAD(&tqp->apple_sq);
	INIT_LIST_HEAD(&tqp->rx_reorder);
	INIT_WORK(&tqp->apple_sq_work, tbv_apple_sq_work);
	INIT_DELAYED_WORK(&tqp->timeout_work, tbv_qp_timeout_work);
	tqp->apple_pending_active = -1;
	tqp->state = IB_QPS_RESET;
	tqp->type = init_attr->qp_type;
	tqp->qpn_allocated = !gsi;
	qp->qp_num = qpn;
	init_attr->cap.max_inline_data = 0;
	if (!gsi) {
		xa_lock_irqsave(&tqp->owner->verbs_qps_xa, flags);
		ret = __xa_insert(&tqp->owner->verbs_qps_xa, qpn, tqp,
				  GFP_KERNEL);
		xa_unlock_irqrestore(&tqp->owner->verbs_qps_xa, flags);
		if (ret) {
			kvfree(tqp->apple_pending);
			kfree(tqp->recvq);
			ida_free(&tbv_qpn_ida, qpn);
			tqp->qpn_allocated = false;
			goto err_put_rail;
		}
	} else {
		mutex_lock(&state->lock);
		if (dev->gsi_qp) {
			mutex_unlock(&state->lock);
			kvfree(tqp->apple_pending);
			kfree(tqp->recvq);
			ret = -EBUSY;
			goto err_put_rail;
		}
		dev->gsi_qp = tqp;
		mutex_unlock(&state->lock);
	}
	atomic_inc(&tqp->owner->verbs_qps);
	return 0;

err_put_rail:
	tbv_qp_unbind_rail(tqp);
	return ret;
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

	if (tqp->type == IB_QPT_GSI && tqp->rail && tqp->rail->ibdev) {
		mutex_lock(&tqp->owner->lock);
		if (tqp->rail->ibdev->gsi_qp == tqp)
			tqp->rail->ibdev->gsi_qp = NULL;
		mutex_unlock(&tqp->owner->lock);
	}

	wake_up_all(&tqp->credit_wait);
	wake_up_all(&tqp->apple_tx_wait);
	cancel_work_sync(&tqp->apple_sq_work);
	cancel_delayed_work_sync(&tqp->timeout_work);
	spin_lock_irqsave(&tqp->lock, flags);
	tqp->timeout_work_armed = false;
	spin_unlock_irqrestore(&tqp->lock, flags);

	tbv_qp_flush_apple_sq(tqp);
	tbv_qp_flush_sends(tqp, &flush);
	while (!list_empty(&flush)) {
		struct tbv_send_ctx *send =
			list_first_entry(&flush, struct tbv_send_ctx, node);
		int status = send->completion_status;

		list_del_init(&send->node);
		tbv_cancel_send_ctx_packets(send);
		tbv_send_complete(send, status);
		tbv_send_ctx_put(send);
	}

	tbv_qp_flush_reads(tqp, &flush);
	while (!list_empty(&flush)) {
		struct tbv_read_ctx *read =
			list_first_entry(&flush, struct tbv_read_ctx, node);

		list_del_init(&read->node);
		tbv_cancel_read_ctx_packets(read);
		tbv_read_complete(read, -ECANCELED);
		tbv_read_ctx_put(read);
	}

	tbv_qp_cancel_read_resps(tqp, &flush);
	while (!list_empty(&flush)) {
		struct tbv_read_resp_ctx *ctx =
			list_first_entry(&flush, struct tbv_read_resp_ctx, node);

		list_del_init(&ctx->node);
		tbv_read_resp_ctx_put(ctx);
	}

	if (!wait_event_timeout(tqp->refs_wait,
				refcount_read(&tqp->refs) == 1,
				msecs_to_jiffies(5000))) {
		pr_warn("QP %u destroy timed out with %u refs; leaving it closing for retry\n",
			qp->qp_num, refcount_read(&tqp->refs));
		return -ETIMEDOUT;
	}

	if (tqp->owner && tqp->qpn_allocated) {
		xa_lock_irqsave(&tqp->owner->verbs_qps_xa, flags);
		__xa_erase(&tqp->owner->verbs_qps_xa, qp->qp_num);
		xa_unlock_irqrestore(&tqp->owner->verbs_qps_xa, flags);
	}

	tbv_qp_put(tqp);
	wait_for_completion(&tqp->refs_zero);
	tbv_qp_flush_active_rx(tqp);
	tbv_qp_flush_reorder(tqp);
	tbv_qp_flush_apple_pending(tqp);
	for (i = 0; i < tqp->apple_pending_slot_count; i++)
		kvfree(tqp->apple_pending[i].buf);
	kvfree(tqp->apple_pending);

	pending = tqp->recv_count;
	if (tqp->owner && pending)
		atomic_sub(pending, &tqp->owner->verbs_recv_wqes);
	kfree(tqp->recvq);
	if (tqp->qpn_allocated) {
		tbv_free_qpn(tqp->backend, qp->qp_num);
		tqp->qpn_allocated = false;
	}
	if (tqp->owner)
		atomic_dec(&tqp->owner->verbs_qps);
	tbv_qp_unbind_rail(tqp);
	return 0;
}

static int tbv_modify_qp(struct ib_qp *qp, struct ib_qp_attr *attr,
			 int attr_mask, struct ib_udata *udata)
{
	struct tbv_qp *tqp = container_of(qp, struct tbv_qp, base);
	enum ib_qp_state cur_state;
	enum ib_qp_state next_state;
	unsigned long flags;
	bool flush_error = false;
	int ret = 0;

	if (!attr)
		return -EINVAL;

	if ((attr_mask & IB_QP_MAX_DEST_RD_ATOMIC) &&
	    attr->max_dest_rd_atomic > TBV_IBDEV_MAX_READ_CTX)
		return -EINVAL;
	if ((attr_mask & IB_QP_MAX_QP_RD_ATOMIC) &&
	    attr->max_rd_atomic > TBV_IBDEV_MAX_READ_CTX)
		return -EINVAL;

	spin_lock_irqsave(&tqp->lock, flags);
	cur_state = tqp->state;
	next_state = (attr_mask & IB_QP_STATE) ? attr->qp_state : cur_state;
	if ((attr_mask & IB_QP_CUR_STATE) && attr->cur_qp_state != cur_state) {
		ret = -EINVAL;
		goto out_unlock;
	}
	if (!ib_modify_qp_is_ok(cur_state, next_state, tqp->type, attr_mask)) {
		pr_warn_ratelimited("modify_qp invalid transition qpn=%u backend=%u type=%u cur=%u next=%u mask=0x%x\n",
				    qp->qp_num, tqp->backend, tqp->type,
				    cur_state, next_state, attr_mask);
		ret = -EINVAL;
		goto out_unlock;
	}

	if (attr_mask & IB_QP_STATE) {
		flush_error = tqp->state != IB_QPS_ERR &&
			      attr->qp_state == IB_QPS_ERR;
		tqp->state = attr->qp_state;
		tqp->attr.qp_state = attr->qp_state;
	}
	if (attr_mask & IB_QP_PKEY_INDEX)
		tqp->attr.pkey_index = attr->pkey_index;
	if (attr_mask & IB_QP_PORT)
		tqp->attr.port_num = attr->port_num;
	if (attr_mask & IB_QP_QKEY)
		tqp->attr.qkey = attr->qkey;
	if (attr_mask & IB_QP_ACCESS_FLAGS)
		tqp->attr.qp_access_flags = attr->qp_access_flags;
	if (attr_mask & IB_QP_AV)
		tqp->attr.ah_attr = attr->ah_attr;
	if (attr_mask & IB_QP_PATH_MTU)
		tqp->attr.path_mtu = attr->path_mtu;
	if (attr_mask & IB_QP_DEST_QPN) {
		if (tqp->early_remote_recv_credit_src_known &&
		    tqp->early_remote_recv_credit_src_qp != attr->dest_qp_num) {
			ret = -EINVAL;
			goto out_unlock;
		}
		tqp->attr.dest_qp_num = attr->dest_qp_num;
		tqp->dest_qp_known = true;
	}
	if (attr_mask & IB_QP_RQ_PSN) {
		tqp->rx_expected_psn = attr->rq_psn & TBV_PSN_MASK;
		tqp->attr.rq_psn = attr->rq_psn & TBV_PSN_MASK;
	}
	if (attr_mask & IB_QP_MAX_DEST_RD_ATOMIC)
		tqp->attr.max_dest_rd_atomic = attr->max_dest_rd_atomic;
	if (attr_mask & IB_QP_MIN_RNR_TIMER)
		tqp->attr.min_rnr_timer = attr->min_rnr_timer;
	if (attr_mask & IB_QP_SQ_PSN) {
		tqp->send_psn = attr->sq_psn & TBV_PSN_MASK;
		tqp->attr.sq_psn = attr->sq_psn & TBV_PSN_MASK;
	}
	if (attr_mask & IB_QP_TIMEOUT) {
		tqp->attr.timeout = attr->timeout;
		tqp->ack_timeout_set = true;
	}
	if (attr_mask & IB_QP_RETRY_CNT)
		tqp->attr.retry_cnt = attr->retry_cnt;
	if (attr_mask & IB_QP_RNR_RETRY)
		tqp->attr.rnr_retry = attr->rnr_retry;
	if (attr_mask & IB_QP_MAX_QP_RD_ATOMIC)
		tqp->attr.max_rd_atomic = attr->max_rd_atomic;
out_unlock:
	spin_unlock_irqrestore(&tqp->lock, flags);
	if (ret)
		return ret;
	if (flush_error)
		tbv_qp_flush_error(tqp);
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
		tbv_release_send_segments(send->segs, send->nsegs);
		atomic64_dec(&send->tqp->owner->data_wr_live);
		tbv_qp_put(send->tqp);
		kfree(send);
	}
}

static void tbv_apple_sq_release_slot(struct tbv_send_ctx *send)
{
	struct tbv_qp *tqp = send->tqp;
	unsigned long flags;
	bool wake = false;

	spin_lock_irqsave(&tqp->lock, flags);
	if (send->apple_sq_counted) {
		send->apple_sq_counted = false;
		if (tqp->apple_sq_outstanding)
			tqp->apple_sq_outstanding--;
		wake = true;
	}
	spin_unlock_irqrestore(&tqp->lock, flags);

	if (wake)
		wake_up_all(&tqp->apple_tx_wait);
}

static int tbv_apple_sq_reserve_slot(struct tbv_qp *tqp,
				     struct tbv_send_ctx *send)
{
	unsigned long flags;
	u32 max_wr = tqp->init_attr.cap.max_send_wr;
	int ret = 0;

	if (!max_wr)
		max_wr = 1;

	spin_lock_irqsave(&tqp->lock, flags);
	if (tqp->closing || tqp->state == IB_QPS_ERR) {
		ret = -ECANCELED;
	} else if (tqp->apple_sq_outstanding >= max_wr) {
		ret = -ENOMEM;
		atomic64_inc(&tqp->owner->apple_sq_full);
	} else {
		tqp->apple_sq_outstanding++;
		send->apple_sq_counted = true;
	}
	spin_unlock_irqrestore(&tqp->lock, flags);

	return ret;
}

static void tbv_apple_sq_free_entry(struct tbv_apple_sq_entry *entry)
{
	if (!entry)
		return;

	kvfree(entry->payload);
	kfree(entry);
}

static void tbv_apple_sq_queue_entry(struct tbv_qp *tqp,
				     struct tbv_apple_sq_entry *entry)
{
	struct workqueue_struct *wq = tqp->owner && tqp->owner->workqueue ?
				      tqp->owner->workqueue : system_unbound_wq;
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	list_add_tail(&entry->node, &tqp->apple_sq);
	spin_unlock_irqrestore(&tqp->lock, flags);

	atomic64_inc(&tqp->owner->apple_sq_queued);
	queue_work(wq, &tqp->apple_sq_work);
}

static struct tbv_apple_sq_entry *tbv_apple_sq_pop(struct tbv_qp *tqp)
{
	struct tbv_apple_sq_entry *entry = NULL;
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	if (!list_empty(&tqp->apple_sq)) {
		entry = list_first_entry(&tqp->apple_sq,
					 struct tbv_apple_sq_entry, node);
		list_del_init(&entry->node);
	}
	spin_unlock_irqrestore(&tqp->lock, flags);
	return entry;
}

static void tbv_qp_release_apple_tx_window(struct tbv_qp *tqp,
					   bool wr_acquired,
					   u32 frames_acquired)
{
	if (wr_acquired)
		atomic_dec(&tqp->apple_tx_inflight);
	if (frames_acquired)
		atomic_sub(frames_acquired, &tqp->apple_tx_inflight_frames);
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

	tbv_cancel_send_ctx_packets(send);
	tbv_apple_sq_release_slot(send);

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
		if (!status)
			wc.status = IB_WC_SUCCESS;
		else if (status == -EAGAIN)
			wc.status = IB_WC_RNR_RETRY_EXC_ERR;
		else if (status == -ETIMEDOUT)
			wc.status = IB_WC_RETRY_EXC_ERR;
		else
			wc.status = IB_WC_WR_FLUSH_ERR;
		wc.opcode = send->wc_opcode;
		wc.qp = &tqp->base;
		wc.port_num = 1;
		tbv_cq_push(send_cq, &wc);
	}

	return true;
}

static bool tbv_send_is_completed(struct tbv_send_ctx *send)
{
	unsigned long flags;
	bool completed;

	spin_lock_irqsave(&send->lock, flags);
	completed = send->completed;
	spin_unlock_irqrestore(&send->lock, flags);

	return completed;
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

static void tbv_read_resp_free_frags(struct tbv_read_ctx *read)
{
	struct tbv_rx_reorder_frag *frag;
	struct tbv_rx_reorder_frag *tmp;

	list_for_each_entry_safe(frag, tmp, &read->resp_frags, node) {
		list_del(&frag->node);
		kfree(frag);
	}
	read->resp_buffered_bytes = 0;
}

static void tbv_read_ctx_put(struct tbv_read_ctx *read)
{
	if (refcount_dec_and_test(&read->refs)) {
		tbv_read_resp_free_frags(read);
		tbv_release_read_segments(read->segs, read->nsegs);
		atomic64_dec(&read->tqp->owner->data_wr_live);
		tbv_qp_put(read->tqp);
		kfree(read);
	}
}

static void tbv_read_resp_ctx_get(struct tbv_read_resp_ctx *ctx)
{
	refcount_inc(&ctx->refs);
}

static void tbv_read_resp_ctx_put(struct tbv_read_resp_ctx *ctx)
{
	if (!ctx)
		return;
	if (refcount_dec_and_test(&ctx->refs)) {
		vfree(ctx->data);
		tbv_path_put_response(ctx->rx_path);
		tbv_mr_put(ctx->mr);
		tbv_qp_put(ctx->tqp);
		kfree(ctx);
	}
}

static enum ib_wc_status tbv_read_wc_status(int status)
{
	if (!status)
		return IB_WC_SUCCESS;
	if (status == -ETIMEDOUT)
		return IB_WC_RETRY_EXC_ERR;
	if (status == -ECANCELED)
		return IB_WC_WR_FLUSH_ERR;
	if (status == -EACCES || status == -EFAULT || status == -EPERM)
		return IB_WC_REM_ACCESS_ERR;
	return IB_WC_GENERAL_ERR;
}

static void tbv_ibdev_atomic64_max_ms(atomic64_t *counter, u64 value)
{
	s64 old;

	if (value > S64_MAX)
		value = S64_MAX;

	for (;;) {
		old = atomic64_read(counter);
		if (old >= (s64)value)
			return;
		if (atomic64_cmpxchg(counter, old, (s64)value) == old)
			return;
	}
}

static void tbv_note_matched_send_ack(struct tbv_state *state,
				      const struct tbv_native_data_header *hdr,
				      const struct tbv_send_ctx *send)
{
	unsigned long now = jiffies;
	unsigned long total_age_ms = 0;
	unsigned long current_age_ms = 0;
	bool retried;

	if (send->first_queued_jiffies)
		total_age_ms =
			jiffies_to_msecs(now - send->first_queued_jiffies);
	if (send->queued_jiffies)
		current_age_ms =
			jiffies_to_msecs(now - send->queued_jiffies);

	retried = send->retry_reason != TBV_SEND_POST_INITIAL ||
		  send->retries || send->rnr_retries;

	atomic64_inc(&state->data_rx_ack_matched);
	tbv_ibdev_atomic64_max_ms(&state->data_rx_ack_match_max_ms,
				  total_age_ms);
	tbv_ibdev_atomic64_max_ms(&state->data_rx_ack_match_current_max_ms,
				  current_age_ms);
	if (total_age_ms > 10)
		atomic64_inc(&state->data_rx_ack_match_over_10ms);
	if (total_age_ms > 64)
		atomic64_inc(&state->data_rx_ack_match_over_64ms);
	if (retried) {
		atomic64_inc(&state->data_rx_ack_match_retried);
		pr_warn_ratelimited("native ACK matched after retry qpn=0x%x dest_qp=0x%x psn=%u ack_status=%u total_age_ms=%lu current_age_ms=%lu retries=%u rnr_retries=%u reason=%u\n",
				    hdr->dest_qp, hdr->src_qp,
				    hdr->psn & TBV_PSN_MASK, hdr->imm_data,
				    total_age_ms, current_age_ms,
				    send->retries, send->rnr_retries,
				    send->retry_reason);
	}
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
		tbv_qp_arm_read_timeout(tqp, read);
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

	if (send->retryable) {
		unsigned long flags;
		bool tx_drained;

		spin_lock_irqsave(&tqp->lock, flags);
		tx_drained = atomic_dec_and_test(&send->tx_pending);
		if (tx_drained) {
			send->retrying = false;
			if (!status && send->pending && !send->rnr_waiting) {
				tbv_send_mark_queued(send, jiffies);
				tbv_qp_schedule_timeout_locked(tqp);
			}
		}
		spin_unlock_irqrestore(&tqp->lock, flags);
	}

	if (!status) {
		tbv_send_ctx_put(send);
		return;
	}

	{
		LIST_HEAD(complete);
		struct tbv_send_ctx *pos;

		if (tbv_qp_complete_send_ordered(tqp, send->psn, status,
						 &complete, NULL)) {
			while (!list_empty(&complete)) {
				pos = list_first_entry(&complete,
						       struct tbv_send_ctx,
						       node);
				list_del_init(&pos->node);
				tbv_send_complete(pos, pos->completion_status);
				tbv_send_ctx_put(pos);
			}
		}
	}
	tbv_send_ctx_put(send);
}

static void tbv_apple_send_complete_work(struct work_struct *work)
{
	struct tbv_send_ctx *send =
		container_of(to_delayed_work(work), struct tbv_send_ctx,
			     apple_complete_work);

	tbv_send_complete(send, send->apple_complete_status);
	tbv_send_ctx_put(send);
}

static bool tbv_qp_timeout_reap_tx(struct tbv_qp *tqp,
				   struct list_head *retry_sends,
				   struct list_head *completed_sends,
				   struct list_head *timed_out_reads,
				   unsigned long now,
				   unsigned long timeout,
				   bool *tx_failed)
{
	struct tbv_send_ctx *send, *send_tmp;
	struct tbv_read_ctx *read, *read_tmp;
	unsigned long flags;
	bool need_resched = false;

	spin_lock_irqsave(&tqp->lock, flags);
	list_for_each_entry_safe(send, send_tmp, &tqp->pending_sends, node) {
		u8 max_retries = send->max_retries;
		unsigned long send_timeout =
			(send->retryable && max_retries) ?
				tbv_send_retry_jiffies(timeout, max_retries) :
				timeout;
		unsigned long rnr_timeout =
			tbv_rnr_timer_jiffies(tqp->attr.min_rnr_timer);

		if (send->ready) {
			need_resched = true;
			continue;
		}
		if (send->rnr_waiting) {
			bool credit_wait =
				tbv_send_rnr_waits_for_recv_credit(send);
			bool credit_ready = send->recv_credit_required &&
					    tqp->remote_recv_credits;
			bool timer_expired =
				tbv_qp_entry_expired(send->queued_jiffies, now,
						     rnr_timeout);

			if (credit_wait && !credit_ready)
				continue;
			if (!credit_ready && !timer_expired) {
				need_resched = true;
				continue;
			}
			if (send->retryable && !send->retrying &&
			    !atomic_read(&send->tx_pending) &&
			    tbv_send_rnr_retry_allowed(send) &&
			    !tqp->closing && tqp->state != IB_QPS_ERR) {
				if (credit_ready)
					tqp->remote_recv_credits--;
				send->retrying = true;
				send->rnr_waiting = false;
				send->retry_reason = TBV_SEND_POST_RETRY_RNR;
				tbv_send_ctx_get(send);
				list_add_tail(&send->retry_node, retry_sends);
				continue;
			}
			if (!send->ready) {
				send->ready = true;
				send->completion_status = -EAGAIN;
				send->retrying = false;
				send->rnr_waiting = false;
			}
			if (tx_failed)
				*tx_failed = true;
			tbv_qp_drain_ready_sends_locked(tqp, completed_sends);
			continue;
		}

		if (!tbv_qp_entry_expired(send->queued_jiffies, now,
					  send_timeout))
			continue;
		if (send->retryable && atomic_read(&send->tx_pending)) {
			need_resched = true;
			continue;
		}
		if (send->retryable && max_retries && send->retrying) {
			if (!tbv_qp_entry_expired(send->queued_jiffies, now,
						  timeout)) {
				need_resched = true;
				continue;
			}
		}
		if (send->retryable && !send->retrying &&
		    !atomic_read(&send->tx_pending) &&
		    send->retries < max_retries &&
		    !tqp->closing && tqp->state != IB_QPS_ERR) {
			send->retrying = true;
			send->retry_reason = TBV_SEND_POST_RETRY_TIMEOUT;
			tbv_send_ctx_get(send);
			list_add_tail(&send->retry_node, retry_sends);
			continue;
		}
		if (send->retryable && send->retries >= max_retries)
			atomic64_inc(&tqp->owner->data_wr_retry_exhausted);
		if (!send->ready) {
			send->ready = true;
			send->completion_status = -ETIMEDOUT;
			send->retrying = false;
			send->rnr_waiting = false;
		}
		if (tx_failed)
			*tx_failed = true;
		tbv_qp_drain_ready_sends_locked(tqp, completed_sends);
	}
	list_for_each_entry_safe(read, read_tmp, &tqp->pending_reads, node) {
		if (read->ready) {
			need_resched = true;
			continue;
		}
		if (!tbv_qp_entry_expired(read->queued_jiffies, now, timeout))
			continue;
		tbv_qp_release_sendq_counted_locked(tqp, &read->sq_counted);
		list_move_tail(&read->node, timed_out_reads);
	}
	need_resched |= !tqp->closing &&
			(!list_empty(retry_sends) ||
			 !list_empty(&tqp->pending_sends) ||
			 !list_empty(&tqp->pending_reads));
	spin_unlock_irqrestore(&tqp->lock, flags);

	return need_resched;
}

static bool tbv_qp_timeout_reap_rx(struct tbv_qp *tqp, unsigned long now,
				   unsigned long timeout)
{
	struct tbv_state *state = tqp->owner;
	bool need_resched;
	bool timed_out = false;

	mutex_lock(&tqp->rx_lock);
	if (tqp->rx_msg.active &&
	    tbv_qp_entry_expired(tqp->rx_msg.started_jiffies, now, timeout)) {
		atomic64_inc(&state->data_rx_active_timeout);
		pr_warn_ratelimited("native SEND active timeout qpn=0x%x src_qp=0x%x psn=%u received=%u total=%u last_offset=%u last_len=%u first_rail=0x%x first_route=0x%llx first_path=%u last_rail=0x%x last_route=0x%llx last_path=%u\n",
				    tqp->base.qp_num, tqp->rx_msg.src_qp,
				    tqp->rx_msg.psn, tqp->rx_msg.received,
				    tqp->rx_msg.total_len,
				    tqp->rx_msg.last_offset,
				    tqp->rx_msg.last_len,
				    tqp->rx_msg.first_rail_id,
				    tqp->rx_msg.first_route,
				    tqp->rx_msg.first_path_id,
				    tqp->rx_msg.last_rail_id,
				    tqp->rx_msg.last_route,
				    tqp->rx_msg.last_path_id);
		tbv_rx_fail_active_send(state, tqp, NULL, IB_WC_GENERAL_ERR);
		timed_out = true;
	}
	if (tqp->rx_write.active &&
	    tbv_qp_entry_expired(tqp->rx_write.started_jiffies, now, timeout)) {
		atomic64_inc(&state->data_rx_active_timeout);
		pr_warn_ratelimited("native RDMA_WRITE active timeout qpn=0x%x src_qp=0x%x psn=%u base=0x%llx received=%u with_imm=%u\n",
				    tqp->base.qp_num, tqp->rx_write.src_qp,
				    tqp->rx_write.psn,
				    tqp->rx_write.remote_addr,
				    tqp->rx_write.received,
				    tqp->rx_write.with_imm);
		tbv_rx_fail_active_write_locked(state, tqp, NULL,
						    IB_WC_GENERAL_ERR);
		timed_out = true;
	}

	for (;;) {
		struct tbv_rx_reorder_msg *msg;
		u32 src_qp;
		u32 psn;
		u32 total_len;
		enum tbv_rx_reorder_kind kind;
		bool expected;
		bool found = false;

		list_for_each_entry(msg, &tqp->rx_reorder, node) {
			if (!tbv_qp_entry_expired(msg->first_jiffies, now,
						  timeout))
				continue;
			found = true;
			break;
		}
		if (!found)
			break;

		src_qp = msg->src_qp;
		psn = msg->psn;
		total_len = msg->total_len;
		kind = msg->kind;
		expected = psn == tqp->rx_expected_psn;
		tbv_rx_drop_reorder_msg_locked(state, tqp, msg);
		atomic64_inc(&state->data_rx_reorder_timeout);
		if (expected)
			tqp->rx_expected_psn = tbv_psn_next(psn);
		if (kind == TBV_RX_REORDER_READ_REQ)
			tbv_send_read_status_on_path(tqp, NULL, src_qp,
						     tqp->base.qp_num, psn,
						     total_len, -ETIMEDOUT);
		else
			tbv_send_ack(tqp, src_qp, tqp->base.qp_num, psn,
				     TBV_NATIVE_SEND_ACK_ERROR);
		timed_out = true;
		if (expected)
			tbv_rx_drain_reorder_locked(state, tqp, NULL);
	}

	need_resched = tqp->rx_msg.active || tqp->rx_write.active ||
		       !list_empty(&tqp->rx_reorder);
	mutex_unlock(&tqp->rx_lock);

	if (timed_out)
		tbv_qp_mark_error(tqp);

	return need_resched;
}

static bool tbv_qp_timeout_reap_read_resps(struct tbv_qp *tqp,
					   struct list_head *retry,
					   struct list_head *drop,
					   unsigned long now,
					   unsigned long timeout)
{
	struct tbv_read_resp_ctx *ctx;
	struct tbv_read_resp_ctx *tmp;
	unsigned long flags;
	bool need_resched = false;

	spin_lock_irqsave(&tqp->lock, flags);
	list_for_each_entry_safe(ctx, tmp, &tqp->pending_read_resps, node) {
		if (!tbv_qp_entry_expired(ctx->queued_jiffies, now, timeout)) {
			need_resched = true;
			continue;
		}
		if (ctx->retrying) {
			need_resched = true;
			continue;
		}
		if (ctx->retries >= TBV_READ_RESP_MAX_RETRIES ||
		    tqp->closing || tqp->state == IB_QPS_ERR) {
			list_del_init(&ctx->node);
			ctx->closing = true;
			list_add_tail(&ctx->retry_node, drop);
			continue;
		}

		ctx->retrying = true;
		ctx->queued_jiffies = now;
		tbv_read_resp_ctx_get(ctx);
		list_add_tail(&ctx->retry_node, retry);
		need_resched = true;
	}
	spin_unlock_irqrestore(&tqp->lock, flags);

	return need_resched;
}

static void tbv_qp_timeout_work(struct work_struct *work)
{
	struct tbv_qp *tqp =
		container_of(to_delayed_work(work), struct tbv_qp,
			     timeout_work);
	LIST_HEAD(retry_sends);
	LIST_HEAD(completed_sends);
	LIST_HEAD(timed_out_reads);
	LIST_HEAD(retry_read_resps);
	LIST_HEAD(drop_read_resps);
	unsigned long timeout;
	unsigned long rx_timeout;
	unsigned long read_resp_timeout;
	unsigned long now = jiffies;
	unsigned long flags;
	bool need_resched = false;
	bool read_resp_dropped = false;
	bool tx_timed_out = false;

	spin_lock_irqsave(&tqp->lock, flags);
	tqp->timeout_work_armed = false;
	timeout = tbv_qp_tx_timeout_jiffies_locked(tqp);
	rx_timeout = tbv_qp_rx_timeout_jiffies_locked(tqp, timeout);
	spin_unlock_irqrestore(&tqp->lock, flags);
	read_resp_timeout = tbv_read_resp_retry_jiffies(timeout);

	if (!timeout)
		return;

	need_resched |= tbv_qp_timeout_reap_tx(tqp, &retry_sends,
					       &completed_sends,
					       &timed_out_reads, now,
					       timeout, &tx_timed_out);
	tx_timed_out |= !list_empty(&timed_out_reads);
	if (tx_timed_out) {
		need_resched = false;
	} else {
		need_resched |=
			tbv_qp_timeout_reap_read_resps(tqp, &retry_read_resps,
						       &drop_read_resps, now,
						       read_resp_timeout);
	}

	while (!list_empty(&retry_sends)) {
		struct tbv_send_ctx *send =
			list_first_entry(&retry_sends, struct tbv_send_ctx,
					 retry_node);
		enum tbv_send_post_reason reason = send->retry_reason;
		bool pending = false;
		bool fatal = false;
		int ret;

			list_del_init(&send->retry_node);
			if (tbv_send_is_completed(send) ||
			    !tbv_qp_send_retry_pending(tqp, send)) {
				tbv_send_ctx_put(send);
				continue;
			}

		ret = tbv_native_send_ctx_post_frames(send, reason);
		if (ret)
			atomic64_inc(&tqp->owner->data_wr_retry_enqueue_error);

		spin_lock_irqsave(&tqp->lock, flags);
		pending = send->pending && !send->completed &&
			  !tqp->closing && tqp->state != IB_QPS_ERR;
		if (pending && !ret) {
			if (reason == TBV_SEND_POST_RETRY_RNR) {
				if (send->rnr_retries < U8_MAX)
					send->rnr_retries++;
			} else if (send->retries < U8_MAX) {
				send->retries++;
			}
			tbv_send_mark_queued(send, jiffies);
			tbv_qp_schedule_timeout_locked(tqp);
			need_resched = true;
		} else if (pending && ret == -ENOMEM) {
			tbv_send_mark_queued(send, jiffies);
			send->retrying = false;
			tbv_qp_schedule_timeout_locked(tqp);
			need_resched = true;
		} else if (pending && ret == -ENOTCONN) {
			if (send->retries < U8_MAX)
				send->retries++;
			tbv_send_mark_queued(send, jiffies);
			send->retrying = false;
			tbv_qp_schedule_timeout_locked(tqp);
			need_resched = true;
		} else if (pending && ret) {
			if (!send->ready) {
				send->ready = true;
				send->completion_status = -ETIMEDOUT;
				send->retrying = false;
				send->rnr_waiting = false;
			}
			tbv_qp_drain_ready_sends_locked(tqp,
							&completed_sends);
			fatal = true;
		} else {
			send->retrying = false;
		}
		spin_unlock_irqrestore(&tqp->lock, flags);

		if (fatal)
			tx_timed_out = true;
		tbv_send_ctx_put(send);
	}
	if (tx_timed_out) {
		need_resched = false;
	}

	while (!list_empty(&completed_sends)) {
		struct tbv_send_ctx *send =
			list_first_entry(&completed_sends,
					 struct tbv_send_ctx, node);
		int status = send->completion_status;

		list_del_init(&send->node);
		tbv_cancel_send_ctx_packets(send);
		if (status == -ETIMEDOUT)
			atomic64_inc(&tqp->owner->data_wr_timeout);
		else if (status == -EAGAIN)
			atomic64_inc(&tqp->owner->data_wr_rnr_retry_exhausted);
		tbv_send_complete(send, status);
		tbv_send_ctx_put(send);
	}

	if (tx_timed_out)
		tbv_qp_mark_error(tqp);

	while (!list_empty(&timed_out_reads)) {
		struct tbv_read_ctx *read =
			list_first_entry(&timed_out_reads,
					 struct tbv_read_ctx, node);

		list_del_init(&read->node);
		tbv_cancel_read_ctx_packets(read);
		atomic64_inc(&tqp->owner->data_wr_timeout);
		tbv_read_complete(read, -ETIMEDOUT);
		tbv_read_ctx_put(read);
	}

	while (!list_empty(&retry_read_resps)) {
		struct tbv_read_resp_ctx *ctx =
			list_first_entry(&retry_read_resps,
					 struct tbv_read_resp_ctx,
					 retry_node);
		struct tbv_state *state = tqp->owner;
		bool closing;
		bool attempted = false;
		bool was_sent = false;
		int ret = 0;

		list_del_init(&ctx->retry_node);
		spin_lock_irqsave(&tqp->lock, flags);
		closing = ctx->closing;
		was_sent = ctx->response_sent;
		spin_unlock_irqrestore(&tqp->lock, flags);
		if (!closing) {
			attempted = true;
			ret = tbv_send_read_response_ctx(ctx);
		}

		if (attempted) {
			if (!ret && was_sent)
				atomic64_inc(&state->data_read_resp_retransmit);
			else if (ret == -ENOMEM)
				atomic64_inc(&state->data_rx_read_req_resp_busy);
			else if (ret)
				atomic64_inc(&state->data_wr_path_send_error);
		}

		spin_lock_irqsave(&tqp->lock, flags);
		if (!ctx->closing) {
			if (!ret) {
				if (was_sent && ctx->retries < U8_MAX)
					ctx->retries++;
				ctx->response_sent = true;
				ctx->queued_jiffies = jiffies;
			} else if (ret != -ENOMEM) {
				if (ctx->retries < U8_MAX)
					ctx->retries++;
				ctx->queued_jiffies = jiffies;
			} else {
				ctx->queued_jiffies = jiffies;
			}
		}
		ctx->retrying = false;
		closing = ctx->closing;
		spin_unlock_irqrestore(&tqp->lock, flags);
		if (!closing)
			need_resched = true;
		tbv_read_resp_ctx_put(ctx);
	}

	while (!list_empty(&drop_read_resps)) {
		struct tbv_read_resp_ctx *ctx =
			list_first_entry(&drop_read_resps,
					 struct tbv_read_resp_ctx,
					 retry_node);

		list_del_init(&ctx->retry_node);
		read_resp_dropped = true;
		atomic64_inc(&tqp->owner->data_read_resp_drop);
		tbv_read_resp_ctx_put(ctx);
	}
	if (read_resp_dropped)
		tbv_qp_mark_error(tqp);

	need_resched |= tbv_qp_timeout_reap_rx(tqp, now, rx_timeout);
	if (need_resched)
		tbv_qp_schedule_timeout(tqp);
}

static void tbv_release_send_segments(struct tbv_send_segment *segs, int nsegs)
{
	int i;

	for (i = 0; i < nsegs; i++)
		tbv_mr_put(segs[i].mr);
}

static void tbv_get_send_segments(struct tbv_send_segment *dst,
				  const struct tbv_send_segment *src,
				  int nsegs)
{
	int i;

	for (i = 0; i < nsegs; i++) {
		dst[i] = src[i];
		if (dst[i].mr)
			refcount_inc(&dst[i].mr->refs);
	}
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
		if (mr->dma_mr) {
			tbv_mr_put(mr);
			ret = -EOPNOTSUPP;
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
	wake_up_all(&tqp->apple_tx_wait);
	if (status) {
		if (tbv_qp_unqueue_send(tqp, send)) {
			tbv_send_complete(send, status);
			tbv_send_ctx_put(send);
		}
	} else if (last) {
		if (tbv_qp_unqueue_send(tqp, send)) {
			u32 delay_us = READ_ONCE(apple_tx_completion_delay_us);

			if (delay_us) {
				struct workqueue_struct *wq =
					tqp->owner && tqp->owner->workqueue ?
						tqp->owner->workqueue :
						system_unbound_wq;
				unsigned long delay =
					max_t(unsigned long, 1,
					      usecs_to_jiffies(delay_us));

				send->apple_complete_status = 0;
				queue_delayed_work(wq,
						   &send->apple_complete_work,
						   delay);
			} else {
				tbv_send_complete(send, 0);
				tbv_send_ctx_put(send);
			}
		}
	}

	tbv_send_ctx_put(send);
}

static int tbv_apple_send_fill(void *ctx, void *dst, u32 len)
{
	struct tbv_apple_send_fill *fill = ctx;
	u32 valid_len = fill->payload_len;
	__le32 crc_le;

	if (fill->append_crc) {
		if (valid_len > U32_MAX - sizeof(crc_le))
			return -EINVAL;
		valid_len += sizeof(crc_le);
	}
	if (valid_len > len)
		return -EINVAL;

	memcpy(dst, fill->payload, fill->payload_len);
	if (fill->append_crc) {
		crc_le = cpu_to_le32(fill->crc);
		memcpy((u8 *)dst + fill->payload_len, &crc_le,
		       sizeof(crc_le));
	}
	if (valid_len < len)
		memset((u8 *)dst + valid_len, 0, len - valid_len);
	return 0;
}

static int tbv_post_apple_send_piece(struct tbv_qp *tqp,
				     struct tbv_path *path,
				     struct tbv_send_ctx *ctx,
				     const void *payload, u32 payload_len,
				     u32 wire_len, u8 sof, u8 eof,
				     bool append_crc, u32 crc,
				     tbv_path_tx_done_fn done, void *done_ctx)
{
	struct tbv_apple_send_fill fill = {
		.payload = payload,
		.payload_len = payload_len,
		.crc = crc,
		.append_crc = append_crc,
	};
	int ret;

	tbv_send_ctx_get(ctx);
	atomic64_inc(&tqp->owner->data_wr_path_send);
	ret = tbv_path_send_marked_fill(path, wire_len, sof, eof,
					TBV_PATH_SEND_DEFER,
					tbv_apple_send_fill, &fill,
					done, done_ctx);
	if (ret) {
		tbv_send_ctx_put(ctx);
		atomic64_inc(&tqp->owner->data_wr_path_send_error);
	}
	return ret;
}

static int tbv_post_apple_send_frame(struct tbv_qp *tqp,
				     struct tbv_path *path,
				     struct tbv_send_ctx *ctx,
				     const void *payload,
				     u32 payload_len, u8 sof, u8 eof,
				     tbv_path_tx_done_fn done,
				     void *done_ctx)
{
	return tbv_post_apple_send_piece(tqp, path, ctx, payload,
					 payload_len, payload_len, sof, eof,
					 false, 0, done, done_ctx);
}

static int tbv_post_apple_send(struct tbv_qp *tqp, const struct ib_send_wr *wr)
{
	struct tbv_send_segment segs[TBV_IBDEV_MAX_SGE];
	struct tbv_apple_sq_entry *entry = NULL;
	struct tbv_send_ctx *ctx = NULL;
	unsigned long flags;
	u32 total_len = 0;
	int nsegs = 0;
	void *payload = NULL;
	int ret;

	if (tqp->type != IB_QPT_UC)
		return -EOPNOTSUPP;
	if (wr->opcode != IB_WR_SEND)
		return -EOPNOTSUPP;
	if (!tbv_qp_has_dest_qp(tqp))
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
	ctx->wc_opcode = IB_WC_SEND;
	INIT_DELAYED_WORK(&ctx->apple_complete_work,
			  tbv_apple_send_complete_work);
	INIT_LIST_HEAD(&ctx->node);
	INIT_LIST_HEAD(&ctx->retry_node);
	atomic_set(&ctx->tx_pending, 0);

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry) {
		ret = -ENOMEM;
		goto err_put_ctx;
	}

	payload = kvzalloc(total_len, GFP_KERNEL);
	if (!payload) {
		ret = -ENOMEM;
		goto err_free_entry;
	}

	ret = tbv_copy_send_range(segs, nsegs, 0, payload, total_len);
	if (ret) {
		atomic64_inc(&tqp->owner->data_wr_copy_error);
		goto err_free_payload;
	}
	atomic64_inc(&tqp->owner->data_wr_copied);

	ret = tbv_apple_sq_reserve_slot(tqp, ctx);
	if (ret)
		goto err_free_payload;

	spin_lock_irqsave(&tqp->lock, flags);
	ctx->psn = tqp->send_psn & TBV_PSN_MASK;
	tqp->send_psn = tbv_psn_next(ctx->psn);
	spin_unlock_irqrestore(&tqp->lock, flags);

	INIT_LIST_HEAD(&entry->node);
	entry->send = ctx;
	entry->payload = payload;
	entry->length = total_len;
	payload = NULL;

	tbv_qp_queue_send(tqp, ctx);
	tbv_apple_sq_queue_entry(tqp, entry);
	tbv_release_send_segments(segs, nsegs);
	atomic64_inc(&tqp->owner->data_tx_accepted);
	return 0;

err_free_payload:
	kvfree(payload);
err_free_entry:
	kfree(entry);
	tbv_apple_sq_release_slot(ctx);
err_put_ctx:
	tbv_send_ctx_put(ctx);
	tbv_release_send_segments(segs, nsegs);
	return ret;
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
		remaining = min_t(u32, remaining, stream->max_chunk);
		ret = tbv_umem_page_from_addr(seg->mr, seg->addr + seg_off,
					      remaining, page, page_off,
					      length);
		if (ret)
			return ret;

		stream->offset += *length;
		refcount_inc(&stream->refs);
		atomic_inc(&stream->send->tx_pending);
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

static bool tbv_send_ctx_allows_raw_zcopy(const struct tbv_send_ctx *ctx)
{
	/*
	 * Raw page streams remove the per-frame native header. That is not safe
	 * for retryable RC operations: a retransmit can reach a receiver path
	 * that is still consuming raw bytes from the earlier attempt, and
	 * arbitrary user payload cannot be distinguished from a new frame header.
	 */
	return ctx && !ctx->retryable;
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

static bool tbv_send_ctx_is_send(const struct tbv_send_ctx *ctx)
{
	return ctx->opcode == TBV_NATIVE_DATA_OP_SEND ||
	       ctx->opcode == TBV_NATIVE_DATA_OP_SEND_IMM;
}

static bool tbv_send_ctx_is_write(const struct tbv_send_ctx *ctx)
{
	return ctx->opcode == TBV_NATIVE_DATA_OP_RDMA_WRITE ||
	       ctx->opcode == TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM;
}

static bool tbv_send_ctx_has_imm(const struct tbv_send_ctx *ctx)
{
	return ctx->opcode == TBV_NATIVE_DATA_OP_SEND_IMM ||
	       ctx->opcode == TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM;
}

static void tbv_send_ctx_build_native_header(struct tbv_send_ctx *ctx,
					     u32 offset, u32 payload_len,
					     bool last,
					     struct tbv_native_data_header *hdr)
{
	struct tbv_qp *tqp = ctx->tqp;

	memset(hdr, 0, sizeof(*hdr));
	hdr->opcode = ctx->opcode;
	hdr->flags = last ? TBV_NATIVE_DATA_F_LAST : 0;
	if (last && ctx->solicited)
		hdr->flags |= TBV_NATIVE_DATA_F_SOLICITED;
	hdr->dest_qp = tqp->attr.dest_qp_num;
	hdr->src_qp = tqp->base.qp_num;
	hdr->psn = ctx->psn;
	hdr->length = payload_len;

	if (tbv_send_ctx_is_send(ctx)) {
		hdr->imm_data = ctx->total_len;
		hdr->remote_addr = 0;
		hdr->frag_offset = offset;
		hdr->rkey = tbv_send_ctx_has_imm(ctx) ? ctx->imm_data : 0;
	} else {
		hdr->imm_data = tbv_send_ctx_has_imm(ctx) ?
				ctx->imm_data : ctx->total_len;
		hdr->remote_addr = ctx->remote_addr;
		hdr->frag_offset = offset;
		hdr->rkey = ctx->rkey;
	}
}

static int tbv_native_send_ctx_post_frames(struct tbv_send_ctx *ctx,
					   enum tbv_send_post_reason reason)
{
	struct tbv_qp *tqp = ctx->tqp;
	struct tbv_native_data_header hdr = {};
	struct tbv_path *path = NULL;
	struct tbv_path *paths[TBV_NATIVE_MAX_LANES] = {};
	struct list_head frame_lists[TBV_NATIVE_MAX_LANES];
	struct list_head packet_lists[TBV_NATIVE_MAX_LANES];
	u32 reservations[TBV_NATIVE_MAX_LANES] = {};
	u32 frame_counts[TBV_NATIVE_MAX_LANES] = {};
	u32 packet_counts[TBV_NATIVE_MAX_LANES] = {};
	u32 nfrags = ctx->total_len ?
		     DIV_ROUND_UP(ctx->total_len,
				  TBV_NATIVE_DATA_MAX_PAYLOAD) : 1;
	bool fragment_striping = tqp->owner->native_fragment_striping &&
				  !tbv_send_ctx_is_write(ctx);
	u32 offset = 0;
	u32 frag_idx = 0;
	u32 path_count = 0;
	u32 list_idx;
	bool zcopy_requested;
	bool raw_zcopy_allowed = false;
	bool zcopy_safe = false;
	bool sent_any = false;
	int ret = 0;

	for (list_idx = 0; list_idx < ARRAY_SIZE(frame_lists); list_idx++) {
		INIT_LIST_HEAD(&frame_lists[list_idx]);
		INIT_LIST_HEAD(&packet_lists[list_idx]);
	}

	if (reason == TBV_SEND_POST_RETRY_TIMEOUT ||
	    reason == TBV_SEND_POST_RETRY_RNR) {
		unsigned long age_ms = ctx->queued_jiffies ?
			jiffies_to_msecs(jiffies - ctx->queued_jiffies) : 0;

		if (reason == TBV_SEND_POST_RETRY_RNR)
			atomic64_inc(&tqp->owner->data_wr_rnr_retransmit);
		else
			atomic64_inc(&tqp->owner->data_wr_retransmit);
		pr_warn_ratelimited("native retransmit qpn=0x%x dest_qp=0x%x psn=%u opcode=%u total=%u retries=%u max_retries=%u rnr_retries=%u max_rnr_retries=%u reason=%u age_ms=%lu tx_pending=%d\n",
				    tqp->base.qp_num, tqp->attr.dest_qp_num,
				    ctx->psn, ctx->opcode, ctx->total_len,
				    ctx->retries, ctx->max_retries,
				    ctx->rnr_retries, ctx->max_rnr_retries,
				    reason, age_ms, atomic_read(&ctx->tx_pending));
	}

	zcopy_requested = !fragment_striping && tbv_send_ctx_is_write(ctx) &&
			  tbv_should_zcopy_payload(ctx->total_len);
	if (zcopy_requested) {
		raw_zcopy_allowed = tbv_send_ctx_allows_raw_zcopy(ctx);
		if (raw_zcopy_allowed)
			zcopy_safe = tbv_send_segments_zcopy_safe(
				ctx->segs, ctx->nsegs, ctx->total_len);
	}

	if (zcopy_requested && raw_zcopy_allowed && zcopy_safe) {
		struct tbv_send_page_stream *stream;

		if (reason == TBV_SEND_POST_INITIAL)
			atomic64_inc(&tqp->owner->data_wr_zcopy);
		tbv_send_ctx_build_native_header(ctx, 0, ctx->total_len,
						 true, &hdr);

		mutex_lock(&tqp->owner->lock);
		path = tbv_select_native_data_path_for_qp_locked(tqp);
		mutex_unlock(&tqp->owner->lock);
		if (!path) {
			atomic64_inc(&tqp->owner->data_wr_no_path);
			return -ENOTCONN;
		}

		stream = kzalloc(sizeof(*stream), GFP_KERNEL);
		if (!stream) {
			tbv_release_path_refs(&path, 1);
			return -ENOMEM;
		}
		refcount_set(&stream->refs, 1);
		stream->send = ctx;
		stream->total_len = ctx->total_len;
		stream->max_chunk = TBV_NATIVE_DATA_FRAME_SIZE;
		stream->nsegs = ctx->nsegs;
		tbv_get_send_segments(stream->segs, ctx->segs, ctx->nsegs);

		tbv_send_ctx_get(ctx);
		tbv_send_ctx_get(ctx);
		atomic_inc(&ctx->tx_pending);
		atomic64_inc(&tqp->owner->data_wr_path_send);
		ret = tbv_path_send_page_stream(path, &hdr, ctx->total_len, 0,
						tbv_send_tx_done, ctx,
						tbv_send_page_stream_next,
						stream);
		tbv_release_path_refs(&path, 1);
		tbv_send_page_stream_put(stream);
		if (ret) {
			tbv_send_ctx_put(ctx);
			atomic64_inc(&tqp->owner->data_wr_path_send_error);
			return ret;
		}
		tbv_send_ctx_put(ctx);
		return 0;
	}

	if (reason == TBV_SEND_POST_INITIAL) {
		if (zcopy_requested) {
			atomic64_inc(&tqp->owner->data_wr_zcopy_fallback);
			if (raw_zcopy_allowed && !zcopy_safe)
				atomic64_inc(
					&tqp->owner->data_wr_zcopy_fallback_unsafe_sge);
		}
		atomic64_inc(&tqp->owner->data_wr_copied);
	}

	mutex_lock(&tqp->owner->lock);
	if (fragment_striping) {
		u32 i;

		path_count = tbv_collect_native_data_paths_for_qp_locked(
			tqp, paths, ARRAY_SIZE(paths));
		if (!path_count) {
			ret = -ENOTCONN;
			goto out_unlock_paths;
		}
		for (i = 0; i < nfrags; i++)
			reservations[(ctx->psn + i) % path_count]++;
	} else {
		path = tbv_select_native_data_path_for_qp_locked(tqp);
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
		return ret;
	}

	do {
		u32 payload_len = min_t(u32, ctx->total_len - offset,
					TBV_NATIVE_DATA_MAX_PAYLOAD);
		bool last = offset + payload_len == ctx->total_len;
		u32 path_idx = fragment_striping ?
			       (ctx->psn + frag_idx) % path_count : 0;
		u32 packet_len = TBV_NATIVE_DATA_HDR_SIZE + payload_len;
		struct tbv_path_owned_frame *owned;
		u8 *frame;

		frame = kmalloc(packet_len, GFP_KERNEL);
		if (!frame) {
			ret = -ENOMEM;
			goto err_release_paths;
		}

		ret = tbv_copy_send_range(ctx->segs, ctx->nsegs, offset,
					  frame + TBV_NATIVE_DATA_HDR_SIZE,
					  payload_len);
		if (ret) {
			kfree(frame);
			atomic64_inc(&tqp->owner->data_wr_copy_error);
			goto err_release_paths;
		}

		tbv_send_ctx_build_native_header(ctx, offset, payload_len,
						 last, &hdr);
		ret = tbv_native_data_build_header(frame, packet_len, &hdr);
		if (ret < 0) {
			kfree(frame);
			goto err_release_paths;
		}

		owned = kzalloc(sizeof(*owned), GFP_KERNEL);
		if (!owned) {
			kfree(frame);
			ret = -ENOMEM;
			goto err_release_paths;
		}
		INIT_LIST_HEAD(&owned->node);
		owned->data = frame;
		owned->len = packet_len;
		owned->sof = TBV_DATA_PDF_FRAME_START;
		owned->eof = TBV_DATA_PDF_FRAME_END;
		list_add_tail(&owned->node, &frame_lists[path_idx]);
		frame_counts[path_idx]++;

		offset += payload_len;
		frag_idx++;
	} while (offset < ctx->total_len);

	{
		u32 total_frames = 0;
		u32 queued_frames = 0;
		u32 refs = 0;

		for (list_idx = 0; list_idx < path_count; list_idx++) {
			if (!frame_counts[list_idx])
				continue;
			ret = tbv_path_prepare_owned_list(
				paths[list_idx], &frame_lists[list_idx],
				&packet_lists[list_idx],
				&packet_counts[list_idx],
				TBV_PATH_SEND_DEFER, tbv_send_tx_done, ctx);
			if (ret) {
				atomic64_inc(&tqp->owner->data_wr_path_send_error);
				goto err_release_paths;
			}
			total_frames += packet_counts[list_idx];
		}

		if (WARN_ON_ONCE(!total_frames)) {
			ret = -EIO;
			goto err_release_paths;
		}

		atomic_add(total_frames, &ctx->tx_pending);
		while (refs < total_frames) {
			tbv_send_ctx_get(ctx);
			atomic64_inc(&tqp->owner->data_wr_path_send);
			refs++;
		}

		for (list_idx = 0; list_idx < path_count; list_idx++) {
			u32 released_frames = 0;
			u32 j;

			if (!packet_counts[list_idx])
				continue;

			ret = tbv_path_enqueue_prepared_reserved(
				paths[list_idx], &packet_lists[list_idx],
				packet_counts[list_idx], TBV_PATH_SEND_DEFER);
			if (!ret) {
				queued_frames += packet_counts[list_idx];
				reservations[list_idx] = 0;
				sent_any = true;
				continue;
			}

			atomic64_inc(&tqp->owner->data_wr_path_send_error);
			released_frames = packet_counts[list_idx];
			packet_counts[list_idx] = 0;
			for (j = list_idx + 1; j < path_count; j++) {
				if (!packet_counts[j])
					continue;
				tbv_path_release_prepared_list_silent(
					&packet_lists[j], ret);
				released_frames += packet_counts[j];
				packet_counts[j] = 0;
			}
			for (j = list_idx; j < path_count; j++) {
				if (!reservations[j])
					continue;
				tbv_path_release_data_reservation(paths[j],
								  reservations[j]);
				reservations[j] = 0;
			}
			if (released_frames) {
				atomic_sub(released_frames, &ctx->tx_pending);
				while (released_frames--)
					tbv_send_ctx_put(ctx);
			}
			if (!queued_frames)
				goto err_release_paths;
			pr_warn_ratelimited("native partial reserved enqueue qpn=0x%x dest_qp=0x%x psn=%u opcode=%u total=%u ret=%d queued=%u\n",
					    tqp->base.qp_num,
					    tqp->attr.dest_qp_num, ctx->psn,
					    ctx->opcode, ctx->total_len, ret,
					    queued_frames);
			ret = 0;
			break;
		}
	}

	tbv_kick_paths(paths, path_count);
	tbv_release_path_refs(paths, path_count);
	return 0;

err_release_paths:
	tbv_release_owned_frame_lists(frame_lists, ARRAY_SIZE(frame_lists));
	tbv_release_prepared_packet_lists(packet_lists, ARRAY_SIZE(packet_lists),
					  ret);
	tbv_release_path_reservations(paths, reservations, path_count);
	if (sent_any)
		tbv_kick_paths(paths, path_count);
	tbv_release_path_refs(paths, path_count);
	return ret;
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

	if (!tbv_qp_has_dest_qp(tqp))
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
		mutex_init(&ctx->data_lock);
		INIT_LIST_HEAD(&ctx->resp_frags);
		ctx->wr_id = wr->wr_id;
	ctx->signaled = !!(wr->send_flags & IB_SEND_SIGNALED);
	ctx->total_len = total_len;
	ctx->nsegs = nsegs;
	memcpy(ctx->segs, segs, sizeof(ctx->segs));
	memset(segs, 0, sizeof(segs));
	INIT_LIST_HEAD(&ctx->node);

	ret = tbv_qp_reserve_sendq(tqp);
	if (ret)
		goto err_put_ctx;
	ctx->sq_counted = true;

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
	path = tbv_select_native_data_path_for_qp_locked(tqp);
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
	if (ctx->sq_counted)
		tbv_qp_release_sendq_counted(tqp, &ctx->sq_counted);
	tbv_read_ctx_put(ctx);
	return ret;
err_release_segs:
	tbv_release_read_segments(segs, nsegs);
err_put_qp:
	atomic64_dec(&tqp->owner->data_wr_live);
	tbv_qp_put(tqp);
	return ret;
}

static void tbv_gsi_meta_put(void *buf, const union ib_gid *sgid,
			     const union ib_gid *dgid, u16 pkey_index)
{
	u8 *p = buf;

	memcpy(p + TBV_GSI_MAD_META_SGID_OFF, sgid, sizeof(*sgid));
	memcpy(p + TBV_GSI_MAD_META_DGID_OFF, dgid, sizeof(*dgid));
	tbv_wire_put_le16(p + TBV_GSI_MAD_META_PKEY_OFF, pkey_index);
	memset(p + TBV_GSI_MAD_META_PKEY_OFF + sizeof(tbv_wire_u16), 0,
	       TBV_GSI_MAD_META_SIZE - TBV_GSI_MAD_META_PKEY_OFF -
	       sizeof(tbv_wire_u16));
}

static int tbv_gsi_meta_parse(const void *payload, u32 len,
			      union ib_gid *sgid, union ib_gid *dgid,
			      u16 *pkey_index, const void **mad,
			      u32 *mad_len)
{
	const u8 *p = payload;

	if (!payload || !sgid || !dgid || !pkey_index || !mad || !mad_len)
		return -EINVAL;
	if (len < TBV_GSI_MAD_META_SIZE + sizeof(struct ib_mad))
		return -EINVAL;

	memcpy(sgid, p + TBV_GSI_MAD_META_SGID_OFF, sizeof(*sgid));
	memcpy(dgid, p + TBV_GSI_MAD_META_DGID_OFF, sizeof(*dgid));
	*pkey_index = tbv_wire_get_le16(p + TBV_GSI_MAD_META_PKEY_OFF);
	*mad = p + TBV_GSI_MAD_META_SIZE;
	*mad_len = len - TBV_GSI_MAD_META_SIZE;
	return 0;
}

static void tbv_gsi_send_done(void *ctx, int status)
{
	struct tbv_gsi_send_ctx *send = ctx;
	struct tbv_qp *tqp = send->tqp;
	struct tbv_cq *send_cq = container_of(tqp->base.send_cq,
					      struct tbv_cq, base);
	struct ib_wc wc = {};

	wc.wr_cqe = send->wr_cqe;
	wc.status = status ? IB_WC_WR_FLUSH_ERR : IB_WC_SUCCESS;
	wc.opcode = IB_WC_SEND;
	wc.qp = &tqp->base;
	wc.port_num = 1;
	tbv_cq_push(send_cq, &wc);

	tbv_qp_put(tqp);
	kfree(send);
}

static int tbv_gsi_get_sgid(struct tbv_qp *tqp, const struct ib_ah *ah,
			    const struct rdma_ah_attr *attr,
			    union ib_gid *sgid)
{
	const struct ib_global_route *grh = rdma_ah_read_grh(attr);

	if (ah->sgid_attr) {
		*sgid = ah->sgid_attr->gid;
		return 0;
	}

	return rdma_query_gid(tqp->base.device, 1, grh->sgid_index, sgid);
}

static int tbv_post_gsi_send(struct tbv_qp *tqp, const struct ib_send_wr *wr)
{
	const struct ib_ud_wr *uwr = ud_wr(wr);
	struct tbv_ah *tah;
	const struct ib_global_route *grh;
	struct tbv_native_data_header hdr = {};
	struct tbv_gsi_send_ctx *ctx = NULL;
	struct tbv_path *path;
	union ib_gid sgid;
	u32 payload_len = 0;
	u32 remote_qkey;
	u32 packet_len;
	u32 offset;
	u8 *frame = NULL;
	int len;
	int ret;
	int i;

	if (!tbv_qp_is_kernel_gsi(tqp))
		return -EOPNOTSUPP;
	if (!tbv_qp_allows_post(tqp))
		return -EINVAL;
	if (!uwr->ah || wr->opcode != IB_WR_SEND || (wr->send_flags & IB_SEND_INLINE))
		return -EOPNOTSUPP;
	if (wr->num_sge <= 0 || wr->num_sge > TBV_IBDEV_MAX_SGE)
		return -EINVAL;
	remote_qkey = tbv_gsi_effective_remote_qkey(tqp, uwr->remote_qkey);
	if (uwr->remote_qpn != TBV_GSI_QPN || remote_qkey != IB_QP1_QKEY)
		return -EINVAL;

	tah = container_of(uwr->ah, struct tbv_ah, base);
	if (tah->base.device != tqp->base.device ||
	    !(rdma_ah_get_ah_flags(&tah->attr) & IB_AH_GRH))
		return -EINVAL;

	for (i = 0; i < wr->num_sge; i++) {
		if (!tbv_kernel_dma_sge_ptr(tqp, &wr->sg_list[i]))
			return -EFAULT;
		if (check_add_overflow(payload_len, wr->sg_list[i].length,
				       &payload_len))
			return -EMSGSIZE;
	}
	if (payload_len != sizeof(struct ib_mad))
		return -EMSGSIZE;
	if (check_add_overflow(payload_len, (u32)TBV_GSI_MAD_META_SIZE,
			       &payload_len))
		return -EMSGSIZE;
	if (payload_len > TBV_NATIVE_DATA_MAX_PAYLOAD)
		return -EMSGSIZE;
	packet_len = TBV_NATIVE_DATA_HDR_SIZE + payload_len;

	ret = tbv_gsi_get_sgid(tqp, uwr->ah, &tah->attr, &sgid);
	if (ret)
		return ret;
	grh = rdma_ah_read_grh(&tah->attr);

	if (!tbv_qp_get_live(tqp))
		return -EINVAL;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ret = -ENOMEM;
		goto err_put_qp;
	}
	ctx->tqp = tqp;
	ctx->wr_cqe = wr->wr_cqe;

	frame = kmalloc(packet_len, GFP_KERNEL);
	if (!frame) {
		ret = -ENOMEM;
		goto err_free_ctx;
	}

	hdr.opcode = TBV_NATIVE_DATA_OP_MAD;
	hdr.flags = TBV_NATIVE_DATA_F_LAST;
	hdr.dest_qp = TBV_GSI_QPN;
	hdr.src_qp = TBV_GSI_QPN;
	hdr.length = payload_len;
	hdr.imm_data = sizeof(struct ib_mad);
	len = tbv_native_data_build_header(frame, packet_len, &hdr);
	if (len < 0) {
		ret = len;
		goto err_free_frame;
	}

	tbv_gsi_meta_put(frame + TBV_NATIVE_DATA_HDR_SIZE, &sgid,
			 &grh->dgid, uwr->pkey_index);
	offset = TBV_NATIVE_DATA_HDR_SIZE + TBV_GSI_MAD_META_SIZE;
	for (i = 0; i < wr->num_sge; i++) {
		const struct ib_sge *sge = &wr->sg_list[i];
		const void *src = tbv_kernel_dma_sge_ptr(tqp, sge);

		memcpy(frame + offset, src, sge->length);
		offset += sge->length;
	}

	mutex_lock(&tqp->owner->lock);
	path = tbv_select_native_data_path_for_qp_locked(tqp);
	mutex_unlock(&tqp->owner->lock);
	if (!path) {
		ret = -ENOTCONN;
		goto err_free_frame;
	}

	ret = tbv_path_send_owned(path, frame, packet_len, 0,
				  tbv_gsi_send_done, ctx);
	tbv_release_path_refs(&path, 1);
	if (ret)
		goto err_free_ctx;
	return 0;

err_free_frame:
	kfree(frame);
err_free_ctx:
	kfree(ctx);
err_put_qp:
	tbv_qp_put(tqp);
	return ret;
}

static int tbv_post_send_one(struct tbv_qp *tqp, const struct ib_send_wr *wr)
{
	struct tbv_send_segment segs[TBV_IBDEV_MAX_SGE] = {};
	struct tbv_send_ctx *ctx;
	unsigned long flags;
	u32 total_len = 0;
	u32 psn;
	int nsegs = 0;
	bool credit_consumed = false;
	bool recv_credit_required;
	bool is_send = wr->opcode == IB_WR_SEND ||
		       wr->opcode == IB_WR_SEND_WITH_IMM;
	bool send_with_imm = wr->opcode == IB_WR_SEND_WITH_IMM;
	bool is_write = wr->opcode == IB_WR_RDMA_WRITE ||
			wr->opcode == IB_WR_RDMA_WRITE_WITH_IMM;
	bool write_with_imm = wr->opcode == IB_WR_RDMA_WRITE_WITH_IMM;
	int ret;

	if (!tbv_qp_allows_post(tqp))
		return -EINVAL;

	if (tqp->type == IB_QPT_GSI)
		return tbv_post_gsi_send(tqp, wr);

	if (tbv_qp_uses_apple_transport(tqp))
		return tbv_post_apple_send(tqp, wr);

	if (wr->opcode == IB_WR_RDMA_READ)
		return tbv_post_rdma_read(tqp, wr);

	if (!is_send && !is_write) {
		atomic64_inc(&tqp->owner->data_wr_op_unsupported);
		return -EOPNOTSUPP;
	}
	if ((is_send || is_write) && !tbv_qp_has_dest_qp(tqp))
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
	if (is_write) {
		const struct ib_rdma_wr *rwr = rdma_wr(wr);
		u64 remote_end;

		if (check_add_overflow(rwr->remote_addr, (u64)total_len,
				       &remote_end)) {
			ret = -EINVAL;
			goto err_release_segs;
		}
	}

	recv_credit_required = is_send || write_with_imm;
	if (recv_credit_required) {
		ret = tbv_qp_consume_remote_recv_credit(tqp);
		if (!ret) {
			credit_consumed = true;
		} else if (ret == -EAGAIN) {
			/*
			 * Native receive credits are an advisory flow-control signal.
			 * They are not reliable enough to be a verbs admission gate:
			 * setup-time credits can legitimately race QP bring-up, and
			 * RC post_send must not fail synchronously just because the
			 * remote credit side-channel has not caught up.
			 *
			 * If the remote really has no receive resource, the receive
			 * side will either buffer within its reorder window or complete
			 * this WR asynchronously with an error ACK.
			 */
			atomic64_inc(&tqp->owner->data_wr_no_recv_credit);
			ret = 0;
		} else {
			goto err_release_segs;
		}
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
	INIT_LIST_HEAD(&ctx->retry_node);
	atomic_set(&ctx->tx_pending, 0);
	ctx->total_len = total_len;
	ctx->nsegs = nsegs;
	memcpy(ctx->segs, segs, sizeof(ctx->segs));
	memset(segs, 0, sizeof(segs));
	nsegs = 0;
	ctx->opcode = send_with_imm ? TBV_NATIVE_DATA_OP_SEND_IMM :
		      is_send ? TBV_NATIVE_DATA_OP_SEND :
		      write_with_imm ? TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM :
				       TBV_NATIVE_DATA_OP_RDMA_WRITE;
	ctx->imm_data = (send_with_imm || write_with_imm) ?
			be32_to_cpu(wr->ex.imm_data) : 0;
	ctx->solicited = !!(wr->send_flags & IB_SEND_SOLICITED);
	ctx->retryable = true;
	ctx->max_retries = tbv_qp_send_max_retries(tqp);
	ctx->max_rnr_retries = tbv_qp_send_max_rnr_retries(tqp);
	ctx->recv_credit_required = recv_credit_required;
	if (is_write) {
		const struct ib_rdma_wr *rwr = rdma_wr(wr);

		ctx->remote_addr = rwr->remote_addr;
		ctx->rkey = rwr->rkey;
	}

	ret = tbv_qp_reserve_sendq(tqp);
	if (ret)
		goto err_put_ctx;
	ctx->sq_counted = true;

	spin_lock_irqsave(&tqp->lock, flags);
	psn = tqp->send_psn & TBV_PSN_MASK;
	tqp->send_psn = tbv_psn_next(psn);
	spin_unlock_irqrestore(&tqp->lock, flags);
	ctx->psn = psn;

	tbv_qp_queue_send(tqp, ctx);
	tbv_send_ctx_get(ctx);
	ret = tbv_native_send_ctx_post_frames(ctx, TBV_SEND_POST_INITIAL);
	if (ret) {
		if (tbv_qp_unqueue_send(tqp, ctx)) {
			if (credit_consumed)
				tbv_qp_return_remote_recv_credit(tqp);
			tbv_send_ctx_put(ctx);
		}
		tbv_send_ctx_put(ctx);
		return ret;
	}
	atomic64_inc(&tqp->owner->data_tx_accepted);
	tbv_send_ctx_put(ctx);
	return 0;
err_put_ctx:
	if (ctx->sq_counted)
		tbv_qp_release_sendq_counted(tqp, &ctx->sq_counted);
	tbv_send_ctx_put(ctx);
	if (credit_consumed)
		tbv_qp_return_remote_recv_credit(tqp);
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
	if (tbv_qp_accepts_kernel_dma_lkey(tqp, sge->lkey))
		return ib_virt_dma_to_ptr(sge->addr) ? 0 : -EFAULT;
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

	if (!tbv_qp_allows_post(tqp)) {
		if (bad_wr)
			*bad_wr = wr;
		return -EINVAL;
	}

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
		if (tqp->closing || tqp->state == IB_QPS_RESET ||
		    tqp->state == IB_QPS_ERR) {
			spin_unlock_irqrestore(&tqp->lock, flags);
			ret = -EINVAL;
			goto err_bad;
		}
		if (tqp->recv_count == tqp->recvq_size) {
			spin_unlock_irqrestore(&tqp->lock, flags);
			ret = -ENOMEM;
			goto err_bad;
		}

		tbv_recv_wqe_set_wr(tqp, &tqp->recvq[tqp->recv_tail], cur);
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
		tbv_rx_drain_reorder_locked(tqp->owner, tqp, NULL);
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
	struct tbv_qp *overflow_qp = NULL;
	unsigned long flags;
	bool notify = false;
	int ret = 0;

	spin_lock_irqsave(&tcq->lock, flags);
	if (tcq->overflowed || tcq->count == tcq->cqe) {
		tcq->overflowed = true;
		if (tcq->owner)
			atomic64_inc(&tcq->owner->data_cq_overflow);
		if (tcq->notify_armed) {
			tcq->notify_armed = false;
			notify = true;
		}
		if (wc && wc->qp)
			overflow_qp = container_of(wc->qp, struct tbv_qp,
						   base);
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
	if (overflow_qp)
		tbv_qp_mark_error(overflow_qp);
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
	tbv_wc_set_recv_wr(&wc, wqe);
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

		if (status == IB_WC_LOC_LEN_ERR)
			atomic64_inc(&state->apple_rx_len_overrun);
		if (tbv_apple_rx_trace_take())
			pr_info("apple rx drain qpn=%u pending_len=%u copy_len=%u delivered=%u wqe_len=%u status=%d head=%u ready=%u recv_count=%u wr_id=%llu\n",
				tqp->base.qp_num, p->delivered, copy_len,
				delivered, wqe.length, status,
				tqp->apple_pending_head,
				tqp->apple_pending_ready_count,
				tqp->recv_count, wqe.wr_id);

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

static int tbv_apple_rx_wire_user_len(u32 len, u8 eof, u32 *user_len)
{
	if (tbv_path_apple_rx_raw_mode() && (eof == 2 || eof == 3)) {
		if (len < sizeof(__le32))
			return -EINVAL;
		*user_len = len - sizeof(__le32);
	} else {
		*user_len = len;
	}
	return 0;
}

static int tbv_apple_rx_copy_frame_to_buf(struct tbv_qp *tqp,
					  struct tbv_apple_pending_rx *p,
					  const void *payload, u32 len, u8 eof,
					  u32 *out_user_len)
{
	int ret;
	u32 copy_len;
	u32 user_len = 0;

	ret = tbv_apple_rx_wire_user_len(len, eof, &copy_len);
	if (ret)
		return ret;

	ret = tbv_apple_rx_copy_piece_to_buf(tqp, p, payload, copy_len,
					     &user_len);
	if (ret)
		return ret;
	*out_user_len = user_len;
	return 0;
}

void tbv_ibdev_rx_apple_frame(struct tbv_state *state,
			      const struct tbv_path *path,
			      const void *payload, u32 len, u8 sof, u8 eof)
{
	struct tbv_apple_pending_rx *pending;
	struct tbv_qp *tqp;
	bool raw_rx;
	u32 qpn;
	u32 user_len = 0;
	int ret;

	if (!state || !state->verbs_registered)
		return;
	if (!payload || !len || len > TBV_APPLE_FRAME_SIZE) {
		atomic64_inc(&state->data_rx_bad_frame);
		return;
	}
	raw_rx = tbv_path_apple_rx_raw_mode();

	qpn = tbv_apple_qpn_from_path(path);
	if (sof)
		atomic64_inc(&state->apple_rx_sof);
	if (eof == 3)
		atomic64_inc(&state->apple_rx_eof3);
	else
		atomic64_inc(&state->apple_rx_eof_other);

	tqp = tbv_qp_get_by_num(state, qpn);
	if (!tqp) {
		atomic64_inc(&state->data_rx_no_qp);
		return;
	}

	mutex_lock(&tqp->rx_lock);
	if (tqp->apple_pending_ready_count)
		tbv_apple_rx_drain_pending_locked(state, tqp);
	if (sof && tqp->apple_pending_active >= 0)
		atomic64_inc(&state->apple_rx_sof_while_active);
	if (tqp->apple_pending_active < 0 && !sof && !raw_rx) {
		atomic64_inc(&state->apple_rx_no_sof_when_idle);
		atomic64_inc(&state->data_rx_bad_frame);
		mutex_unlock(&tqp->rx_lock);
		tbv_qp_put(tqp);
		return;
	}

	pending = tbv_apple_pending_active_locked(state, tqp);
	if (!pending) {
		atomic64_inc(&state->data_rx_no_recv);
		atomic64_inc(&state->data_rx_reorder_dropped);
		mutex_unlock(&tqp->rx_lock);
		tbv_qp_put(tqp);
		return;
	}

	if (tbv_apple_rx_trace_take())
		pr_info("apple rx qpn=%u sof=%u eof=%u len=%u pending_len=%u recv_count=%u pending_active=%d pending_ready=%u\n",
			qpn, sof, eof, len, pending->delivered,
			tqp->recv_count, tqp->apple_pending_active,
			tqp->apple_pending_ready_count);

	ret = tbv_apple_rx_copy_frame_to_buf(tqp, pending, payload, len, eof,
					     &user_len);
	if (ret) {
		atomic64_inc(&state->data_rx_bad_frame);
		pending->status = (ret == -EMSGSIZE || ret == -ENOSPC) ?
			IB_WC_LOC_LEN_ERR : IB_WC_LOC_PROT_ERR;
	}

	if (eof == 3) {
		tbv_apple_pending_finish_locked(tqp);
		tbv_apple_rx_drain_pending_locked(state, tqp);
	} else if (!pending->active) {
		atomic64_inc(&state->apple_rx_eof_without_active);
	}
	mutex_unlock(&tqp->rx_lock);
	tbv_qp_put(tqp);
}

static int tbv_send_control_frame_on_qp(struct tbv_qp *tqp,
					struct tbv_path *rx_path,
					const void *frame, u32 len)
{
	struct tbv_state *state = tqp->owner;
	struct tbv_path *path;
	int ret;

	mutex_lock(&state->lock);
	path = tbv_select_native_control_path_for_qp_locked(tqp, rx_path);
	mutex_unlock(&state->lock);
	if (!path)
		return -ENOTCONN;

	ret = tbv_path_send(path, frame, len, TBV_PATH_SEND_CONTROL, NULL, NULL);
	tbv_rail_put(path->rail);
	return ret;
}

/*
 * QP control frames are self-identifying by QPN/PSN and do not need to preserve
 * FIFO ordering with unrelated opposite-direction payloads. Prefer the least
 * congested live native rail for the same peer so ACKs do not sit behind a long
 * data FIFO. Some callers (no-local-QP error responses) pass tqp=NULL; for
 * those, the inbound path is the only safe option.
 */
static int tbv_send_control_frame_on_path(struct tbv_qp *tqp,
					  struct tbv_path *rx_path,
					  const void *frame, u32 len)
{
	if (tqp)
		return tbv_send_control_frame_on_qp(tqp, rx_path, frame, len);

	if (rx_path && rx_path->rail && tbv_rail_data_ready(rx_path->rail))
		return tbv_path_send(rx_path, frame, len,
				     TBV_PATH_SEND_CONTROL, NULL, NULL);

	return -ENOTCONN;
}

static int tbv_send_control_frame_on_all_native_paths(struct tbv_qp *tqp,
						      struct tbv_path *rx_path,
						      const void *frame,
						      u32 len)
{
	struct tbv_path *paths[TBV_NATIVE_MAX_LANES] = {};
	struct tbv_state *state;
	u32 path_count;
	u32 i;
	int first_ret = -ENOTCONN;
	bool sent = false;

	if (!tqp || !tqp->rail || tqp->rail->peer->backend != TBV_BACKEND_NATIVE)
		return tbv_send_control_frame_on_path(tqp, rx_path, frame, len);

	state = tqp->owner;
	mutex_lock(&state->lock);
	path_count = tbv_collect_native_data_paths_for_qp_locked(
		tqp, paths, ARRAY_SIZE(paths));
	mutex_unlock(&state->lock);

	if (!path_count)
		return -ENOTCONN;

	for (i = 0; i < path_count; i++) {
		int ret = tbv_path_send(paths[i], frame, len,
					TBV_PATH_SEND_CONTROL, NULL, NULL);

		if (!ret) {
			sent = true;
			continue;
		}
		if (first_ret == -ENOTCONN)
			first_ret = ret;
	}

	tbv_release_path_refs(paths, path_count);
	return sent ? 0 : first_ret;
}

static int tbv_send_ack_on_path(struct tbv_qp *tqp,
				struct tbv_path *rx_path, u32 dest_qp,
				u32 src_qp, u32 psn, int status)
{
	struct tbv_native_data_header hdr = {};
	u8 frame[TBV_NATIVE_DATA_HDR_SIZE];
	int len;
	int ret;

	hdr.opcode = TBV_NATIVE_DATA_OP_SEND_ACK;
	hdr.dest_qp = dest_qp;
	hdr.src_qp = src_qp;
	hdr.psn = psn;
	hdr.imm_data = status;

	len = tbv_native_data_build_header(frame, sizeof(frame), &hdr);
	if (len < 0)
		return len;

	/*
	 * OK SEND ACKs are idempotent, and a lost ACK forces the peer to
	 * retransmit the whole SEND. On native multi-rail peers, put OK ACKs
	 * on every live rail; non-OK ACKs stay single-path to preserve their
	 * existing retry/error timing.
	 */
	if (status == TBV_NATIVE_SEND_ACK_OK)
		ret = tbv_send_control_frame_on_all_native_paths(tqp, rx_path,
								frame, len);
	else
		ret = tbv_send_control_frame_on_path(tqp, rx_path, frame, len);
	if (tqp && tqp->owner) {
		if (ret)
			atomic64_inc(&tqp->owner->data_tx_ack_send_error);
		else if (status == TBV_NATIVE_SEND_ACK_RNR)
			atomic64_inc(&tqp->owner->data_tx_ack_rnr);
		else if (status == TBV_NATIVE_SEND_ACK_ERROR)
			atomic64_inc(&tqp->owner->data_tx_ack_error);
		else
			atomic64_inc(&tqp->owner->data_tx_ack_ok);
	}
	return ret;
}

static int tbv_send_ack(struct tbv_qp *tqp, u32 dest_qp, u32 src_qp,
			u32 psn, int status)
{
	return tbv_send_ack_on_path(tqp, NULL, dest_qp, src_qp, psn, status);
}

static void tbv_count_tx_read_ack(struct tbv_state *state, int status)
{
	switch (status) {
	case TBV_NATIVE_READ_ACK_OK:
		atomic64_inc(&state->data_tx_read_ack_ok);
		break;
	case TBV_NATIVE_READ_ACK_RETRY:
		atomic64_inc(&state->data_tx_read_ack_retry);
		break;
	default:
		atomic64_inc(&state->data_tx_read_ack_error);
		break;
	}
}

static void tbv_count_rx_read_ack(struct tbv_state *state, u32 status)
{
	switch (status) {
	case TBV_NATIVE_READ_ACK_OK:
		atomic64_inc(&state->data_rx_read_ack_ok);
		break;
	case TBV_NATIVE_READ_ACK_RETRY:
		atomic64_inc(&state->data_rx_read_ack_retry);
		break;
	default:
		atomic64_inc(&state->data_rx_read_ack_error);
		break;
	}
}

static int tbv_send_read_ack_on_path(struct tbv_qp *tqp,
				     struct tbv_path *rx_path, u32 dest_qp,
				     u32 src_qp, u32 psn, int status)
{
	struct tbv_native_data_header hdr = {};
	u8 frame[TBV_NATIVE_DATA_HDR_SIZE];
	int len;
	int ret;

	hdr.opcode = TBV_NATIVE_DATA_OP_RDMA_READ_ACK;
	hdr.dest_qp = dest_qp;
	hdr.src_qp = src_qp;
	hdr.psn = psn;
	hdr.imm_data = status;

	len = tbv_native_data_build_header(frame, sizeof(frame), &hdr);
	if (len < 0)
		goto err_count;

	ret = tbv_send_control_frame_on_path(tqp, rx_path, frame, len);
	if (!ret) {
		tbv_count_tx_read_ack(tqp->owner, status);
		return 0;
	}

err_count:
	if (tqp && tqp->owner)
		atomic64_inc(&tqp->owner->data_tx_read_ack_error);
	return len < 0 ? len : ret;
}

static int tbv_send_read_status_on_path(struct tbv_qp *tqp,
					struct tbv_path *rx_path,
					u32 dest_qp, u32 src_qp, u32 psn,
					u32 total_len, int status)
{
	struct tbv_native_data_header hdr = {};
	u8 frame[TBV_NATIVE_DATA_HDR_SIZE];
	int len;
	int ret;

	hdr.opcode = TBV_NATIVE_DATA_OP_RDMA_READ_RESP;
	hdr.flags = TBV_NATIVE_DATA_F_LAST;
	hdr.dest_qp = dest_qp;
	hdr.src_qp = src_qp;
	hdr.psn = psn;
	hdr.imm_data = total_len;
	hdr.rkey = status ? 1 : 0;

	len = tbv_native_data_build_header(frame, sizeof(frame), &hdr);
	if (len < 0)
		goto err_count;

	ret = tbv_send_control_frame_on_path(tqp, rx_path, frame, len);
	if (!ret)
		return 0;

err_count:
	if (tqp && tqp->owner)
		atomic64_inc(&tqp->owner->data_rx_read_req_resp_error);
	return len < 0 ? len : ret;
}

static int tbv_send_recv_credit(struct tbv_qp *tqp, u32 dest_qp,
				u32 src_qp, u32 credits)
{
	struct tbv_native_data_header hdr = {};
	u8 frame[TBV_NATIVE_DATA_HDR_SIZE];
	int len;

	if (!credits)
		return 0;

	hdr.opcode = TBV_NATIVE_DATA_OP_RECV_CREDIT;
	hdr.dest_qp = dest_qp;
	hdr.src_qp = src_qp;
	hdr.imm_data = credits;

	len = tbv_native_data_build_header(frame, sizeof(frame), &hdr);
	if (len < 0)
		return len;

	/* Recv-credit is endpoint-scoped, so the control scheduler may use any
	 * live native rail for this peer.
	 */
	return tbv_send_control_frame_on_qp(tqp, NULL, frame, len);
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
	if (tqp->closing || !tqp->dest_qp_known ||
	    tqp->recv_count <= tqp->recv_credits_advertised) {
		spin_unlock_irqrestore(&tqp->lock, flags);
		return;
	}
	credits = tqp->recv_count - tqp->recv_credits_advertised;
	tqp->recv_credits_advertised += credits;
	dest_qp = tqp->attr.dest_qp_num;
	spin_unlock_irqrestore(&tqp->lock, flags);

	ret = tbv_send_recv_credit(tqp, dest_qp, tqp->base.qp_num, credits);
	if (!ret)
		return;

	atomic64_inc(&tqp->owner->data_rx_credit_send_error);

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
	if (tqp->closing || tqp->state == IB_QPS_ERR) {
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
	int ret = -EAGAIN;

	if (tbv_qp_try_consume_remote_recv_credit(tqp, &ret))
		return ret;
	return -EAGAIN;
}

static void tbv_qp_return_remote_recv_credit(struct tbv_qp *tqp)
{
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	if (!tqp->closing && tqp->state != IB_QPS_ERR)
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
	if (tqp->closing || tqp->state == IB_QPS_ERR) {
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

static bool tbv_qp_apple_tx_window_available(struct tbv_qp *tqp, u32 frames)
{
	unsigned int max_wr = READ_ONCE(apple_tx_max_inflight_wr);
	unsigned int max_frames = READ_ONCE(apple_tx_max_inflight_frames);
	u32 frame_charge = tbv_apple_tx_frame_charge(frames, max_frames);
	unsigned long flags;
	bool available;

	if (!max_wr && !max_frames)
		return true;

	spin_lock_irqsave(&tqp->lock, flags);
	if (tqp->closing || tqp->state == IB_QPS_ERR) {
		available = true;
	} else {
		int cur_wr = atomic_read(&tqp->apple_tx_inflight);
		int cur_frames = atomic_read(&tqp->apple_tx_inflight_frames);

		available = (!max_wr || cur_wr < max_wr) &&
			    (!max_frames ||
			     cur_frames + frame_charge <= max_frames);
	}
	spin_unlock_irqrestore(&tqp->lock, flags);
	return available;
}

static int tbv_qp_wait_apple_tx_window(struct tbv_qp *tqp, u32 frames,
				       bool *wr_acquired,
				       u32 *frames_acquired)
{
	int ret;

	for (;;) {
		ret = -EAGAIN;
		if (tbv_qp_try_acquire_apple_tx_window(tqp, frames,
						       wr_acquired,
						       frames_acquired,
						       &ret))
			return ret;

		wait_event_timeout(tqp->apple_tx_wait,
				   tbv_qp_apple_tx_window_available(tqp,
								     frames),
				   msecs_to_jiffies(100));
	}
}

static bool tbv_qp_apple_sq_stopping(struct tbv_qp *tqp)
{
	unsigned long flags;
	bool stopping;

	spin_lock_irqsave(&tqp->lock, flags);
	stopping = tqp->closing || tqp->state == IB_QPS_ERR;
	spin_unlock_irqrestore(&tqp->lock, flags);
	return stopping;
}

static int tbv_apple_sq_get_tx_resources(struct tbv_qp *tqp,
					 u32 window_frames,
					 u32 reserve_frames,
					 struct tbv_path **path_out,
					 bool *wr_acquired,
					 u32 *frames_acquired)
{
	struct tbv_path *path;
	int ret;

	*path_out = NULL;
	*wr_acquired = false;
	*frames_acquired = 0;

	for (;;) {
		if (tbv_qp_apple_sq_stopping(tqp))
			return -ECANCELED;

		mutex_lock(&tqp->owner->lock);
		path = tbv_first_active_apple_path_for_qp_locked(tqp);
		mutex_unlock(&tqp->owner->lock);
		if (!path) {
			atomic64_inc(&tqp->owner->data_wr_no_path);
			return -ENOTCONN;
		}

		ret = tbv_qp_wait_apple_tx_window(tqp, window_frames,
						  wr_acquired,
						  frames_acquired);
		if (ret) {
			tbv_release_path_refs(&path, 1);
			return ret;
		}

		ret = tbv_path_reserve_data(path, reserve_frames);
		if (!ret) {
			*path_out = path;
			return 0;
		}

		tbv_qp_release_apple_tx_window(tqp, *wr_acquired,
					       *frames_acquired);
		*wr_acquired = false;
		*frames_acquired = 0;
		tbv_release_path_refs(&path, 1);
		if (ret != -ENOMEM)
			return ret;
		msleep(1);
	}
}

static int tbv_apple_raw_desc_count(u32 len, u32 *desc_count)
{
	u32 chunks;

	if (!len || len % TBV_APPLE_FRAME_SIZE)
		return -EMSGSIZE;

	chunks = len / TBV_APPLE_FRAME_SIZE;
	if (chunks > U32_MAX / TBV_APPLE_RAW_DESCS_PER_CHUNK)
		return -EMSGSIZE;

	*desc_count = chunks * TBV_APPLE_RAW_DESCS_PER_CHUNK;
	return 0;
}

static int tbv_post_apple_send_raw_piece(struct tbv_qp *tqp,
					 struct tbv_path *path,
					 struct tbv_send_ctx *ctx,
					 const u8 *payload,
					 u32 payload_len, u32 wire_len,
					 u8 sof, u8 eof, bool append_crc,
					 u32 crc, u32 *remaining,
					 u32 *posted)
{
	int ret;

	ret = tbv_post_apple_send_piece(tqp, path, ctx, payload, payload_len,
					wire_len, sof, eof, append_crc, crc,
					tbv_apple_send_tx_done, ctx);
	if (ret)
		return ret;

	(*remaining)--;
	(*posted)++;
	return 0;
}

static int tbv_apple_sq_transmit_raw_chunk(struct tbv_qp *tqp,
					   struct tbv_path *path,
					   struct tbv_send_ctx *ctx,
					   const u8 *payload, u32 base,
					   u8 tail_eof, u32 *remaining,
					   u32 *posted)
{
	u32 crc = crc32c(~0u, payload + base, TBV_APPLE_FRAME_SIZE) ^ ~0u;
	u32 piece;
	int ret;

	for (piece = 0; piece < 15; piece++) {
		ret = tbv_post_apple_send_raw_piece(
			tqp, path, ctx,
			payload + base + piece * TBV_APPLE_RAW_SLOT_USER_SIZE,
			TBV_APPLE_RAW_SLOT_USER_SIZE,
			TBV_APPLE_RAW_SLOT_USER_SIZE,
			0, piece == 0 ? 1 : 0, false, 0, remaining, posted);
		if (ret)
			return ret;
	}

	ret = tbv_post_apple_send_raw_piece(
		tqp, path, ctx, payload + base + 15 * TBV_APPLE_RAW_SLOT_USER_SIZE,
		TBV_APPLE_RAW_SPLIT_USER_SIZE, TBV_APPLE_RAW_SPLIT_USER_SIZE,
		0, 0, false, 0, remaining, posted);
	if (ret)
		return ret;

	return tbv_post_apple_send_raw_piece(
		tqp, path, ctx,
		payload + base + 15 * TBV_APPLE_RAW_SLOT_USER_SIZE +
			TBV_APPLE_RAW_SPLIT_USER_SIZE,
		TBV_APPLE_RAW_TAIL_USER_SIZE, TBV_APPLE_RAW_TAIL_USER_SIZE + 4,
		0, tail_eof, true, crc, remaining, posted);
}

static int tbv_apple_sq_wait_frame_group(struct tbv_qp *tqp,
					 struct tbv_send_ctx *ctx,
					 u32 target_pending)
{
	for (;;) {
		if (atomic_read(&ctx->apple_pending) <= target_pending)
			return 0;
		if (tbv_send_is_completed(ctx) || tbv_qp_apple_sq_stopping(tqp))
			return -ECANCELED;

		wait_event_timeout(tqp->apple_tx_wait,
				   atomic_read(&ctx->apple_pending) <= target_pending ||
					   tbv_send_is_completed(ctx) ||
					   tbv_qp_apple_sq_stopping(tqp),
				   msecs_to_jiffies(100));
	}
}

static int tbv_apple_sq_transmit(struct tbv_qp *tqp,
				 struct tbv_apple_sq_entry *entry)
{
	struct tbv_send_ctx *ctx = entry->send;
	struct tbv_path *path = NULL;
	bool apple_wr_acquired = false;
	u32 apple_frames_acquired = 0;
	u32 nfrags = DIV_ROUND_UP(entry->length, TBV_APPLE_FRAME_SIZE);
	u32 pending_descs = nfrags;
	u32 remaining = nfrags;
	u32 offset = 0;
	u32 posted = 0;
	u32 group_posted = 0;
	u32 group_limit = READ_ONCE(apple_tx_max_inflight_frames);
	bool raw_mode = tbv_path_apple_tx_raw_mode();
	bool sent_any = false;
	int ret;

	if (raw_mode) {
		ret = tbv_apple_raw_desc_count(entry->length, &pending_descs);
		if (ret)
			return ret;
		remaining = pending_descs;
	}

	ret = tbv_apple_sq_get_tx_resources(tqp, nfrags, pending_descs, &path,
					    &apple_wr_acquired,
					    &apple_frames_acquired);
	if (ret)
		return ret;

	ctx->apple_window_wr_acquired = apple_wr_acquired;
	ctx->apple_window_frames = apple_frames_acquired;
	ctx->apple_window_acquired = apple_wr_acquired ||
				     apple_frames_acquired;
	atomic_set(&ctx->apple_pending, pending_descs);
	tbv_qp_arm_send_timeout(tqp, ctx);

	while (offset < entry->length) {
		u32 payload_len = min_t(u32, entry->length - offset,
					TBV_APPLE_FRAME_SIZE);
		bool last = offset + payload_len == entry->length;

		if (raw_mode) {
			ret = tbv_apple_sq_transmit_raw_chunk(
				tqp, path, ctx, entry->payload, offset,
				last ? 3 : 2, &remaining, &posted);
		} else {
			ret = tbv_post_apple_send_frame(
				tqp, path, ctx, (u8 *)entry->payload + offset,
				payload_len, 1, last ? 3 : 2,
				tbv_apple_send_tx_done, ctx);
			if (!ret) {
				remaining--;
				posted++;
			}
		}
		if (ret)
			goto err_release_reservation;

		group_posted++;
		sent_any = true;
		offset += payload_len;

		if (group_limit && group_posted >= group_limit &&
		    offset < entry->length) {
			tbv_path_kick_tx(path);
			ret = tbv_apple_sq_wait_frame_group(tqp, ctx,
							    pending_descs -
								    posted);
			if (ret)
				goto err_release_reservation;
			group_posted = 0;
		}
	}

	tbv_path_kick_tx(path);
	tbv_release_path_refs(&path, 1);
	atomic64_inc(&tqp->owner->apple_sq_dequeued);
	return 0;

err_release_reservation:
	tbv_path_release_data_reservation(path, remaining);
	if (sent_any)
		tbv_path_kick_tx(path);
	tbv_release_path_refs(&path, 1);
	return ret;
}

static void tbv_apple_sq_work(struct work_struct *work)
{
	struct tbv_qp *tqp = container_of(work, struct tbv_qp, apple_sq_work);
	struct tbv_apple_sq_entry *entry;

	while ((entry = tbv_apple_sq_pop(tqp))) {
		struct tbv_send_ctx *send = entry->send;
		int ret;

		tbv_send_ctx_get(send);
		ret = tbv_apple_sq_transmit(tqp, entry);
		tbv_send_ctx_put(send);
		if (ret) {
			if (tbv_qp_unqueue_send(tqp, send)) {
				tbv_send_complete(send, ret);
				tbv_send_ctx_put(send);
			}
			atomic64_inc(&tqp->owner->data_wr_path_send_error);
		}
		tbv_apple_sq_free_entry(entry);
	}
}

static void tbv_qp_flush_apple_sq(struct tbv_qp *tqp)
{
	LIST_HEAD(flush);
	unsigned long flags;

	spin_lock_irqsave(&tqp->lock, flags);
	list_splice_init(&tqp->apple_sq, &flush);
	spin_unlock_irqrestore(&tqp->lock, flags);

	while (!list_empty(&flush)) {
		struct tbv_apple_sq_entry *entry =
			list_first_entry(&flush, struct tbv_apple_sq_entry,
					 node);
		struct tbv_send_ctx *send = entry->send;

		list_del_init(&entry->node);
		atomic64_inc(&tqp->owner->apple_sq_flushed);
		if (tbv_qp_unqueue_send(tqp, send)) {
			tbv_send_complete(send, -ECANCELED);
			tbv_send_ctx_put(send);
		}
		tbv_apple_sq_free_entry(entry);
	}
}

static int tbv_umem_copy_to(struct tbv_mr *mr, u64 addr, const void *src,
			    size_t len)
{
	struct sg_table *sgt = &mr->umem->sgt_append.sgt;
	size_t offset;
	size_t copied;
	u64 mr_end;
	u64 end;

	if (!len)
		return 0;
	if (mr->dma_mr)
		return -EOPNOTSUPP;
	if (check_add_overflow(addr, (u64)len, &end))
		return -EINVAL;
	if (check_add_overflow(mr->start, mr->length, &mr_end))
		return -EINVAL;
	if (addr < mr->start || end > mr_end)
		return -EFAULT;

	offset = ib_umem_offset(mr->umem) + addr - mr->start;
	copied = sg_pcopy_from_buffer(sgt->sgl, sgt->orig_nents, src, len,
				      offset);
	return copied == len ? 0 : -EFAULT;
}

static int tbv_umem_copy_to_iova(struct tbv_mr *mr, u64 iova,
				 const void *src, size_t len)
{
	u64 iova_end;
	u64 mr_iova_end;
	u64 addr;

	if (!len)
		return 0;
	if (mr->dma_mr)
		return -EOPNOTSUPP;
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

	if (mr->dma_mr)
		return -EOPNOTSUPP;
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
	if (mr->dma_mr)
		return -EOPNOTSUPP;
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

static struct tbv_rx_reorder_frag *
tbv_read_resp_find_frag_locked(struct tbv_read_ctx *read, u32 offset)
{
	struct tbv_rx_reorder_frag *frag;

	list_for_each_entry(frag, &read->resp_frags, node) {
		if (frag->offset == offset)
			return frag;
	}

	return NULL;
}

static int tbv_read_resp_store_frag_locked(struct tbv_read_ctx *read,
					   u32 offset, const void *payload,
					   u32 len)
{
	struct tbv_rx_reorder_frag *frag;
	struct tbv_rx_reorder_frag *pos;
	u32 new_bytes;

	frag = tbv_read_resp_find_frag_locked(read, offset);
	if (frag) {
		if (frag->len != len ||
		    (len && memcmp(frag->data, payload, len)))
			return -EIO;
		return 0;
	}

	if (check_add_overflow(read->resp_buffered_bytes, len, &new_bytes) ||
	    new_bytes > read->total_len)
		return -ENOSPC;

	frag = kmalloc(struct_size(frag, data, len), GFP_ATOMIC);
	if (!frag)
		return -ENOMEM;
	INIT_LIST_HEAD(&frag->node);
	frag->offset = offset;
	frag->len = len;
	if (len)
		memcpy(frag->data, payload, len);

	list_for_each_entry(pos, &read->resp_frags, node) {
		if (offset < pos->offset) {
			list_add_tail(&frag->node, &pos->node);
			read->resp_buffered_bytes = new_bytes;
			return 0;
		}
	}
	list_add_tail(&frag->node, &read->resp_frags);
	read->resp_buffered_bytes = new_bytes;
	return 0;
}

static int tbv_read_resp_drain_frags_locked(struct tbv_read_ctx *read)
{
	struct tbv_rx_reorder_frag *frag;
	int ret;

	for (;;) {
		frag = tbv_read_resp_find_frag_locked(read, read->received);
		if (!frag)
			return 0;

		ret = tbv_copy_to_read_segments(read, frag->offset, frag->data,
						frag->len);
		if (ret)
			return ret;
		read->received = frag->offset + frag->len;
		if (read->resp_buffered_bytes >= frag->len)
			read->resp_buffered_bytes -= frag->len;
		else
			read->resp_buffered_bytes = 0;
		list_del(&frag->node);
		kfree(frag);
	}
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
	struct tbv_rx_reorder_frag *frag;
	struct tbv_rx_reorder_frag *tmp;

	if (!msg)
		return;
	list_for_each_entry_safe(frag, tmp, &msg->frags, node) {
		list_del(&frag->node);
		kfree(frag);
	}
	kfree(msg);
}

static void tbv_qp_flush_reorder(struct tbv_qp *tqp)
{
	struct tbv_rx_reorder_msg *msg;
	struct tbv_rx_reorder_msg *tmp;
	u32 discarded = 0;

	list_for_each_entry_safe(msg, tmp, &tqp->rx_reorder, node) {
		pr_warn_ratelimited("native RX reorder discarded qpn=0x%x expected_psn=%u psn=%u kind=%u complete=%u received=%u frags=%u/%u bytes=%u\n",
				    tqp->base.qp_num, tqp->rx_expected_psn,
				    msg->psn, msg->kind, msg->complete,
				    msg->received, msg->frags_received,
				    msg->frag_count, msg->buffered_bytes);
		list_del(&msg->node);
		tbv_rx_reorder_free_msg(msg);
		discarded++;
	}
	tqp->rx_reorder_count = 0;
	tqp->rx_reorder_bytes = 0;
	if (discarded && tqp->owner)
		atomic64_add(discarded, &tqp->owner->data_rx_pending_discarded);
}

static void tbv_qp_flush_active_rx(struct tbv_qp *tqp)
{
	u32 discarded = 0;

	if (tqp->rx_msg.active) {
		pr_warn_ratelimited("native RX active SEND discarded qpn=0x%x expected_psn=%u psn=%u src_qp=0x%x received=%u total=%u frags=%u/%u\n",
				    tqp->base.qp_num, tqp->rx_expected_psn,
				    tqp->rx_msg.psn, tqp->rx_msg.src_qp,
				    tqp->rx_msg.received, tqp->rx_msg.total_len,
				    tqp->rx_msg.frags_received,
				    tqp->rx_msg.frag_count);
		memset(&tqp->rx_msg, 0, sizeof(tqp->rx_msg));
		discarded++;
	}
	if (tqp->rx_write.active) {
		pr_warn_ratelimited("native RX active WRITE discarded qpn=0x%x expected_psn=%u psn=%u src_qp=0x%x received=%u base=0x%llx with_imm=%u\n",
				    tqp->base.qp_num, tqp->rx_expected_psn,
				    tqp->rx_write.psn, tqp->rx_write.src_qp,
				    tqp->rx_write.received,
				    tqp->rx_write.remote_addr,
				    tqp->rx_write.with_imm);
		memset(&tqp->rx_write, 0, sizeof(tqp->rx_write));
		discarded++;
	}
	if (discarded && tqp->owner)
		atomic64_add(discarded, &tqp->owner->data_rx_pending_discarded);
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

static bool tbv_rx_fragment_shape_max(u32 total_len, u32 max_payload,
				      u32 offset, u32 len, bool last,
				      u32 *frag_idx, u32 *frag_count)
{
	return !tbv_native_data_fragment_shape(total_len, max_payload,
					       TBV_RX_REORDER_MAX_FRAGS,
					       offset, len, last, frag_idx,
					       frag_count);
}

static bool tbv_rx_fragment_shape(u32 total_len, u32 offset, u32 len,
				  bool last, u32 *frag_idx, u32 *frag_count)
{
	return tbv_rx_fragment_shape_max(total_len, TBV_NATIVE_DATA_MAX_PAYLOAD,
					 offset, len, last, frag_idx,
					 frag_count);
}

static bool tbv_rx_write_fragment_shape(u32 total_len, u32 offset, u32 len,
					bool last, u32 *frag_idx,
					u32 *frag_count)
{
	if (tbv_rx_fragment_shape(total_len, offset, len, last, frag_idx,
				  frag_count))
		return true;

	return tbv_rx_fragment_shape_max(total_len, TBV_NATIVE_DATA_FRAME_SIZE,
					 offset, len, last, frag_idx,
					 frag_count);
}

static void tbv_rx_reorder_unlink_msg_locked(struct tbv_qp *tqp,
					     struct tbv_rx_reorder_msg *msg)
{
	list_del(&msg->node);
	tqp->rx_reorder_count--;
	if (tqp->rx_reorder_bytes >= msg->buffered_bytes)
		tqp->rx_reorder_bytes -= msg->buffered_bytes;
	else
		tqp->rx_reorder_bytes = 0;
}

static int tbv_rx_reorder_store_fragment_locked(struct tbv_qp *tqp,
						struct tbv_rx_reorder_msg *msg,
						u32 offset, const void *payload,
						u32 len)
{
	struct tbv_rx_reorder_frag *frag;
	u32 new_bytes;

	if (!len)
		return 0;

	if (check_add_overflow(tqp->rx_reorder_bytes, len, &new_bytes) ||
	    new_bytes > TBV_RX_REORDER_MAX_BYTES)
		return -ENOSPC;

	frag = kmalloc(struct_size(frag, data, len), GFP_KERNEL);
	if (!frag)
		return -ENOMEM;

	INIT_LIST_HEAD(&frag->node);
	frag->offset = offset;
	frag->len = len;
	memcpy(frag->data, payload, len);
	list_add_tail(&frag->node, &msg->frags);

	msg->buffered_bytes += len;
	tqp->rx_reorder_bytes = new_bytes;
	return 0;
}

static bool tbv_rx_reorder_fragment_matches_locked(
	const struct tbv_rx_reorder_msg *msg, u32 offset, const void *payload,
	u32 len)
{
	struct tbv_rx_reorder_frag *frag;

	if (!len)
		return true;

	list_for_each_entry(frag, &msg->frags, node) {
		if (frag->offset != offset)
			continue;
		return frag->len == len && !memcmp(frag->data, payload, len);
	}

	return false;
}

static void tbv_rx_record_send_error(struct tbv_state *state,
				     const char *source, struct tbv_qp *tqp,
				     u32 src_qp, u32 psn, int status,
				     bool cq_error, u32 total_len, u32 wqe_len,
				     u32 received, u32 delivered,
				     u32 last_offset, u32 last_len)
{
	if (cq_error)
		atomic64_inc(&state->data_rx_send_cq_error);

	switch (status) {
	case IB_WC_SUCCESS:
		break;
	case IB_WC_LOC_LEN_ERR:
		atomic64_inc(&state->data_rx_send_len_error);
		break;
	case IB_WC_LOC_PROT_ERR:
		atomic64_inc(&state->data_rx_send_prot_error);
		break;
	default:
		atomic64_inc(&state->data_rx_send_prot_error);
		break;
	}

	pr_warn_ratelimited("native SEND %s error qpn=0x%x src_qp=0x%x psn=%u status=%d cq_error=%d total=%u wqe_len=%u received=%u delivered=%u last_offset=%u last_len=%u\n",
			    source, tqp->base.qp_num, src_qp, psn & TBV_PSN_MASK,
			    status, cq_error, total_len, wqe_len, received,
			    delivered, last_offset, last_len);
}

static void tbv_rx_send_error_ack(struct tbv_state *state, struct tbv_qp *tqp,
				  struct tbv_path *rx_path,
				  const struct tbv_native_data_header *hdr,
				  u32 psn, const char *reason, bool sequence)
{
	if (sequence)
		atomic64_inc(&state->data_rx_send_sequence_error);
	else
		atomic64_inc(&state->data_rx_send_bad_fragment);

	pr_warn_ratelimited("native SEND %s qpn=0x%x src_qp=0x%x psn=%u opcode=%u total=%u offset=%llu len=%u flags=0x%x expected_psn=%u\n",
			    reason, tqp->base.qp_num, hdr->src_qp,
			    psn & TBV_PSN_MASK, hdr->opcode, hdr->imm_data,
			    hdr->remote_addr, hdr->length, hdr->flags,
			    tqp->rx_expected_psn);
	tbv_send_ack_on_path(tqp, rx_path, hdr->src_qp, hdr->dest_qp, psn,
			     TBV_NATIVE_SEND_ACK_ERROR);
}

static void tbv_rx_finish_send(struct tbv_state *state, struct tbv_qp *tqp,
			       struct tbv_path *rx_path)
{
	struct tbv_rx_message *msg = &tqp->rx_msg;
	struct tbv_cq *recv_cq = container_of(tqp->base.recv_cq,
					      struct tbv_cq, base);
	struct ib_wc wc = {};
	u32 src_qp = msg->src_qp;
	u32 psn = msg->psn;
	int ack_status = msg->status == IB_WC_SUCCESS ?
			 TBV_NATIVE_SEND_ACK_OK : TBV_NATIVE_SEND_ACK_ERROR;
	bool cq_error = false;

	tbv_wc_set_recv_wr(&wc, &msg->wqe);
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
	if (tbv_cq_push(recv_cq, &wc)) {
		ack_status = TBV_NATIVE_SEND_ACK_ERROR;
		cq_error = true;
	}

	if (msg->status != IB_WC_SUCCESS || cq_error)
		tbv_rx_record_send_error(state, "completion", tqp, src_qp, psn,
					 msg->status, cq_error, msg->total_len,
					 msg->wqe.length, msg->received,
					 msg->delivered, msg->last_offset,
					 msg->last_len);

	memset(msg, 0, sizeof(*msg));
	tqp->rx_expected_psn = tbv_psn_next(psn);
	tbv_qp_ack_history_store_locked(tqp, psn, ack_status);
	tbv_send_ack_on_path(tqp, rx_path, src_qp, tqp->base.qp_num, psn,
			     ack_status);
}

static void tbv_rx_note_active_path(struct tbv_rx_message *msg,
				    struct tbv_path *rx_path, u32 offset,
				    u32 len)
{
	u32 rail_id = 0;
	u64 route = 0;
	u32 path_id = 0;

	if (rx_path && rx_path->rail) {
		rail_id = rx_path->rail->rail_id;
		route = rx_path->rail->key.route;
		path_id = rx_path->rail->key.path_id;
	}

	if (!msg->received) {
		msg->first_rail_id = rail_id;
		msg->first_route = route;
		msg->first_path_id = path_id;
	}

	msg->last_rail_id = rail_id;
	msg->last_route = route;
	msg->last_path_id = path_id;
	msg->last_offset = offset;
	msg->last_len = len;
}

static void tbv_rx_fail_active_send(struct tbv_state *state, struct tbv_qp *tqp,
				    struct tbv_path *rx_path,
				    enum ib_wc_status status)
{
	if (!tqp->rx_msg.active)
		return;
	tqp->rx_msg.status = status;
	tbv_rx_finish_send(state, tqp, rx_path);
}

static void tbv_qp_flush_recv_wqes(struct tbv_qp *tqp)
{
	struct tbv_cq *recv_cq;

	if (!tqp->base.recv_cq)
		return;

	recv_cq = container_of(tqp->base.recv_cq, struct tbv_cq, base);
	for (;;) {
		struct tbv_recv_wqe wqe;
		struct ib_wc wc = {};
		unsigned long flags;

		spin_lock_irqsave(&tqp->lock, flags);
		if (!tqp->recv_count || !tqp->recvq) {
			spin_unlock_irqrestore(&tqp->lock, flags);
			return;
		}

		wqe = tqp->recvq[tqp->recv_head];
		memset(&tqp->recvq[tqp->recv_head], 0,
		       sizeof(tqp->recvq[tqp->recv_head]));
		tqp->recv_head = (tqp->recv_head + 1) % tqp->recvq_size;
		tqp->recv_count--;
		if (tqp->recv_credits_advertised)
			tqp->recv_credits_advertised--;
		if (tqp->owner)
			atomic_dec(&tqp->owner->verbs_recv_wqes);
		spin_unlock_irqrestore(&tqp->lock, flags);

		tbv_wc_set_recv_wr(&wc, &wqe);
		wc.status = IB_WC_WR_FLUSH_ERR;
		wc.opcode = IB_WC_RECV;
		wc.qp = &tqp->base;
		wc.port_num = 1;
		tbv_cq_push(recv_cq, &wc);
	}
}

static void tbv_qp_flush_error(struct tbv_qp *tqp)
{
	LIST_HEAD(flush);

	wake_up_all(&tqp->credit_wait);
	wake_up_all(&tqp->apple_tx_wait);
	tbv_qp_flush_apple_sq(tqp);

	tbv_qp_flush_sends(tqp, &flush);
	while (!list_empty(&flush)) {
		struct tbv_send_ctx *send =
			list_first_entry(&flush, struct tbv_send_ctx, node);
		int status = send->completion_status;

		list_del_init(&send->node);
		tbv_cancel_send_ctx_packets(send);
		tbv_send_complete(send, status);
		tbv_send_ctx_put(send);
	}

	tbv_qp_flush_reads(tqp, &flush);
	while (!list_empty(&flush)) {
		struct tbv_read_ctx *read =
			list_first_entry(&flush, struct tbv_read_ctx, node);

		list_del_init(&read->node);
		tbv_cancel_read_ctx_packets(read);
		tbv_read_complete(read, -ECANCELED);
		tbv_read_ctx_put(read);
	}

	tbv_qp_cancel_read_resps(tqp, &flush);
	while (!list_empty(&flush)) {
		struct tbv_read_resp_ctx *ctx =
			list_first_entry(&flush, struct tbv_read_resp_ctx, node);

		list_del_init(&ctx->node);
		tbv_read_resp_ctx_put(ctx);
	}

	mutex_lock(&tqp->rx_lock);
	tbv_rx_fail_active_send(tqp->owner, tqp, NULL, IB_WC_WR_FLUSH_ERR);
	tbv_rx_fail_active_write_locked(tqp->owner, tqp, NULL,
					    IB_WC_WR_FLUSH_ERR);
	tbv_qp_flush_reorder(tqp);
	mutex_unlock(&tqp->rx_lock);

	tbv_qp_flush_recv_wqes(tqp);
}

static bool tbv_rx_deliver_reorder_msg_locked(struct tbv_state *state,
					      struct tbv_qp *tqp,
					      struct tbv_path *rx_path,
					      struct tbv_rx_reorder_msg *msg)
{
	struct tbv_rx_reorder_frag *frag;
	struct tbv_cq *recv_cq = container_of(tqp->base.recv_cq,
					      struct tbv_cq, base);
	struct tbv_recv_wqe wqe;
	struct ib_wc wc = {};
	u32 delivered = 0;
	int ack_status = 0;
	int status;
	bool cq_error = false;

	if (!tbv_qp_pop_recv(tqp, &wqe)) {
		atomic64_inc(&state->data_rx_rnr);
		tbv_send_ack_on_path(tqp, rx_path, msg->src_qp,
				     tqp->base.qp_num, msg->psn,
				     TBV_NATIVE_SEND_ACK_RNR);
		return false;
	}

	tbv_rx_reorder_unlink_msg_locked(tqp, msg);

	status = msg->total_len > wqe.length ? IB_WC_LOC_LEN_ERR :
					       IB_WC_SUCCESS;
	list_for_each_entry(frag, &msg->frags, node) {
		if (tbv_rx_copy_to_wqe(state, &wqe, frag->offset, frag->data,
				       frag->len, &delivered)) {
			status = IB_WC_LOC_PROT_ERR;
			break;
		}
	}

	tbv_wc_set_recv_wr(&wc, &wqe);
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
	if (tbv_cq_push(recv_cq, &wc)) {
		ack_status = TBV_NATIVE_SEND_ACK_ERROR;
		cq_error = true;
	}
	if (status != IB_WC_SUCCESS)
		ack_status = TBV_NATIVE_SEND_ACK_ERROR;
	if (status != IB_WC_SUCCESS || cq_error)
		tbv_rx_record_send_error(state, "reorder", tqp, msg->src_qp,
					 msg->psn, status, cq_error,
					 msg->total_len, wqe.length,
					 msg->received, delivered, 0, 0);

	tqp->rx_expected_psn = tbv_psn_next(msg->psn);
	tbv_qp_ack_history_store_locked(tqp, msg->psn, ack_status);
	tbv_send_ack_on_path(tqp, rx_path, msg->src_qp, tqp->base.qp_num,
			     msg->psn, ack_status);
	atomic64_inc(&state->data_rx_reorder_delivered);
	tbv_rx_reorder_free_msg(msg);
	return true;
}

static bool tbv_rx_deliver_reorder_write_locked(struct tbv_state *state,
						struct tbv_qp *tqp,
						struct tbv_path *rx_path,
						struct tbv_rx_reorder_msg *msg)
{
	struct tbv_rx_reorder_frag *frag;
	struct tbv_mr *mr;
	struct ib_wc wc = {};
	enum ib_wc_status status = IB_WC_SUCCESS;
	int ack_status = TBV_NATIVE_SEND_ACK_OK;
	bool cq_error = false;
	int ret = 0;

	if (msg->with_imm && !tbv_qp_pop_recv(tqp, &tqp->rx_write.imm_wqe)) {
		atomic64_inc(&state->data_rx_rnr);
		tbv_send_ack_on_path(tqp, rx_path, msg->src_qp,
				     tqp->base.qp_num, msg->psn,
				     TBV_NATIVE_SEND_ACK_RNR);
		return false;
	}

	tbv_rx_reorder_unlink_msg_locked(tqp, msg);

	mr = tbv_mr_get(state, msg->rkey);
	if (!mr) {
		atomic64_inc(&state->data_rx_copy_error);
		status = IB_WC_LOC_PROT_ERR;
		ret = -EINVAL;
	} else if (!(mr->access & IB_ACCESS_REMOTE_WRITE)) {
		atomic64_inc(&state->data_rx_copy_error);
		status = IB_WC_LOC_PROT_ERR;
		ret = -EACCES;
	} else {
		list_for_each_entry(frag, &msg->frags, node) {
			u64 copy_addr;

			if (check_add_overflow(msg->remote_addr,
					       (u64)frag->offset,
					       &copy_addr)) {
				atomic64_inc(&state->data_rx_copy_error);
				status = IB_WC_LOC_PROT_ERR;
				ret = -EINVAL;
				break;
			}
			ret = tbv_umem_copy_to_iova(mr, copy_addr, frag->data,
						    frag->len);
			if (ret) {
				atomic64_inc(&state->data_rx_copy_error);
				status = IB_WC_LOC_PROT_ERR;
				break;
			}
		}
	}
	if (mr)
		tbv_mr_put(mr);

	if (msg->with_imm) {
		struct tbv_cq *recv_cq = container_of(tqp->base.recv_cq,
						      struct tbv_cq, base);

		tbv_wc_set_recv_wr(&wc, &tqp->rx_write.imm_wqe);
		wc.status = status;
		wc.opcode = status == IB_WC_SUCCESS ?
				    IB_WC_RECV_RDMA_WITH_IMM :
				    IB_WC_RECV;
		wc.qp = &tqp->base;
		wc.src_qp = msg->src_qp;
		wc.pkey_index = 0;
		wc.port_num = 1;
		if (status == IB_WC_SUCCESS) {
			wc.wc_flags = IB_WC_WITH_IMM;
			wc.ex.imm_data = cpu_to_be32(msg->imm_data);
		}
		if (tbv_cq_push(recv_cq, &wc)) {
			cq_error = true;
			status = IB_WC_GENERAL_ERR;
			ret = -ENOSPC;
		}
		memset(&tqp->rx_write.imm_wqe, 0, sizeof(tqp->rx_write.imm_wqe));
	}

	if (status != IB_WC_SUCCESS || cq_error)
		ack_status = TBV_NATIVE_SEND_ACK_ERROR;

	tqp->rx_expected_psn = tbv_psn_next(msg->psn);
	tbv_qp_ack_history_store_locked(tqp, msg->psn, ack_status);
	tbv_send_ack_on_path(tqp, rx_path, msg->src_qp, tqp->base.qp_num,
			     msg->psn, ack_status);
	atomic64_inc(&state->data_rx_reorder_delivered);
	tbv_rx_reorder_free_msg(msg);
	return ret != -EAGAIN;
}

static void tbv_rx_prune_stale_reorder_locked(struct tbv_state *state,
					      struct tbv_qp *tqp,
					      struct tbv_path *rx_path)
{
	struct tbv_rx_reorder_msg *msg;
	struct tbv_rx_reorder_msg *tmp;

	list_for_each_entry_safe(msg, tmp, &tqp->rx_reorder, node) {
		if (tbv_psn_delta(msg->psn, tqp->rx_expected_psn) >= 0)
			continue;

		if (msg->kind == TBV_RX_REORDER_READ_REQ)
			tbv_qp_retry_read_resp(tqp, msg->psn);
		else
			tbv_rx_reack_duplicate_locked(state, tqp, rx_path,
						      msg->src_qp,
						      tqp->base.qp_num,
						      msg->psn);
		tbv_rx_reorder_unlink_msg_locked(tqp, msg);
		tbv_rx_reorder_free_msg(msg);
	}
}

static bool tbv_rx_deliver_reorder_read_req_locked(
	struct tbv_state *state, struct tbv_qp *tqp, struct tbv_path *rx_path,
	struct tbv_rx_reorder_msg *msg)
{
	struct tbv_native_data_header hdr = {};

	hdr.opcode = TBV_NATIVE_DATA_OP_RDMA_READ_REQ;
	hdr.flags = TBV_NATIVE_DATA_F_LAST;
	hdr.dest_qp = tqp->base.qp_num;
	hdr.src_qp = msg->src_qp;
	hdr.psn = msg->psn;
	hdr.imm_data = msg->total_len;
	hdr.remote_addr = msg->remote_addr;
	hdr.rkey = msg->rkey;

	tbv_rx_reorder_unlink_msg_locked(tqp, msg);
	tqp->rx_expected_psn = tbv_psn_next(msg->psn);
	tbv_rx_queue_rdma_read_req_work(state, tqp, &hdr, rx_path);
	atomic64_inc(&state->data_rx_reorder_delivered);
	tbv_rx_reorder_free_msg(msg);
	return true;
}

static void tbv_rx_drain_reorder_locked(struct tbv_state *state,
					struct tbv_qp *tqp,
					struct tbv_path *rx_path)
{
	struct tbv_rx_reorder_msg *msg;

	while (!tqp->rx_msg.active && !tqp->rx_write.active) {
		tbv_rx_prune_stale_reorder_locked(state, tqp, rx_path);
		msg = tbv_rx_reorder_find(tqp, tqp->rx_expected_psn);
		if (!msg || !msg->complete)
			return;
		if (msg->kind == TBV_RX_REORDER_WRITE) {
			if (!tbv_rx_deliver_reorder_write_locked(state, tqp,
								 rx_path, msg))
				return;
		} else if (msg->kind == TBV_RX_REORDER_READ_REQ) {
			if (!tbv_rx_deliver_reorder_read_req_locked(state, tqp,
								    rx_path,
								    msg))
				return;
		} else if (!tbv_rx_deliver_reorder_msg_locked(state, tqp,
							      rx_path, msg)) {
			return;
		}
	}
}

static bool tbv_rx_consume_ordered_psn_ack_locked(struct tbv_state *state,
						  struct tbv_qp *tqp,
						  struct tbv_path *rx_path,
						  u32 psn, int ack_status)
{
	if (psn != tqp->rx_expected_psn)
		return false;

	tqp->rx_expected_psn = tbv_psn_next(psn);
	tbv_qp_ack_history_store_locked(tqp, psn, ack_status);
	tbv_rx_drain_reorder_locked(state, tqp, rx_path);
	return true;
}

static void tbv_rx_drop_reorder_msg_locked(struct tbv_state *state,
					   struct tbv_qp *tqp,
					   struct tbv_rx_reorder_msg *msg)
{
	tbv_rx_reorder_unlink_msg_locked(tqp, msg);
	atomic64_inc(&state->data_rx_reorder_dropped);
	tbv_rx_reorder_free_msg(msg);
}

static bool tbv_rx_rnr_matches_locked(const struct tbv_qp *tqp,
				      u32 src_qp, u32 psn)
{
	return tqp->rx_rnr_active && tqp->rx_rnr_src_qp == src_qp &&
	       tqp->rx_rnr_psn == (psn & TBV_PSN_MASK);
}

static bool tbv_rx_rnr_is_first_retry_locked(const struct tbv_qp *tqp,
					     u32 src_qp, u32 psn,
					     u64 remote_addr,
					     u32 frag_offset)
{
	return tbv_rx_rnr_matches_locked(tqp, src_qp, psn) &&
	       tqp->rx_rnr_remote_addr == remote_addr &&
	       tqp->rx_rnr_frag_offset == frag_offset;
}

static void tbv_rx_mark_rnr_locked(struct tbv_qp *tqp, u32 src_qp, u32 psn,
				   u64 remote_addr, u32 frag_offset)
{
	tqp->rx_rnr_active = true;
	tqp->rx_rnr_src_qp = src_qp;
	tqp->rx_rnr_psn = psn & TBV_PSN_MASK;
	tqp->rx_rnr_remote_addr = remote_addr;
	tqp->rx_rnr_frag_offset = frag_offset;
}

static void tbv_rx_clear_rnr_locked(struct tbv_qp *tqp, u32 src_qp, u32 psn)
{
	if (tbv_rx_rnr_matches_locked(tqp, src_qp, psn))
		tqp->rx_rnr_active = false;
}

static void tbv_rx_buffer_read_req_locked(
	struct tbv_state *state, struct tbv_qp *tqp, struct tbv_path *rx_path,
	const struct tbv_native_data_header *hdr, u32 psn)
{
	struct tbv_rx_reorder_msg *msg;
	s32 delta = tbv_psn_delta(psn, tqp->rx_expected_psn);

	if (delta < 0) {
		tbv_qp_retry_read_resp(tqp, psn);
		return;
	}
	if (delta >= TBV_RX_REORDER_MAX_MESSAGES) {
		atomic64_inc(&state->data_rx_reorder_window);
		tbv_send_read_status_on_path(tqp, rx_path, hdr->src_qp,
					     hdr->dest_qp, psn,
					     hdr->imm_data, -EINVAL);
		return;
	}

	msg = tbv_rx_reorder_find(tqp, psn);
	if (!msg) {
		if (tqp->rx_reorder_count >= TBV_RX_REORDER_MAX_MESSAGES) {
			atomic64_inc(&state->data_rx_reorder_window);
			tbv_send_read_status_on_path(tqp, rx_path, hdr->src_qp,
						     hdr->dest_qp, psn,
						     hdr->imm_data, -EINVAL);
			return;
		}

		msg = kzalloc(sizeof(*msg), GFP_KERNEL);
		if (!msg) {
			atomic64_inc(&state->data_rx_read_req_alloc_error);
			tbv_send_read_status_on_path(tqp, rx_path, hdr->src_qp,
						     hdr->dest_qp, psn,
						     hdr->imm_data, -ENOMEM);
			return;
		}

		INIT_LIST_HEAD(&msg->frags);
		msg->first_jiffies = jiffies;
		msg->kind = TBV_RX_REORDER_READ_REQ;
		msg->remote_addr = hdr->remote_addr;
		msg->src_qp = hdr->src_qp;
		msg->psn = psn;
		msg->total_len = hdr->imm_data;
		msg->rkey = hdr->rkey;
		msg->complete = true;
		msg->frag_count = 1;
		msg->frags_received = 1;
		list_add_tail(&msg->node, &tqp->rx_reorder);
		tqp->rx_reorder_count++;
		atomic64_inc(&state->data_rx_reorder_buffered);
		tbv_qp_schedule_timeout(tqp);
		return;
	}

	if (msg->kind != TBV_RX_REORDER_READ_REQ ||
	    msg->src_qp != hdr->src_qp ||
	    msg->remote_addr != hdr->remote_addr ||
	    msg->total_len != hdr->imm_data ||
	    msg->rkey != hdr->rkey) {
		tbv_rx_drop_reorder_msg_locked(state, tqp, msg);
		tbv_send_read_status_on_path(tqp, rx_path, hdr->src_qp,
					     hdr->dest_qp, psn,
					     hdr->imm_data, -EINVAL);
		return;
	}

	if (msg->complete)
		tbv_rx_drain_reorder_locked(state, tqp, rx_path);
}

static void tbv_rx_buffer_fragment_locked(struct tbv_state *state,
					  struct tbv_qp *tqp,
					  struct tbv_path *rx_path,
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
	int ret;

	if (delta < 0) {
		tbv_rx_reack_duplicate_locked(state, tqp, rx_path,
					      hdr->src_qp, hdr->dest_qp,
					      psn);
		return;
	}
	if (delta >= TBV_RX_REORDER_MAX_MESSAGES) {
		atomic64_inc(&state->data_rx_reorder_window);
		tbv_rx_send_error_ack(state, tqp, rx_path, hdr, psn,
				      "reorder window", true);
		return;
	}
	if (!tbv_rx_fragment_shape(total_len, offset, hdr->length, last,
				   &frag_idx, &frag_count)) {
		tbv_rx_send_error_ack(state, tqp, rx_path, hdr, psn,
				      "bad fragment shape", false);
		return;
	}

	msg = tbv_rx_reorder_find(tqp, psn);
	if (!msg) {
		if (tqp->rx_reorder_count >= TBV_RX_REORDER_MAX_MESSAGES) {
			atomic64_inc(&state->data_rx_reorder_window);
			tbv_rx_send_error_ack(state, tqp, rx_path, hdr, psn,
					      "reorder full", true);
			return;
		}

		msg = kzalloc(sizeof(*msg), GFP_KERNEL);
		if (!msg) {
			tbv_rx_send_error_ack(state, tqp, rx_path, hdr, psn,
					      "reorder alloc failed", false);
			return;
		}

		INIT_LIST_HEAD(&msg->frags);
		msg->first_jiffies = jiffies;
		msg->kind = TBV_RX_REORDER_SEND;
		msg->src_qp = hdr->src_qp;
		msg->psn = psn;
		msg->total_len = total_len;
		msg->with_imm = with_imm;
		msg->imm_data = imm_data;
		msg->frag_count = frag_count;
		msg->solicited = hdr->flags & TBV_NATIVE_DATA_F_SOLICITED;
		list_add_tail(&msg->node, &tqp->rx_reorder);
		tqp->rx_reorder_count++;
		atomic64_inc(&state->data_rx_reorder_buffered);
		tbv_qp_schedule_timeout(tqp);
	} else if (msg->kind != TBV_RX_REORDER_SEND ||
		   msg->src_qp != hdr->src_qp ||
		   msg->total_len != total_len ||
		   msg->with_imm != with_imm ||
		   msg->imm_data != imm_data ||
		   msg->frag_count != frag_count) {
		tbv_rx_drop_reorder_msg_locked(state, tqp, msg);
		tbv_rx_send_error_ack(state, tqp, rx_path, hdr, psn,
				      "reorder collision", true);
		return;
	} else if (test_bit(frag_idx, msg->frag_seen)) {
		if (!tbv_rx_reorder_fragment_matches_locked(msg, offset,
							    payload,
							    hdr->length)) {
			tbv_rx_drop_reorder_msg_locked(state, tqp, msg);
			tbv_rx_send_error_ack(state, tqp, rx_path, hdr, psn,
					      "reorder collision", true);
			return;
		}
		if (msg->complete)
			tbv_rx_drain_reorder_locked(state, tqp, rx_path);
		return;
	}

	ret = tbv_rx_reorder_store_fragment_locked(tqp, msg, offset, payload,
						   hdr->length);
	if (ret) {
		if (ret == -ENOSPC)
			atomic64_inc(&state->data_rx_reorder_window);
		tbv_rx_drop_reorder_msg_locked(state, tqp, msg);
		tbv_rx_send_error_ack(state, tqp, rx_path, hdr, psn,
				      "reorder store failed", false);
		return;
	}
	set_bit(frag_idx, msg->frag_seen);
	msg->frags_received++;
	msg->received += hdr->length;
	if (msg->frags_received == msg->frag_count)
		msg->complete = true;
	if (msg->complete)
		tbv_rx_drain_reorder_locked(state, tqp, rx_path);
}

static void tbv_rx_buffer_write_fragment_locked(
	struct tbv_state *state, struct tbv_qp *tqp, struct tbv_path *rx_path,
	const struct tbv_native_data_header *hdr, u32 psn, u32 total_len,
	u32 offset, bool last, bool with_imm, const void *payload)
{
	struct tbv_rx_reorder_msg *msg;
	s32 delta = tbv_psn_delta(psn, tqp->rx_expected_psn);
	u32 imm_data = with_imm ? hdr->imm_data : 0;
	u32 frag_idx;
	u32 frag_count;
	int ret;

	if (delta < 0) {
		tbv_rx_reack_duplicate_locked(state, tqp, rx_path,
					      hdr->src_qp, hdr->dest_qp, psn);
		return;
	}
	if (delta >= TBV_RX_REORDER_MAX_MESSAGES) {
		atomic64_inc(&state->data_rx_reorder_window);
		tbv_send_ack_on_path(tqp, rx_path, hdr->src_qp, hdr->dest_qp,
				     psn, TBV_NATIVE_SEND_ACK_ERROR);
		return;
	}
	if (!tbv_rx_write_fragment_shape(total_len, offset, hdr->length, last,
					 &frag_idx, &frag_count)) {
		atomic64_inc(&state->data_rx_send_bad_fragment);
		tbv_send_ack_on_path(tqp, rx_path, hdr->src_qp, hdr->dest_qp,
				     psn, TBV_NATIVE_SEND_ACK_ERROR);
		return;
	}

	msg = tbv_rx_reorder_find(tqp, psn);
	if (!msg) {
		if (tqp->rx_reorder_count >= TBV_RX_REORDER_MAX_MESSAGES) {
			atomic64_inc(&state->data_rx_reorder_window);
			tbv_send_ack_on_path(tqp, rx_path, hdr->src_qp,
					     hdr->dest_qp, psn,
					     TBV_NATIVE_SEND_ACK_ERROR);
			return;
		}

		msg = kzalloc(sizeof(*msg), GFP_KERNEL);
		if (!msg) {
			tbv_send_ack_on_path(tqp, rx_path, hdr->src_qp,
					     hdr->dest_qp, psn,
					     TBV_NATIVE_SEND_ACK_ERROR);
			return;
		}

		INIT_LIST_HEAD(&msg->frags);
		msg->first_jiffies = jiffies;
		msg->kind = TBV_RX_REORDER_WRITE;
		msg->remote_addr = hdr->remote_addr;
		msg->src_qp = hdr->src_qp;
		msg->psn = psn;
		msg->total_len = total_len;
		msg->imm_data = imm_data;
		msg->rkey = hdr->rkey;
		msg->frag_count = frag_count;
		msg->with_imm = with_imm;
		msg->solicited = hdr->flags & TBV_NATIVE_DATA_F_SOLICITED;
		list_add_tail(&msg->node, &tqp->rx_reorder);
		tqp->rx_reorder_count++;
		atomic64_inc(&state->data_rx_reorder_buffered);
		tbv_qp_schedule_timeout(tqp);
	} else if (msg->kind != TBV_RX_REORDER_WRITE ||
		   msg->src_qp != hdr->src_qp ||
		   msg->remote_addr != hdr->remote_addr ||
		   msg->total_len != total_len ||
		   msg->with_imm != with_imm ||
		   msg->imm_data != imm_data ||
		   msg->rkey != hdr->rkey ||
		   msg->frag_count != frag_count) {
		tbv_rx_drop_reorder_msg_locked(state, tqp, msg);
		tbv_send_ack_on_path(tqp, rx_path, hdr->src_qp, hdr->dest_qp,
				     psn, TBV_NATIVE_SEND_ACK_ERROR);
		return;
	} else if (test_bit(frag_idx, msg->frag_seen)) {
		if (!tbv_rx_reorder_fragment_matches_locked(msg, offset,
							    payload,
							    hdr->length)) {
			tbv_rx_drop_reorder_msg_locked(state, tqp, msg);
			tbv_send_ack_on_path(tqp, rx_path, hdr->src_qp,
					     hdr->dest_qp, psn,
					     TBV_NATIVE_SEND_ACK_ERROR);
			return;
		}
		if (msg->complete)
			tbv_rx_drain_reorder_locked(state, tqp, rx_path);
		return;
	}

	ret = tbv_rx_reorder_store_fragment_locked(tqp, msg, offset, payload,
						   hdr->length);
	if (ret) {
		if (ret == -ENOSPC)
			atomic64_inc(&state->data_rx_reorder_window);
		tbv_rx_drop_reorder_msg_locked(state, tqp, msg);
		tbv_send_ack_on_path(tqp, rx_path, hdr->src_qp, hdr->dest_qp,
				     psn, TBV_NATIVE_SEND_ACK_ERROR);
		return;
	}
	set_bit(frag_idx, msg->frag_seen);
	msg->frags_received++;
	msg->received += hdr->length;
	if (msg->frags_received == msg->frag_count)
		msg->complete = true;
	if (msg->complete)
		tbv_rx_drain_reorder_locked(state, tqp, rx_path);
}

static void tbv_rx_handle_send_fragment(struct tbv_state *state,
					struct tbv_qp *tqp,
					const struct tbv_native_data_header *hdr,
					const void *payload,
					struct tbv_path *rx_path)
{
	struct tbv_rx_message *msg = &tqp->rx_msg;
	u32 total_len = hdr->imm_data;
	u32 psn = hdr->psn & TBV_PSN_MASK;
	u64 frag_end64;
	u32 offset;
	u32 copy_offset;
	u32 copy_len;
	bool last = hdr->flags & TBV_NATIVE_DATA_F_LAST;
	bool with_imm = hdr->opcode == TBV_NATIVE_DATA_OP_SEND_IMM;
	u32 imm_data = with_imm ? hdr->rkey : 0;
	const void *copy_payload = payload;

	if (hdr->remote_addr ||
	    check_add_overflow((u64)hdr->frag_offset, (u64)hdr->length,
			       &frag_end64) ||
	    frag_end64 > total_len ||
	    total_len > TBV_NATIVE_DATA_MAX_MSG_SIZE ||
	    (!last && !hdr->length) ||
	    (hdr->flags & ~(TBV_NATIVE_DATA_F_LAST |
			    TBV_NATIVE_DATA_F_SOLICITED)) ||
	    (!with_imm && hdr->rkey) ||
	    last != (frag_end64 == total_len)) {
		tbv_rx_send_error_ack(state, tqp, rx_path, hdr, psn,
				      "bad header", false);
		return;
	}
	offset = hdr->frag_offset;
	copy_offset = offset;
	copy_len = hdr->length;

	mutex_lock(&tqp->rx_lock);
	if (tbv_rx_rnr_matches_locked(tqp, hdr->src_qp, psn)) {
		if (!tbv_rx_rnr_is_first_retry_locked(tqp, hdr->src_qp, psn,
						      hdr->remote_addr,
						      hdr->frag_offset)) {
			atomic64_inc(&state->data_rx_rnr_suppressed);
			mutex_unlock(&tqp->rx_lock);
			return;
		}
		tbv_rx_clear_rnr_locked(tqp, hdr->src_qp, psn);
	}

	if (state->native_fragment_striping) {
		u32 frag_idx;
		u32 frag_count;
		s32 delta;

		if (!tbv_rx_fragment_shape(total_len, offset, hdr->length,
					   last, &frag_idx, &frag_count)) {
			mutex_unlock(&tqp->rx_lock);
			tbv_rx_send_error_ack(state, tqp, rx_path, hdr, psn,
					      "bad fragment shape", false);
			return;
		}

		delta = tbv_psn_delta(psn, tqp->rx_expected_psn);
		if (delta < 0) {
			tbv_rx_reack_duplicate_locked(state, tqp, rx_path,
						      hdr->src_qp,
						      hdr->dest_qp, psn);
			mutex_unlock(&tqp->rx_lock);
			return;
		}
		if (delta > 0 ||
		    (!msg->active && tbv_rx_reorder_find(tqp, psn))) {
			tbv_rx_buffer_fragment_locked(state, tqp, rx_path, hdr,
						      psn, total_len, offset,
						      last, payload);
			mutex_unlock(&tqp->rx_lock);
			return;
		}

		if (msg->active) {
			if (msg->src_qp != hdr->src_qp || msg->psn != psn ||
			    msg->total_len != total_len ||
			    msg->with_imm != with_imm ||
			    msg->imm_data != imm_data ||
			    msg->frag_count != frag_count) {
				tbv_rx_fail_active_send(state, tqp, rx_path,
							IB_WC_LOC_PROT_ERR);
				mutex_unlock(&tqp->rx_lock);
				tbv_rx_send_error_ack(state, tqp, rx_path, hdr,
						      psn, "active mismatch",
						      true);
				return;
			}
		} else {
			if (!tbv_qp_pop_recv(tqp, &msg->wqe)) {
				atomic64_inc(&state->data_rx_rnr);
				tbv_rx_mark_rnr_locked(tqp, hdr->src_qp, psn,
						       hdr->remote_addr,
						       hdr->frag_offset);
				tbv_send_ack_on_path(tqp, rx_path,
						     hdr->src_qp,
						     hdr->dest_qp, psn,
						     TBV_NATIVE_SEND_ACK_RNR);
				mutex_unlock(&tqp->rx_lock);
				return;
			}

			msg->active = true;
			msg->started_jiffies = jiffies;
			msg->src_qp = hdr->src_qp;
			msg->psn = psn;
			msg->total_len = total_len;
			msg->with_imm = with_imm;
			msg->imm_data = imm_data;
			msg->status = total_len > msg->wqe.length ?
					      IB_WC_LOC_LEN_ERR :
					      IB_WC_SUCCESS;
			msg->solicited = hdr->flags & TBV_NATIVE_DATA_F_SOLICITED;
			msg->frag_count = frag_count;
			bitmap_zero(msg->frag_seen, TBV_RX_REORDER_MAX_FRAGS);
			tbv_qp_schedule_timeout(tqp);
		}

		if (!test_bit(frag_idx, msg->frag_seen)) {
			tbv_rx_note_active_path(msg, rx_path, offset,
						hdr->length);
			if (tbv_rx_copy_to_wqe(state, &msg->wqe, offset,
					       payload, hdr->length,
					       &msg->delivered))
				msg->status = IB_WC_LOC_PROT_ERR;
			set_bit(frag_idx, msg->frag_seen);
			msg->frags_received++;
			msg->received += hdr->length;
		}

		if (msg->frags_received == msg->frag_count) {
			tbv_rx_finish_send(state, tqp, rx_path);
			tbv_rx_drain_reorder_locked(state, tqp, rx_path);
		}
		mutex_unlock(&tqp->rx_lock);
		return;
	}

	if (msg->active) {
		if (msg->src_qp != hdr->src_qp || msg->psn != psn ||
		    msg->total_len != total_len ||
		    msg->with_imm != with_imm ||
		    msg->imm_data != imm_data ||
		    msg->received != offset) {
			s32 delta = tbv_psn_delta(psn, tqp->rx_expected_psn);

			if (delta < 0) {
				tbv_rx_reack_duplicate_locked(state, tqp,
							      rx_path,
							      hdr->src_qp,
							      hdr->dest_qp,
							      psn);
				mutex_unlock(&tqp->rx_lock);
				return;
			}
			if (tbv_psn_delta(psn, tqp->rx_expected_psn) > 0) {
				tbv_rx_buffer_fragment_locked(state, tqp,
							      rx_path, hdr,
							      psn, total_len,
							      offset, last,
							      payload);
				mutex_unlock(&tqp->rx_lock);
				return;
			}
			if (msg->src_qp == hdr->src_qp && msg->psn == psn &&
			    msg->total_len == total_len &&
			    msg->with_imm == with_imm &&
			    msg->imm_data == imm_data &&
			    offset < msg->received) {
				u32 duplicate = msg->received - offset;

				if (duplicate >= hdr->length) {
					mutex_unlock(&tqp->rx_lock);
					return;
				}
				copy_payload = (const u8 *)payload + duplicate;
				copy_offset = msg->received;
				copy_len = hdr->length - duplicate;
			} else if (msg->src_qp == hdr->src_qp &&
				   msg->psn == psn &&
				   msg->total_len == total_len &&
				   msg->with_imm == with_imm &&
				   msg->imm_data == imm_data &&
				   offset > msg->received) {
				mutex_unlock(&tqp->rx_lock);
				return;
			} else {
				tbv_rx_fail_active_send(state, tqp, rx_path,
							IB_WC_LOC_PROT_ERR);
				mutex_unlock(&tqp->rx_lock);
				tbv_rx_send_error_ack(state, tqp, rx_path, hdr,
						      psn, "active mismatch",
						      true);
				return;
			}
		}
	} else if (tbv_rx_reorder_find(tqp, psn) ||
		   psn != tqp->rx_expected_psn) {
		tbv_rx_buffer_fragment_locked(state, tqp, rx_path, hdr, psn,
					      total_len, offset, last,
					      payload);
		mutex_unlock(&tqp->rx_lock);
		return;
	} else {
		if (offset) {
			mutex_unlock(&tqp->rx_lock);
			tbv_rx_send_error_ack(state, tqp, rx_path, hdr, psn,
					      "idle nonzero offset", true);
			return;
		}
		if (!tbv_qp_pop_recv(tqp, &msg->wqe)) {
			atomic64_inc(&state->data_rx_rnr);
			tbv_rx_mark_rnr_locked(tqp, hdr->src_qp, psn,
					       hdr->remote_addr,
					       hdr->frag_offset);
			tbv_send_ack_on_path(tqp, rx_path, hdr->src_qp,
					     hdr->dest_qp, psn,
					     TBV_NATIVE_SEND_ACK_RNR);
			mutex_unlock(&tqp->rx_lock);
			return;
		}

		msg->active = true;
		msg->started_jiffies = jiffies;
		msg->src_qp = hdr->src_qp;
		msg->psn = psn;
		msg->total_len = total_len;
		msg->with_imm = with_imm;
		msg->imm_data = imm_data;
		msg->status = total_len > msg->wqe.length ?
				      IB_WC_LOC_LEN_ERR :
				      IB_WC_SUCCESS;
		msg->solicited = hdr->flags & TBV_NATIVE_DATA_F_SOLICITED;
		tbv_qp_schedule_timeout(tqp);
	}

	tbv_rx_note_active_path(msg, rx_path, copy_offset, copy_len);
	if (tbv_rx_copy_to_wqe(state, &msg->wqe, copy_offset, copy_payload,
			       copy_len, &msg->delivered))
		msg->status = IB_WC_LOC_PROT_ERR;

	msg->received += copy_len;
	if (last) {
		tbv_rx_finish_send(state, tqp, rx_path);
		tbv_rx_drain_reorder_locked(state, tqp, rx_path);
	}
	mutex_unlock(&tqp->rx_lock);
}

static int tbv_rx_finish_write_locked(struct tbv_state *state,
				      struct tbv_qp *tqp,
				      struct tbv_path *rx_path,
				      enum ib_wc_status status)
{
	struct tbv_rx_write *wrx = &tqp->rx_write;
	struct ib_wc wc = {};
	u32 src_qp;
	u32 psn;
	int ack_status = status == IB_WC_SUCCESS ? TBV_NATIVE_SEND_ACK_OK :
						 TBV_NATIVE_SEND_ACK_ERROR;
	bool cq_error = false;

	if (!wrx->active)
		return 0;

	src_qp = wrx->src_qp;
	psn = wrx->psn;
	if (wrx->with_imm) {
		struct tbv_cq *recv_cq = container_of(tqp->base.recv_cq,
						      struct tbv_cq, base);

		tbv_wc_set_recv_wr(&wc, &wrx->imm_wqe);
		wc.status = status;
		wc.opcode = status == IB_WC_SUCCESS ?
				    IB_WC_RECV_RDMA_WITH_IMM :
				    IB_WC_RECV;
		wc.qp = &tqp->base;
		wc.src_qp = src_qp;
		wc.pkey_index = 0;
		wc.port_num = 1;
		if (status == IB_WC_SUCCESS) {
			wc.wc_flags = IB_WC_WITH_IMM;
			wc.ex.imm_data = cpu_to_be32(wrx->imm_data);
		}
		if (tbv_cq_push(recv_cq, &wc)) {
			ack_status = TBV_NATIVE_SEND_ACK_ERROR;
			cq_error = true;
		}
	}

	memset(wrx, 0, sizeof(*wrx));
	if (!tbv_rx_consume_ordered_psn_ack_locked(state, tqp, rx_path, psn,
						   ack_status)) {
		tbv_send_ack_on_path(tqp, rx_path, src_qp, tqp->base.qp_num, psn,
				     TBV_NATIVE_SEND_ACK_ERROR);
		return -EINVAL;
	}
	tbv_send_ack_on_path(tqp, rx_path, src_qp, tqp->base.qp_num, psn,
			     ack_status);
	return cq_error ? -ENOSPC : 0;
}

static void tbv_rx_fail_active_write_locked(struct tbv_state *state,
						struct tbv_qp *tqp,
						struct tbv_path *rx_path,
						enum ib_wc_status status)
{
	if (!tqp->rx_write.active)
		return;
	tbv_rx_finish_write_locked(state, tqp, rx_path, status);
}

static void tbv_rx_handle_rdma_write_fragment(struct tbv_state *state,
					      struct tbv_qp *tqp,
					      const struct tbv_native_data_header *hdr,
					      const void *payload,
					      struct tbv_path *rx_path)
{
	struct tbv_rx_write *wrx = &tqp->rx_write;
	const void *copy_payload = payload;
	struct tbv_mr *mr;
	bool last = hdr->flags & TBV_NATIVE_DATA_F_LAST;
	bool with_imm = hdr->opcode == TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM;
	u32 offset = hdr->frag_offset;
	u32 copy_offset = offset;
	u32 total_len = with_imm ? 0 : hdr->imm_data;
	u64 frag_end;
	u64 copy_addr;
	u32 psn = hdr->psn & TBV_PSN_MASK;
	u32 copy_len = hdr->length;
	int ret;

	if ((hdr->flags & ~(TBV_NATIVE_DATA_F_LAST |
			    TBV_NATIVE_DATA_F_SOLICITED)) ||
	    check_add_overflow((u64)hdr->frag_offset, (u64)hdr->length,
			       &frag_end) ||
	    frag_end > TBV_NATIVE_DATA_MAX_MSG_SIZE ||
	    (!with_imm && (!total_len ||
			   total_len > TBV_NATIVE_DATA_MAX_MSG_SIZE ||
			   frag_end > total_len ||
			   last != (frag_end == total_len))) ||
	    (!last && !hdr->length) ||
	    !(tqp->attr.qp_access_flags & IB_ACCESS_REMOTE_WRITE)) {
		tbv_rx_bad_header_note(state, rx_path,
				       &state->data_rx_bad_header_write,
				       "write", hdr,
				       TBV_NATIVE_DATA_HDR_SIZE + hdr->length, 0);
		tbv_send_ack_on_path(tqp, rx_path, hdr->src_qp,
				     hdr->dest_qp, hdr->psn,
				     TBV_NATIVE_SEND_ACK_ERROR);
		return;
	}

	mutex_lock(&tqp->rx_lock);
	if (tbv_rx_rnr_matches_locked(tqp, hdr->src_qp, psn)) {
		if (!tbv_rx_rnr_is_first_retry_locked(tqp, hdr->src_qp, psn,
						      hdr->remote_addr,
						      hdr->frag_offset)) {
			atomic64_inc(&state->data_rx_rnr_suppressed);
			mutex_unlock(&tqp->rx_lock);
			return;
		}
		tbv_rx_clear_rnr_locked(tqp, hdr->src_qp, psn);
	}

	if (tbv_rx_reack_duplicate_locked(state, tqp, rx_path, hdr->src_qp,
					  hdr->dest_qp, psn)) {
		mutex_unlock(&tqp->rx_lock);
		return;
	}

	if (wrx->active) {
		bool same_write = wrx->src_qp == hdr->src_qp &&
				  wrx->psn == psn &&
				  wrx->with_imm == with_imm &&
				  wrx->rkey == hdr->rkey &&
				  wrx->imm_data == hdr->imm_data &&
				  wrx->remote_addr == hdr->remote_addr;

		if (!same_write) {
			s32 delta = tbv_psn_delta(psn, tqp->rx_expected_psn);

			if (delta < 0) {
				tbv_rx_reack_duplicate_locked(state, tqp, rx_path,
							      hdr->src_qp,
							      hdr->dest_qp,
							      psn);
				mutex_unlock(&tqp->rx_lock);
				return;
			}
			if (delta > 0) {
				if (!with_imm) {
					tbv_rx_buffer_write_fragment_locked(
						state, tqp, rx_path, hdr, psn,
						total_len, offset, last, with_imm,
						payload);
					mutex_unlock(&tqp->rx_lock);
					return;
				}
				mutex_unlock(&tqp->rx_lock);
				return;
			}
			atomic64_inc(&state->data_rx_send_sequence_error);
			tbv_rx_fail_active_write_locked(state, tqp, rx_path,
							IB_WC_LOC_PROT_ERR);
			mutex_unlock(&tqp->rx_lock);
			return;
		}

		if (offset < wrx->received) {
			u32 duplicate = wrx->received - offset;

			if (duplicate >= hdr->length) {
				mutex_unlock(&tqp->rx_lock);
				return;
			}
			copy_payload = (const u8 *)payload + duplicate;
			copy_offset = wrx->received;
			copy_len = hdr->length - duplicate;
		} else if (offset > wrx->received) {
			if (!with_imm)
				tbv_rx_buffer_write_fragment_locked(
					state, tqp, rx_path, hdr, psn,
					total_len, offset, last, with_imm,
					payload);
			mutex_unlock(&tqp->rx_lock);
			return;
		}
	} else {
		if (psn != tqp->rx_expected_psn) {
			if (!with_imm) {
				tbv_rx_buffer_write_fragment_locked(
					state, tqp, rx_path, hdr, psn, total_len,
					offset, last, with_imm, payload);
				mutex_unlock(&tqp->rx_lock);
				return;
			}
			mutex_unlock(&tqp->rx_lock);
			return;
		}
		if (!with_imm && tbv_rx_reorder_find(tqp, psn)) {
			tbv_rx_buffer_write_fragment_locked(
				state, tqp, rx_path, hdr, psn, total_len, offset,
				last, with_imm, payload);
			mutex_unlock(&tqp->rx_lock);
			return;
		}
		if (offset) {
			if (!with_imm) {
				tbv_rx_buffer_write_fragment_locked(
					state, tqp, rx_path, hdr, psn,
					total_len, offset, last, with_imm, payload);
				mutex_unlock(&tqp->rx_lock);
				return;
			}
			mutex_unlock(&tqp->rx_lock);
			tbv_send_ack_on_path(tqp, rx_path, hdr->src_qp,
					     hdr->dest_qp, hdr->psn,
					     TBV_NATIVE_SEND_ACK_ERROR);
			return;
		}
		if (with_imm && !tbv_qp_pop_recv(tqp, &wrx->imm_wqe)) {
			atomic64_inc(&state->data_rx_rnr);
			tbv_rx_mark_rnr_locked(tqp, hdr->src_qp, psn,
					       hdr->remote_addr,
					       hdr->frag_offset);
			tbv_send_ack_on_path(tqp, rx_path, hdr->src_qp,
					     hdr->dest_qp, psn,
					     TBV_NATIVE_SEND_ACK_RNR);
			mutex_unlock(&tqp->rx_lock);
			return;
		}

		wrx->active = true;
		wrx->started_jiffies = jiffies;
		wrx->src_qp = hdr->src_qp;
		wrx->psn = psn;
		wrx->remote_addr = hdr->remote_addr;
		wrx->rkey = hdr->rkey;
		wrx->imm_data = hdr->imm_data;
		wrx->with_imm = with_imm;
		wrx->solicited = hdr->flags & TBV_NATIVE_DATA_F_SOLICITED;
		tbv_qp_schedule_timeout(tqp);
	}

	if (copy_len) {
		if (check_add_overflow(wrx->remote_addr, (u64)copy_offset,
				       &copy_addr)) {
			atomic64_inc(&state->data_rx_copy_error);
			tbv_rx_fail_active_write_locked(
				state, tqp, rx_path, IB_WC_LOC_PROT_ERR);
			mutex_unlock(&tqp->rx_lock);
			return;
		}

		mr = tbv_mr_get(state, wrx->rkey);
		if (!mr) {
			atomic64_inc(&state->data_rx_copy_error);
			tbv_rx_fail_active_write_locked(
				state, tqp, rx_path, IB_WC_LOC_PROT_ERR);
			mutex_unlock(&tqp->rx_lock);
			return;
		}

		if (!(mr->access & IB_ACCESS_REMOTE_WRITE)) {
			atomic64_inc(&state->data_rx_copy_error);
			tbv_mr_put(mr);
			tbv_rx_fail_active_write_locked(
				state, tqp, rx_path, IB_WC_LOC_PROT_ERR);
			mutex_unlock(&tqp->rx_lock);
			return;
		}

		ret = tbv_umem_copy_to_iova(mr, copy_addr, copy_payload, copy_len);
		tbv_mr_put(mr);
		if (ret) {
			atomic64_inc(&state->data_rx_copy_error);
			tbv_rx_fail_active_write_locked(
				state, tqp, rx_path, IB_WC_LOC_PROT_ERR);
			mutex_unlock(&tqp->rx_lock);
			return;
		}

		wrx->received = copy_offset + copy_len;
	}
	if (last) {
		tbv_rx_finish_write_locked(state, tqp, rx_path,
					       IB_WC_SUCCESS);
	}
	mutex_unlock(&tqp->rx_lock);
}

static struct tbv_read_resp_ctx *
tbv_read_resp_ctx_alloc(struct tbv_qp *tqp, struct tbv_mr *mr,
			struct tbv_path *rx_path,
			const struct tbv_native_data_header *req, int *status)
{
	struct tbv_read_resp_ctx *ctx;
	int ret = 0;

	if (status)
		*status = 0;
	if (!tbv_qp_get_live(tqp)) {
		if (status)
			*status = -EINVAL;
		return NULL;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		if (status)
			*status = -ENOMEM;
		tbv_qp_put(tqp);
		return NULL;
	}

	INIT_LIST_HEAD(&ctx->node);
	INIT_LIST_HEAD(&ctx->retry_node);
	ctx->tqp = tqp;
	refcount_inc(&mr->refs);
	ctx->mr = mr;
	ctx->rx_path = tbv_path_get_for_response(rx_path);
	refcount_set(&ctx->refs, 1);
	ctx->req = *req;
	ctx->data_len = req->imm_data;
	if (ctx->data_len) {
		ctx->data = vmalloc(ctx->data_len);
		if (!ctx->data) {
			if (status)
				*status = -ENOMEM;
			tbv_read_resp_ctx_put(ctx);
			return NULL;
		}
		ret = tbv_umem_copy_from_iova(mr, req->remote_addr, ctx->data,
					      ctx->data_len);
		if (ret) {
			if (status)
				*status = ret;
			tbv_read_resp_ctx_put(ctx);
			return NULL;
		}
	}
	return ctx;
}

static int tbv_send_read_response_ctx(struct tbv_read_resp_ctx *ctx)
{
	struct tbv_path *path;
		struct tbv_qp *tqp = ctx->tqp;
		struct tbv_state *state = tqp->owner;
		const struct tbv_native_data_header *req = &ctx->req;
		u32 total_len = req->imm_data;
	u32 nfrags = total_len ? DIV_ROUND_UP(total_len,
					      TBV_NATIVE_DATA_MAX_PAYLOAD) : 1;
	u32 offset = 0;
	u32 remaining = nfrags;
	bool sent_any = false;
	bool selected_ref = false;
	int ret = 0;

	path = tbv_select_read_response_path(state, tqp, ctx->rx_path,
					     &selected_ref);
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

			if (payload_len)
				memcpy(frame + TBV_NATIVE_DATA_HDR_SIZE,
				       (u8 *)ctx->data + offset, payload_len);

		resp.opcode = TBV_NATIVE_DATA_OP_RDMA_READ_RESP;
		resp.flags = last ? TBV_NATIVE_DATA_F_LAST : 0;
		resp.dest_qp = req->src_qp;
		resp.src_qp = req->dest_qp;
			resp.psn = req->psn;
			resp.length = payload_len;
			resp.imm_data = total_len;
			resp.remote_addr = 0;
			resp.frag_offset = offset;

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
	if (selected_ref)
		tbv_release_path_refs(&path, 1);
	return 0;

out_release_reservation:
	tbv_path_release_data_reservation(path, remaining);
	if (sent_any)
		tbv_path_kick_tx(path);
out_put_path:
	if (selected_ref)
		tbv_release_path_refs(&path, 1);
	return ret;
}

static void tbv_read_req_workfn(struct work_struct *work)
{
	struct tbv_read_req_work *req_work =
		container_of(work, struct tbv_read_req_work, work);
	struct tbv_native_data_header *req = &req_work->hdr;
	struct tbv_state *state = req_work->state;
	struct tbv_qp *tqp = req_work->tqp;
	struct tbv_path *rx_path = req_work->rx_path;
	struct tbv_mr *mr;
	u64 addr;
	int ret = 0;

	if (!READ_ONCE(state->verbs_registered))
		goto out_free;

	if (!(tqp->attr.qp_access_flags & IB_ACCESS_REMOTE_READ)) {
		atomic64_inc(&state->data_rx_read_req_no_access);
		ret = -EACCES;
		goto out_put_qp_status;
	}

	mr = tbv_mr_get(state, req->rkey);
	if (!mr) {
		atomic64_inc(&state->data_rx_read_req_no_mr);
		ret = -EACCES;
		goto out_put_qp_status;
	}
	if (!(mr->access & IB_ACCESS_REMOTE_READ)) {
		atomic64_inc(&state->data_rx_read_req_mr_access);
		ret = -EACCES;
		goto out_put_mr_status;
	}
	if (req->imm_data > TBV_NATIVE_DATA_MAX_MSG_SIZE) {
		atomic64_inc(&state->data_rx_read_req_too_large);
		ret = -EMSGSIZE;
		goto out_put_mr_status;
	}
	ret = tbv_umem_iova_to_addr(mr, req->remote_addr, req->imm_data,
				    &addr);
	if (ret) {
		atomic64_inc(&state->data_rx_read_req_bad_iova);
		goto out_put_mr_status;
	}

	if (!req->imm_data) {
		tbv_send_read_status_on_path(tqp, rx_path, req->src_qp,
					     req->dest_qp, req->psn, 0, 0);
		goto out_put_mr;
	}

		{
			struct tbv_read_resp_ctx *resp;
			struct tbv_read_resp_ctx *taken;
			int snapshot_status = 0;

			resp = tbv_read_resp_ctx_alloc(tqp, mr, rx_path, req,
						       &snapshot_status);
			if (!resp) {
				if (snapshot_status == -ENOMEM)
					atomic64_inc(&state->data_rx_read_req_alloc_error);
				else
					atomic64_inc(&state->data_rx_read_req_bad_iova);
				ret = snapshot_status ?: -ENOMEM;
				goto out_send_status;
			}

		tbv_qp_queue_read_resp(tqp, resp);
		ret = tbv_send_read_response_ctx(resp);
		if (!ret)
			tbv_qp_note_read_resp_sent(tqp, resp);
		if (ret) {
			if (ret == -ENOMEM) {
				/*
				 * Transient responder TX pressure: keep the
				 * response queued and let the retry timer send it.
				 */
				atomic64_inc(&state->data_rx_read_req_resp_busy);
				tbv_qp_arm_read_resp_timeout(tqp, resp);
				ret = 0;
			} else {
				taken = tbv_qp_take_read_resp(tqp, req->psn);
				if (taken)
					tbv_read_resp_ctx_put(taken);
			}
		}
		tbv_read_resp_ctx_put(resp);
	}
	if (ret) {
		if (ret == -ENOMEM)
			atomic64_inc(&state->data_rx_read_req_resp_busy);
		else
			atomic64_inc(&state->data_rx_read_req_resp_error);
out_send_status:
		tbv_send_read_status_on_path(tqp, rx_path, req->src_qp,
					     req->dest_qp, req->psn,
					     req->imm_data, ret);
	}

out_put_mr:
	tbv_mr_put(mr);
	goto out_free;

out_put_mr_status:
	tbv_mr_put(mr);
out_put_qp_status:
	tbv_send_read_status_on_path(tqp, rx_path, req->src_qp,
				     req->dest_qp, req->psn, req->imm_data,
				     ret);
out_free:
	tbv_qp_put(tqp);
	tbv_path_put_response(rx_path);
	kfree(req_work);
}

static void tbv_rx_queue_rdma_read_req_work(
	struct tbv_state *state, struct tbv_qp *tqp,
	const struct tbv_native_data_header *hdr, struct tbv_path *rx_path)
{
	struct tbv_read_req_work *work;

	work = kzalloc(sizeof(*work), GFP_ATOMIC);
	if (!work) {
		tbv_send_read_status_on_path(tqp, rx_path, hdr->src_qp,
					     hdr->dest_qp, hdr->psn,
					     hdr->imm_data, -ENOMEM);
		return;
	}
	if (!tbv_qp_get_live(tqp)) {
		kfree(work);
		return;
	}
	INIT_WORK(&work->work, tbv_read_req_workfn);
	work->state = state;
	work->tqp = tqp;
	work->rx_path = tbv_path_get_for_response(rx_path);
	work->hdr = *hdr;
	queue_work(state->workqueue ? state->workqueue : system_unbound_wq,
		   &work->work);
}

static void tbv_rx_handle_rdma_read_req(struct tbv_state *state,
					struct tbv_qp *tqp,
					const struct tbv_native_data_header *hdr,
					struct tbv_path *rx_path)
{
	u32 psn = hdr->psn & TBV_PSN_MASK;
	s32 delta;
	bool queue = false;

	if (hdr->length || !(hdr->flags & TBV_NATIVE_DATA_F_LAST) ||
	    (hdr->flags & ~TBV_NATIVE_DATA_F_LAST)) {
		tbv_rx_bad_header_note(state, rx_path,
				       &state->data_rx_bad_header_read_req,
				       "read_req", hdr,
				       TBV_NATIVE_DATA_HDR_SIZE + hdr->length, 0);
		tbv_send_read_status_on_path(tqp, rx_path, hdr->src_qp,
					     hdr->dest_qp, hdr->psn,
					     hdr->imm_data, -EINVAL);
		return;
	}

	mutex_lock(&tqp->rx_lock);
	delta = tbv_psn_delta(psn, tqp->rx_expected_psn);
	if (delta < 0) {
		tbv_rx_buffer_read_req_locked(state, tqp, rx_path, hdr, psn);
	} else if (delta > 0 || tqp->rx_msg.active || tqp->rx_write.active ||
		   tbv_rx_reorder_find(tqp, psn)) {
		tbv_rx_buffer_read_req_locked(state, tqp, rx_path, hdr, psn);
	} else {
		tqp->rx_expected_psn = tbv_psn_next(psn);
		tbv_rx_drain_reorder_locked(state, tqp, rx_path);
		queue = true;
	}
	mutex_unlock(&tqp->rx_lock);

	if (queue)
		tbv_rx_queue_rdma_read_req_work(state, tqp, hdr, rx_path);
}

static void tbv_rx_handle_rdma_read_resp(struct tbv_state *state,
					 struct tbv_qp *tqp,
					 const struct tbv_native_data_header *hdr,
					 const void *payload,
					 struct tbv_path *rx_path)
{
	struct tbv_read_ctx *read;
	u32 next_received;
	u32 frag_idx;
	u32 frag_count;
	bool last = hdr->flags & TBV_NATIVE_DATA_F_LAST;
	int ret = 0;

	read = tbv_qp_find_read_get(tqp, hdr->psn);
	if (!read) {
		atomic64_inc(&state->data_rx_read_resp_duplicate);
		tbv_send_read_ack_on_path(tqp, rx_path, hdr->src_qp,
					  hdr->dest_qp, hdr->psn,
					  TBV_NATIVE_READ_ACK_OK);
		return;
	}

	mutex_lock(&read->data_lock);
	if (hdr->rkey) {
		atomic64_inc(&state->data_rx_read_resp_remote_error);
		ret = -EIO;
		goto complete_ack;
	}
	if (hdr->remote_addr ||
	    hdr->imm_data != read->total_len ||
	    check_add_overflow(hdr->frag_offset, hdr->length,
			       &next_received) ||
	    next_received > read->total_len ||
	    (hdr->flags & ~(TBV_NATIVE_DATA_F_LAST | TBV_NATIVE_DATA_F_SOLICITED)) ||
	    !tbv_rx_fragment_shape(read->total_len, hdr->frag_offset,
				   hdr->length, last, &frag_idx, &frag_count)) {
		atomic64_inc(&state->data_rx_read_resp_bad_header);
		ret = -EINVAL;
		goto complete_ack;
	}
	if (hdr->frag_offset < read->received) {
		atomic64_inc(&state->data_rx_read_resp_duplicate);
		mutex_unlock(&read->data_lock);
		tbv_read_ctx_put(read);
		return;
	}
	if (hdr->frag_offset != read->received) {
		atomic64_inc(&state->data_rx_read_resp_gap);
		ret = tbv_read_resp_store_frag_locked(read, hdr->frag_offset,
						      payload, hdr->length);
		if (ret == -EIO) {
			atomic64_inc(&state->data_rx_read_resp_bad_header);
			goto complete_ack;
		}
		tbv_send_read_ack_on_path(tqp, rx_path, hdr->src_qp,
					  hdr->dest_qp, hdr->psn,
					  TBV_NATIVE_READ_ACK_RETRY);
		mutex_unlock(&read->data_lock);
		tbv_read_ctx_put(read);
		return;
	}

	ret = tbv_copy_to_read_segments(read, hdr->frag_offset, payload,
					hdr->length);
	if (ret) {
		atomic64_inc(&state->data_rx_read_resp_copy_error);
		goto complete_ack;
	}

	read->received = next_received;
	ret = tbv_read_resp_drain_frags_locked(read);
	if (ret) {
		atomic64_inc(&state->data_rx_read_resp_copy_error);
		goto complete_ack;
	}
	if (read->received != read->total_len) {
		mutex_unlock(&read->data_lock);
		tbv_read_ctx_put(read);
		return;
	}

complete_ack:
	tbv_send_read_ack_on_path(tqp, rx_path, hdr->src_qp, hdr->dest_qp,
				  hdr->psn, ret ? TBV_NATIVE_READ_ACK_ERROR :
						 TBV_NATIVE_READ_ACK_OK);
	tbv_qp_complete_read_ordered(tqp, read, ret);
	mutex_unlock(&read->data_lock);
	tbv_read_ctx_put(read);
}

static int tbv_poll_cq(struct ib_cq *cq, int num_entries, struct ib_wc *wc)
{
	struct tbv_cq *tcq = container_of(cq, struct tbv_cq, base);
	unsigned long flags;
	int polled = 0;
	bool overflowed;

	if (num_entries <= 0 || !wc)
		return 0;

	spin_lock_irqsave(&tcq->lock, flags);
	while (polled < num_entries && tcq->count) {
		wc[polled++] = tcq->entries[tcq->head];
		tcq->head = (tcq->head + 1) % tcq->cqe;
		tcq->count--;
	}
	overflowed = tcq->overflowed;
	spin_unlock_irqrestore(&tcq->lock, flags);

	if (!polled && overflowed)
		return -EIO;
	return polled;
}

static int tbv_req_notify_cq(struct ib_cq *cq, enum ib_cq_notify_flags flags)
{
	struct tbv_cq *tcq = container_of(cq, struct tbv_cq, base);
	unsigned long irq_flags;
	int ret;

	spin_lock_irqsave(&tcq->lock, irq_flags);
	if (tcq->overflowed) {
		ret = -EIO;
	} else {
		tcq->notify_armed = true;
		ret = tcq->count ? 1 : 0;
	}
	spin_unlock_irqrestore(&tcq->lock, irq_flags);
	return ret;
}

static void tbv_rx_handle_mad(struct tbv_state *state, struct tbv_path *rx_path,
			      const struct tbv_native_data_header *hdr,
			      const void *payload)
{
	struct tbv_qp *tqp;
	struct tbv_cq *recv_cq;
	struct tbv_recv_wqe wqe;
	struct ib_grh grh = {};
	struct ib_wc wc = {};
	union ib_gid sgid;
	union ib_gid dgid;
	const void *mad;
	void *dst = NULL;
	u16 pkey_index;
	u32 mad_len;
	enum ib_wc_status status = IB_WC_SUCCESS;
	int ret;

	if (!rx_path || hdr->flags != TBV_NATIVE_DATA_F_LAST ||
	    hdr->dest_qp != TBV_GSI_QPN || hdr->src_qp != TBV_GSI_QPN ||
	    hdr->imm_data != sizeof(struct ib_mad)) {
		tbv_rx_bad_header_note(state, rx_path,
				       &state->data_rx_bad_header_mad,
				       "mad_header", hdr,
				       TBV_NATIVE_DATA_HDR_SIZE + hdr->length, 0);
		return;
	}

	ret = tbv_gsi_meta_parse(payload, hdr->length, &sgid, &dgid,
				 &pkey_index, &mad, &mad_len);
	if (ret || mad_len != sizeof(struct ib_mad)) {
		tbv_rx_bad_header_note(state, rx_path,
				       &state->data_rx_bad_header_mad,
				       "mad_meta", hdr,
				       TBV_NATIVE_DATA_HDR_SIZE + hdr->length, ret);
		return;
	}

	tqp = tbv_path_get_gsi_qp(rx_path);
	if (!tqp) {
		atomic64_inc(&state->data_rx_no_qp);
		return;
	}

	if (!tbv_qp_pop_recv(tqp, &wqe)) {
		atomic64_inc(&state->data_rx_no_recv);
		tbv_qp_put(tqp);
		return;
	}

	if (wqe.length < sizeof(grh) + mad_len) {
		status = IB_WC_LOC_LEN_ERR;
	} else if (!tbv_qp_accepts_kernel_dma_lkey(tqp, wqe.lkey)) {
		status = IB_WC_LOC_PROT_ERR;
	} else {
		dst = ib_virt_dma_to_ptr(wqe.addr);
		if (!dst) {
			status = IB_WC_LOC_PROT_ERR;
		} else {
			grh.version_tclass_flow = cpu_to_be32(6 << 28);
			grh.paylen = cpu_to_be16(mad_len);
			grh.next_hdr = IPPROTO_UDP;
			grh.hop_limit = 0xff;
			grh.sgid = sgid;
			grh.dgid = dgid;
			memcpy(dst, &grh, sizeof(grh));
			memcpy((u8 *)dst + sizeof(grh), mad, mad_len);
		}
	}

	recv_cq = container_of(tqp->base.recv_cq, struct tbv_cq, base);
	tbv_wc_set_recv_wr(&wc, &wqe);
	wc.status = status;
	wc.opcode = IB_WC_RECV;
	wc.qp = &tqp->base;
	wc.byte_len = status == IB_WC_SUCCESS ? sizeof(grh) + mad_len : 0;
	wc.src_qp = hdr->src_qp;
	wc.wc_flags = IB_WC_GRH | IB_WC_WITH_NETWORK_HDR_TYPE;
	wc.pkey_index = pkey_index;
	wc.port_num = 1;
	wc.network_hdr_type = RDMA_NETWORK_IPV6;
	tbv_cq_push(recv_cq, &wc);
	tbv_qp_put(tqp);
}

void tbv_ibdev_rx_native_frame(struct tbv_state *state,
			       struct tbv_path *rx_path,
			       const struct tbv_native_data_header *hdr,
			       const void *payload)
{
	struct tbv_qp *tqp;
	enum tbv_rx_endpoint_status endpoint_status;

	if (!state || !state->verbs_registered)
		return;

	switch (hdr->opcode) {
	case TBV_NATIVE_DATA_OP_SEND_ACK:
	case TBV_NATIVE_DATA_OP_RDMA_READ_ACK:
	case TBV_NATIVE_DATA_OP_RECV_CREDIT:
	case TBV_NATIVE_DATA_OP_RDMA_READ_REQ:
	case TBV_NATIVE_DATA_OP_RDMA_READ_RESP:
	case TBV_NATIVE_DATA_OP_SEND:
	case TBV_NATIVE_DATA_OP_SEND_IMM:
	case TBV_NATIVE_DATA_OP_RDMA_WRITE:
	case TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM:
	case TBV_NATIVE_DATA_OP_MAD:
		break;
	default:
		tbv_rx_bad_header_note(state, rx_path,
				       &state->data_rx_bad_header_opcode,
				       "opcode", hdr,
				       TBV_NATIVE_DATA_HDR_SIZE + hdr->length, 0);
		return;
	}

	if (hdr->opcode == TBV_NATIVE_DATA_OP_MAD) {
		tbv_rx_handle_mad(state, rx_path, hdr, payload);
		return;
	}

	tqp = tbv_qp_get_by_num(state, hdr->dest_qp);
	if (!tqp) {
		atomic64_inc(&state->data_rx_no_qp);
		return;
	}

	if (hdr->opcode == TBV_NATIVE_DATA_OP_RECV_CREDIT) {
		atomic64_inc(&state->data_rx_ack);
		if (!hdr->imm_data) {
			tbv_rx_bad_header_note(state, rx_path,
					       &state->data_rx_bad_header_recv_credit,
					       "recv_credit", hdr,
					       TBV_NATIVE_DATA_HDR_SIZE +
					       hdr->length, 0);
			tbv_qp_put(tqp);
			return;
		}

		endpoint_status = tbv_qp_accept_recv_credit(tqp, hdr);
		if (endpoint_status != TBV_RX_ENDPOINT_OK) {
			if (endpoint_status == TBV_RX_ENDPOINT_QP_ERROR)
				atomic64_inc(&state->data_rx_qp_error);
			else
				atomic64_inc(&state->data_rx_bad_peer);
			tbv_qp_put(tqp);
			return;
		}

		wake_up_all(&tqp->credit_wait);
		tbv_qp_schedule_timeout_now(tqp);
		tbv_qp_put(tqp);
		return;
	}

	endpoint_status = tbv_qp_validate_native_endpoint(tqp, hdr);
	if (endpoint_status != TBV_RX_ENDPOINT_OK) {
		if (endpoint_status == TBV_RX_ENDPOINT_UNCONNECTED)
			atomic64_inc(&state->data_rx_unconnected_qp);
		else if (endpoint_status == TBV_RX_ENDPOINT_QP_ERROR)
			atomic64_inc(&state->data_rx_qp_error);
		else
			atomic64_inc(&state->data_rx_bad_peer);
		tbv_qp_put(tqp);
		return;
	}

	if (hdr->opcode == TBV_NATIVE_DATA_OP_SEND_ACK) {
		LIST_HEAD(acked);
		struct tbv_send_ctx *matched_send = NULL;
		int status;
		bool saw_ack = false;
		bool completed_ack = false;
		bool completed_error = false;

		atomic64_inc(&state->data_rx_ack);
		switch (hdr->imm_data) {
		case TBV_NATIVE_SEND_ACK_OK:
			status = 0;
			saw_ack = tbv_qp_complete_send_ordered(
				tqp, hdr->psn, status, &acked, &matched_send);
			break;
		case TBV_NATIVE_SEND_ACK_RNR:
			status = -EAGAIN;
			atomic64_inc(&state->data_rx_ack_rnr);
			saw_ack = tbv_qp_note_rnr_ack(tqp, hdr->psn,
						      &acked, &matched_send);
			break;
		case TBV_NATIVE_SEND_ACK_ERROR:
			status = -EIO;
			saw_ack = tbv_qp_complete_send_ordered(
				tqp, hdr->psn, status, &acked, &matched_send);
			break;
		default:
			tbv_rx_bad_header_note(state, rx_path,
					       &state->data_rx_bad_header_ack,
					       "send_ack", hdr,
					       TBV_NATIVE_DATA_HDR_SIZE +
					       hdr->length, 0);
			tbv_qp_put(tqp);
			return;
		}
		if (matched_send) {
			tbv_note_matched_send_ack(state, hdr, matched_send);
			tbv_send_ctx_put(matched_send);
		}
		if (!saw_ack) {
			if (tbv_qp_ack_is_late_duplicate(tqp, hdr->psn))
				atomic64_inc(&state->data_rx_late_ack);
			else
				atomic64_inc(&state->data_rx_ack_miss);
		}
		while (!list_empty(&acked)) {
			struct tbv_send_ctx *send =
				list_first_entry(&acked, struct tbv_send_ctx,
						 node);

			list_del_init(&send->node);
			completed_ack = true;
			if (send->completion_status)
				completed_error = true;
			if (send->completion_status == -EAGAIN)
				atomic64_inc(&state->data_wr_rnr_retry_exhausted);
			tbv_send_complete(send, send->completion_status);
			tbv_send_ctx_put(send);
		}
		if (completed_error && completed_ack)
			tbv_qp_mark_error(tqp);
		tbv_qp_put(tqp);
		return;
	}

	if (hdr->opcode == TBV_NATIVE_DATA_OP_RDMA_READ_ACK) {
		struct tbv_read_resp_ctx *ctx;

		atomic64_inc(&state->data_rx_ack);
		tbv_count_rx_read_ack(state, hdr->imm_data);
		if (hdr->imm_data == TBV_NATIVE_READ_ACK_RETRY) {
			tbv_qp_retry_read_resp(tqp, hdr->psn);
		} else {
			ctx = tbv_qp_take_read_resp(tqp, hdr->psn);
			if (ctx)
				tbv_read_resp_ctx_put(ctx);
		}
		tbv_qp_put(tqp);
		return;
	}

	if (hdr->opcode == TBV_NATIVE_DATA_OP_RDMA_READ_REQ) {
		tbv_rx_handle_rdma_read_req(state, tqp, hdr, rx_path);
		tbv_qp_put(tqp);
		return;
	}

	if (hdr->opcode == TBV_NATIVE_DATA_OP_RDMA_READ_RESP) {
		tbv_rx_handle_rdma_read_resp(state, tqp, hdr, payload, rx_path);
		tbv_qp_put(tqp);
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

	if (hdr->opcode == TBV_NATIVE_DATA_OP_SEND ||
	    hdr->opcode == TBV_NATIVE_DATA_OP_SEND_IMM)
		tbv_rx_handle_send_fragment(state, tqp, hdr, payload,
					    rx_path);
	else
		tbv_rx_handle_rdma_write_fragment(state, tqp, hdr, payload,
						  rx_path);
	tbv_qp_put(tqp);
}

void tbv_ibdev_rx_frame(struct tbv_state *state, struct tbv_path *rx_path,
			const void *data, u32 len)
{
	struct tbv_native_data_header hdr;
	const u8 *payload;
	int ret;

	if (!state || !state->verbs_registered)
		return;

	ret = tbv_native_data_parse_header(data, len, &hdr);
	if (ret) {
		tbv_rx_bad_header_parse_note(state, rx_path, data, len, ret);
		return;
	}
	if (hdr.length > len - TBV_NATIVE_DATA_HDR_SIZE) {
		tbv_rx_bad_header_note(state, rx_path,
				       &state->data_rx_bad_header_len,
				       "frame_len", &hdr, len, 0);
		return;
	}

	payload = (const u8 *)data + TBV_NATIVE_DATA_HDR_SIZE;
	tbv_ibdev_rx_native_frame(state, rx_path, &hdr, payload);
}

static int tbv_mr_publish(struct tbv_mr *mr, struct ib_pd *pd)
{
	struct tbv_state *owner = tbv_ibdev_state(pd->device);
	unsigned long flags;
	u32 key;
	int ret;

	key = atomic_inc_return(&tbv_mr_key);
	mr->base.device = pd->device;
	mr->base.pd = pd;
	mr->base.lkey = key;
	mr->base.rkey = key;
	mr->owner = owner;
	refcount_set(&mr->refs, 1);
	INIT_WORK(&mr->free_work, tbv_mr_free_work);

	xa_lock_irqsave(&owner->verbs_mrs_xa, flags);
	ret = __xa_insert(&owner->verbs_mrs_xa, key, mr, GFP_KERNEL);
	xa_unlock_irqrestore(&owner->verbs_mrs_xa, flags);
	if (ret)
		return ret;

	atomic_inc(&owner->verbs_mrs);
	return 0;
}

static struct ib_mr *tbv_get_dma_mr(struct ib_pd *pd, int access)
{
	struct tbv_mr *mr;
	int ret;

	mr = kzalloc(sizeof(*mr), GFP_KERNEL);
	if (!mr)
		return ERR_PTR(-ENOMEM);

	mr->base.type = IB_MR_TYPE_DMA;
	mr->base.iova = 0;
	mr->base.length = U64_MAX;
	mr->start = 0;
	mr->length = U64_MAX;
	mr->virt_addr = 0;
	mr->access = access;
	mr->dma_mr = true;

	ret = tbv_mr_publish(mr, pd);
	if (ret) {
		kfree(mr);
		return ERR_PTR(ret);
	}

	return &mr->base;
}

static struct ib_mr *tbv_reg_user_mr(struct ib_pd *pd, u64 start, u64 length,
				     u64 virt_addr, int access,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
				     struct ib_dmah *dmah,
#endif
				     struct ib_udata *udata)
{
	struct tbv_mr *mr;
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

	mr->base.type = IB_MR_TYPE_USER;
	mr->base.iova = virt_addr;
	mr->base.length = length;
	mr->start = start;
	mr->length = length;
	mr->virt_addr = virt_addr;
	mr->access = access;
	ret = tbv_mr_publish(mr, pd);
	if (ret) {
		ib_umem_release(mr->umem);
		kfree(mr);
		return ERR_PTR(ret);
	}
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
	.add_gid = tbv_add_gid,
	.del_gid = tbv_del_gid,
	.get_dma_mr = tbv_get_dma_mr,

	.alloc_ucontext = tbv_alloc_ucontext,
	.dealloc_ucontext = tbv_dealloc_ucontext,
	.alloc_pd = tbv_alloc_pd,
	.dealloc_pd = tbv_dealloc_pd,
	.create_ah = tbv_create_ah,
	.modify_ah = tbv_modify_ah,
	.query_ah = tbv_query_ah,
	.destroy_ah = tbv_destroy_ah,
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
	INIT_RDMA_OBJ_SIZE(ib_ah, tbv_ah, base),
	INIT_RDMA_OBJ_SIZE(ib_cq, tbv_cq, base),
	INIT_RDMA_OBJ_SIZE(ib_qp, tbv_qp, base),
};

static int tbv_ibdev_register_one(struct tbv_state *state,
				  struct tbv_rail *rail,
				  const char *name,
				  struct tbv_ibdev **out)
{
	enum tbv_backend_type backend = rail->peer->backend;
	struct tbv_ibdev *dev;
	struct device *dma_device;
	int ret;

	if (!rail->path.tx_ring)
		return -ENODEV;

	dev = ib_alloc_device(tbv_ibdev, base);
	if (!dev)
		return -ENOMEM;

	dev->state = state;
	dev->backend = backend;
	dev->rail = rail;
	dev->base.phys_port_cnt = TBV_IBDEV_PORTS;
	dev->base.num_comp_vectors = num_possible_cpus();
	dev->base.local_dma_lkey = 0;
	dev->base.node_type = RDMA_NODE_IB_CA;
	/*
	 * Encode rail identity in the low bits so each lane's ib_device has
	 * a distinct GID — RCCL keys discovery on this.
	 */
	dev->base.node_guid =
		cpu_to_be64(0x0200544256524253ULL +
			    ((u64)backend << 56) +
			    ((u64)rail->peer->peer_id << 24) +
			    rail->rail_id);
	dev->base.uverbs_cmd_mask |=
		BIT_ULL(IB_USER_VERBS_CMD_POST_SEND) |
		BIT_ULL(IB_USER_VERBS_CMD_POST_RECV) |
		BIT_ULL(IB_USER_VERBS_CMD_POLL_CQ) |
		BIT_ULL(IB_USER_VERBS_CMD_REQ_NOTIFY_CQ);

	ib_set_device_ops(&dev->base, &tbv_ibdev_ops);

	dma_device = get_device(tb_ring_dma_device(rail->path.tx_ring));
	dev->base.dev.parent = dma_device;
	ret = tbv_ibdev_attach_netdev(dev);
	if (ret) {
		if (ret == -EPROBE_DEFER)
			pr_debug("deferred %s RoCE netdev %s attach for %s\n",
				 tbv_backend_name(backend),
				 tbv_ibdev_netdev_name(dev) ?: "(none)",
				 name);
		else
			pr_warn("failed to attach %s RoCE netdev %s for %s: %d\n",
				tbv_backend_name(backend),
				tbv_ibdev_netdev_name(dev) ?: "(none)",
				name, ret);
		put_device(dma_device);
		ib_dealloc_device(&dev->base);
		return ret;
	}

	/*
	 * This is a software verbs provider. User MRs remain pinned by
	 * ib_umem_get(), and kernel local-DMA SGEs must stay CPU-visible for
	 * MAD/CM QP1 handling. Keep the Thunderbolt ring device as the parent,
	 * but use RDMA-core virtual DMA for verbs buffer addresses.
	 */
	ret = ib_register_device(&dev->base, name, NULL);
	if (ret) {
		tbv_ibdev_detach_netdev(dev);
		put_device(dma_device);
		ib_dealloc_device(&dev->base);
		return ret;
	}

	if (out)
		*out = dev;
	pr_info("registered %s ib_device %s rail=peer%u/%u domain=%d guid=%016llx\n",
		tbv_backend_name(backend), dev_name(&dev->base.dev),
		rail->peer->peer_id, rail->rail_id,
		rail->peer->xd && rail->peer->xd->tb ?
		rail->peer->xd->tb->index : -1,
		be64_to_cpu(dev->base.node_guid));
	put_device(dma_device);
	return 0;
}

/*
 * Compute the deterministic ib_device name suffix for a per-lane rail.
 *
 * The historical commit (module/ ca70710) called out that probe-order
 * naming gives different ib_device numbers on different nodes for the
 * same physical lane, which breaks any tooling that pins to "usb4_rdma2".
 * Use (tb_domain_index * TBV_NATIVE_MAX_LANES + lane) so the same lane on
 * the same domain always gets the same name. Apple peers don't carry a
 * lane subdivision, so they take the per-domain "Apple slot" which lives
 * just above the native lane range.
 */
static int tbv_ibdev_rail_name_index(const struct tbv_rail *rail)
{
	int domain_idx;
	unsigned int slot;

	if (!rail || !rail->peer || !rail->peer->xd || !rail->peer->xd->tb)
		return -ENODEV;

	domain_idx = rail->peer->xd->tb->index;
	if (domain_idx < 0)
		return -ENODEV;

	if (rail->peer->backend == TBV_BACKEND_NATIVE) {
		if (rail->native_lane >= TBV_NATIVE_MAX_LANES)
			return -ERANGE;
		slot = rail->native_lane;
	} else {
		/* One slot per (domain, backend) for Apple. */
		slot = TBV_NATIVE_MAX_LANES;
	}

	return domain_idx * (TBV_NATIVE_MAX_LANES + 1) + slot;
}

int tbv_ibdev_rail_event(struct tbv_state *state, struct tbv_rail *rail,
			 bool joined)
{
	struct tbv_ibdev *dev;
	char name[16];
	bool ready;
	int idx;
	int ret;

	if (!state || !rail)
		return 0;

	if (!joined) {
		/*
		 * Down edge is unconditional: even after tbv_ibdev_stop() has
		 * disabled new joins, an already-published ib_device still has
		 * to be unregistered when its rail goes away.
		 */
		mutex_lock(&state->rail_register_lock);
		dev = rail->ibdev;
		rail->ibdev = NULL;
		rail->ibdev_register_deferred = false;
		mutex_unlock(&state->rail_register_lock);
		if (!dev)
			return 0;
		tbv_ibdev_detach_netdev(dev);
		ib_unregister_device(&dev->base);
		ib_dealloc_device(&dev->base);
		pr_info("unregistered per-rail ib_device peer=%u rail=%u\n",
			rail->peer->peer_id, rail->rail_id);
		return 0;
	}

	mutex_lock(&state->rail_register_lock);
	if (!state->register_enabled) {
		mutex_unlock(&state->rail_register_lock);
		return 0;
	}
	if (rail->ibdev) {
		mutex_unlock(&state->rail_register_lock);
		return 0;
	}
	if (rail->ibdev_register_failed) {
		mutex_unlock(&state->rail_register_lock);
		return 0;
	}
	if (rail->ibdev_register_deferred) {
		mutex_unlock(&state->rail_register_lock);
		return 0;
	}

	/*
	 * Re-check removing under the registration lock. tbv_peer_remove_rail()
	 * sets rail->removing (under state->lock) before invoking the down
	 * event, which itself serializes on rail_register_lock. So either:
	 *  - remove gets here first: its down event publishes nothing (ibdev
	 *    was NULL) and removing is set, so our check below skips publish.
	 *  - we get here first: we publish, remove's down event then tears
	 *    rail->ibdev down under the same lock.
	 *
	 * The data-ready check is best-effort: we re-evaluate it under
	 * state->lock for stable list/state reads, but the source-of-truth for
	 * "is this rail going away" is rail->removing, which is monotonic.
	 */
	mutex_lock(&state->lock);
	if (rail->peer->backend == TBV_BACKEND_NATIVE)
		ready = tbv_rail_data_ready(rail);
	else
		ready = tbv_rail_apple_data_ready(rail);
	ready = ready && !rail->removing;
	mutex_unlock(&state->lock);
	if (!ready) {
		mutex_unlock(&state->rail_register_lock);
		return 0;
	}

	idx = tbv_ibdev_rail_name_index(rail);
	if (idx < 0) {
		rail->ibdev_register_failed = true;
		rail->ibdev_register_deferred = false;
		mutex_unlock(&state->rail_register_lock);
		pr_warn("rail event ignored: cannot derive deterministic name (peer=%u rail=%u err=%d)\n",
			rail->peer->peer_id, rail->rail_id, idx);
		return idx;
	}
	snprintf(name, sizeof(name), "usb4_rdma%d", idx);
	dev = NULL;
	ret = tbv_ibdev_register_one(state, rail, name, &dev);
	if (ret) {
		if (ret == -EPROBE_DEFER) {
			rail->ibdev_register_deferred = true;
			mutex_unlock(&state->rail_register_lock);
			pr_info("deferred per-rail ib_device %s registration until RoCE netdev %s exists\n",
				name,
				tbv_ibdev_netdev_name_for(state,
							  rail->peer->backend) ?:
				"(none)");
			return ret;
		}
		rail->ibdev_register_failed = true;
		rail->ibdev_register_deferred = false;
		mutex_unlock(&state->rail_register_lock);
		pr_warn("failed to register per-rail ib_device %s: %d (will not retry automatically)\n",
			name, ret);
		return ret;
	}
	rail->ibdev_register_deferred = false;
	rail->ibdev = dev;
	mutex_unlock(&state->rail_register_lock);
	return 0;
}

static bool tbv_ibdev_required_netdev_registered(struct tbv_state *state,
						const struct tbv_rail *rail)
{
	const char *name;
	struct net_device *ndev;
	bool registered;

	if (!rail || !rail->peer)
		return false;

	name = tbv_ibdev_netdev_name_for(state, rail->peer->backend);
	if (!name)
		return true;

	ndev = dev_get_by_name(&init_net, name);
	if (!ndev)
		return false;

	registered = ndev->reg_state == NETREG_REGISTERED;
	dev_put(ndev);
	return registered;
}

static void tbv_ibdev_netdev_retry_work(struct work_struct *work)
{
	struct tbv_state *state =
		container_of(work, struct tbv_state, ibdev_netdev_retry_work);
	struct tbv_peer *peer;

	for (;;) {
		struct tbv_rail *target = NULL;
		bool skip = false;

		mutex_lock(&state->lock);
		list_for_each_entry(peer, &state->peers, node) {
			struct tbv_rail *rail;
			bool ready;

			list_for_each_entry(rail, &peer->rails, node) {
				if (!READ_ONCE(rail->ibdev_register_deferred) ||
				    READ_ONCE(rail->ibdev) ||
				    READ_ONCE(rail->ibdev_register_failed))
					continue;
				if (!tbv_ibdev_required_netdev_registered(state,
									 rail))
					continue;
				if (peer->backend == TBV_BACKEND_NATIVE)
					ready = tbv_rail_data_ready(rail);
				else
					ready = tbv_rail_apple_data_ready(rail);
				if (!ready || rail->removing)
					continue;
				refcount_inc(&rail->refcnt);
				target = rail;
				break;
			}
			if (target)
				break;
		}
		mutex_unlock(&state->lock);

		if (!target)
			break;

		mutex_lock(&state->rail_register_lock);
		if (target->ibdev_register_deferred && !target->ibdev &&
		    !target->ibdev_register_failed) {
			/*
			 * Clear the latch before replaying the up-edge. The
			 * event path re-checks all publication gates under the
			 * same lock and would otherwise return early.
			 */
			target->ibdev_register_deferred = false;
		} else {
			skip = true;
		}
		mutex_unlock(&state->rail_register_lock);

		if (!skip)
			tbv_ibdev_rail_event(state, target, true);
		tbv_rail_put(target);
	}
}

static void tbv_ibdev_detach_matching_netdev(struct tbv_state *state,
					     struct net_device *ndev,
					     bool detach_renamed)
{
	struct tbv_peer *peer;

	mutex_lock(&state->rail_register_lock);
	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		list_for_each_entry(rail, &peer->rails, node) {
			struct tbv_ibdev *dev = rail->ibdev;
			const char *expected_name;

			if (!dev || dev->netdev != ndev)
				continue;

			expected_name = tbv_ibdev_netdev_name(dev);
			if (detach_renamed && expected_name &&
			    !strcmp(expected_name, ndev->name))
				continue;

			pr_info("detaching ib_device %s from %s netdev %s%s%s\n",
				dev_name(&dev->base.dev),
				detach_renamed ? "renamed" : "unregistering",
				ndev->name,
				detach_renamed && expected_name ?
					", expected " : "",
				detach_renamed && expected_name ?
					expected_name : "");
			tbv_ibdev_detach_netdev(dev);
		}
	}
	mutex_unlock(&state->lock);
	mutex_unlock(&state->rail_register_lock);
}

static int tbv_ibdev_netdev_event(struct notifier_block *nb,
				  unsigned long event, void *ptr)
{
	struct tbv_state *state =
		container_of(nb, struct tbv_state, ibdev_netdev_nb);
	struct net_device *ndev = netdev_notifier_info_to_dev(ptr);

	switch (event) {
	case NETDEV_REGISTER:
	case NETDEV_UP:
	case NETDEV_CHANGE:
	case NETDEV_CHANGEADDR:
		if (state->workqueue)
			queue_work(state->workqueue,
				   &state->ibdev_netdev_retry_work);
		return NOTIFY_DONE;
	case NETDEV_CHANGENAME:
		tbv_ibdev_detach_matching_netdev(state, ndev, true);
		if (state->workqueue)
			queue_work(state->workqueue,
				   &state->ibdev_netdev_retry_work);
		return NOTIFY_DONE;
	case NETDEV_UNREGISTER:
		tbv_ibdev_detach_matching_netdev(state, ndev, false);
		return NOTIFY_DONE;
	default:
		return NOTIFY_DONE;
	}
}

static int tbv_ibdev_register_netdev_notifier(struct tbv_state *state)
{
	int ret;

	if (state->ibdev_netdev_nb_registered)
		return 0;

	INIT_WORK(&state->ibdev_netdev_retry_work,
		  tbv_ibdev_netdev_retry_work);
	state->ibdev_netdev_nb.notifier_call = tbv_ibdev_netdev_event;
	ret = register_netdevice_notifier(&state->ibdev_netdev_nb);
	if (ret)
		return ret;

	state->ibdev_netdev_nb_registered = true;
	return 0;
}

static void tbv_ibdev_unregister_netdev_notifier(struct tbv_state *state)
{
	if (!state->ibdev_netdev_nb_registered)
		return;

	unregister_netdevice_notifier(&state->ibdev_netdev_nb);
	state->ibdev_netdev_nb_registered = false;
	cancel_work_sync(&state->ibdev_netdev_retry_work);
}

int tbv_ibdev_start(struct tbv_state *state, bool register_verbs)
{
	struct tbv_peer *peer;
	struct tbv_rail *catchup;
	int ret;

	state->register_verbs = register_verbs;
	if (!register_verbs)
		return 0;

	ret = tbv_ibdev_register_netdev_notifier(state);
	if (ret)
		return ret;

	state->verbs_registered = true;
	mutex_lock(&state->rail_register_lock);
	state->register_enabled = true;
	mutex_unlock(&state->rail_register_lock);

	/*
	 * Catch up any rails that became data-ready before this ran. After
	 * register_enabled is true, native_control and service hooks publish
	 * rising-edge events directly; this loop replays the edges they
	 * would have missed. Rails that previously failed permanently or are
	 * waiting for a late RoCE netdev are skipped so a single lane cannot
	 * spin the loop forever.
	 */
	for (;;) {
		bool ready;

		catchup = NULL;
		mutex_lock(&state->lock);
		list_for_each_entry(peer, &state->peers, node) {
			struct tbv_rail *rail;

			list_for_each_entry(rail, &peer->rails, node) {
				/*
				 * Read registration state without
				 * rail_register_lock: this is only a hint to
				 * avoid waking the event on rails that have
				 * obviously already been handled or deferred.
				 * The real gates are re-checked inside the
				 * event under rail_register_lock. Taking
				 * rail_register_lock here would invert the lock
				 * order we use in tbv_ibdev_rail_event
				 * (register-then-state).
				 */
				if (READ_ONCE(rail->ibdev) ||
				    READ_ONCE(rail->ibdev_register_failed) ||
				    READ_ONCE(rail->ibdev_register_deferred))
					continue;
				if (peer->backend == TBV_BACKEND_NATIVE)
					ready = tbv_rail_data_ready(rail);
				else
					ready = tbv_rail_apple_data_ready(rail);
				if (!ready || rail->removing)
					continue;
				refcount_inc(&rail->refcnt);
				catchup = rail;
				break;
			}
			if (catchup)
				break;
		}
		mutex_unlock(&state->lock);
		if (!catchup)
			break;
		/*
		 * Ignore the per-rail return: on failure the rail is marked
		 * failed or deferred inside the event, so the next loop
		 * iteration skips it and we make forward progress instead of
		 * pinning the same lane. Other rails should still publish.
		 */
		tbv_ibdev_rail_event(state, catchup, true);
		tbv_rail_put(catchup);
	}
	return 0;
}

void tbv_ibdev_stop(struct tbv_state *state)
{
	struct tbv_peer *peer;
	bool any;

	/*
	 * Disable rising-edge registrations before draining. Doing this under
	 * rail_register_lock ensures any in-flight up event either runs to
	 * completion before us (its ib_device is then visible in the loop
	 * below) or observes register_enabled=false and bails out without
	 * publishing.
	 */
	mutex_lock(&state->rail_register_lock);
	state->register_enabled = false;
	mutex_unlock(&state->rail_register_lock);

	state->verbs_registered = false;
	if (state->workqueue)
		flush_workqueue(state->workqueue);

	/*
	 * Walk every peer/rail and unregister anything still published.
	 * ib_unregister_device may block waiting for verbs ops to drain, so
	 * we drop state->lock around each event.
	 */
	do {
		struct tbv_rail *target = NULL;

		any = false;
		mutex_lock(&state->lock);
		list_for_each_entry(peer, &state->peers, node) {
			struct tbv_rail *rail;

			list_for_each_entry(rail, &peer->rails, node) {
				/*
				 * Sample rail->ibdev with READ_ONCE; nested
				 * rail_register_lock under state->lock would
				 * invert the order used in tbv_ibdev_rail_event.
				 * register_enabled is already false above, so no
				 * new ibdev can appear; the event itself
				 * re-checks under the registration lock and is
				 * a no-op for already-cleared rails.
				 */
				if (!READ_ONCE(rail->ibdev))
					continue;
				target = rail;
				refcount_inc(&rail->refcnt);
				break;
			}
			if (target)
				break;
		}
		mutex_unlock(&state->lock);
		if (target) {
			tbv_ibdev_rail_event(state, target, false);
			tbv_rail_put(target);
			any = true;
		}
	} while (any);

	if (state->workqueue)
		flush_workqueue(state->workqueue);
	tbv_ibdev_unregister_netdev_notifier(state);
	ida_destroy(&tbv_qpn_ida);
}
