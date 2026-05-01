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
 *   - Ring callbacks fire in softirq context — no sleeping, no
 *     userspace copies. We hand off to a worker for anything that
 *     needs to touch user memory.
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
#include <linux/workqueue.h>

#include "usb4_rdma.h"
#include "wire.h"

#define U4_DATA_RING_DEPTH         128
#define U4_DATA_FRAMES_PER_DIR     96
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

	struct u4_data_frame *tx_frames;
	struct u4_data_frame *rx_frames;

	atomic_t tx_inflight;
	atomic_t refs;
	bool closing;
	struct mutex tx_lock;
	wait_queue_head_t tx_wait;
	wait_queue_head_t ref_wait;

	bool raw_pending;
	struct u4_wire_hdr raw_hdr;
	u64 raw_base;
	u32 raw_done;
	u32 raw_remaining;

	/* Stats — exposed via debugfs */
	atomic64_t tx_frames_sent;
	atomic64_t rx_frames_recv;
	atomic64_t rx_frames_dropped;
	atomic64_t rx_invalid_hdr;
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

/* QP routing — RX dispatch finds the local QP by qp_num. */
struct u4_data_qp_entry {
	u32 qp_num;
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

static struct u4_data_peer *u4_data_peer_get_at(unsigned int target)
{
	struct u4_data_peer *p, *pick = NULL;
	int count = 0;
	int i = 0;

	read_lock(&peer_lock);
	list_for_each_entry(p, &peer_list, list) {
		if (!READ_ONCE(p->closing))
			count++;
	}
	if (!count)
		goto out;

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
out:
	read_unlock(&peer_lock);
	return pick;
}

static struct u4_data_peer *u4_data_peer_get(void)
{
	return u4_data_peer_get_at(atomic_inc_return(&peer_rr));
}

static struct u4_data_peer *u4_data_peer_get_by_qp(u32 src_qp, u32 dest_qp)
{
	return u4_data_peer_get_at(hash_32(src_qp ^ dest_qp, 32));
}

static int u4_data_peers_get(struct u4_data_peer **peers, int max)
{
	struct u4_data_peer *p;
	int n = 0;

	read_lock(&peer_lock);
	list_for_each_entry(p, &peer_list, list) {
		if (READ_ONCE(p->closing))
			continue;
		atomic_inc(&p->refs);
		peers[n++] = p;
		if (n == max)
			break;
	}
	read_unlock(&peer_lock);
	return n;
}

static void u4_data_peer_put(struct u4_data_peer *p)
{
	if (atomic_dec_return(&p->refs) == 1)
		wake_up(&p->ref_wait);
}

static bool u4_data_tx_room(struct u4_data_peer *p, int needed)
{
	return atomic_read(&p->tx_inflight) <= U4_DATA_FRAMES_PER_DIR - needed;
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

	for (i = 0; i < U4_DATA_FRAMES_PER_DIR; i++) {
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

/* ----- ring callbacks --------------------------------------------- */

static void u4_data_tx_complete(struct tb_ring *ring, struct ring_frame *frame,
				bool canceled)
{
	struct u4_data_frame *f = container_of(frame, typeof(*f), frame);
	struct device *dma_dev = tb_ring_dma_device(ring);

	dma_sync_single_for_cpu(dma_dev, f->dma, U4_FRAME_SIZE, DMA_TO_DEVICE);
	atomic_dec(&f->peer->tx_inflight);
	atomic_set(&f->in_use, 0);
	wake_up(&f->peer->tx_wait);
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
	wake_up(&zf->peer->tx_wait);
	if (!canceled)
		atomic64_inc(&zf->peer->tx_frames_sent);
	if (zf->done)
		zf->done(zf->done_ctx);
	kfree(zf);
}

static void u4_data_dispatch_raw(struct u4_data_peer *p, const void *payload,
				 u32 frame_len)
{
	struct u4_wire_hdr hdr = p->raw_hdr;
	struct u4_data_qp_entry *qe;
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
	hash_for_each_possible_rcu(u4_data_qp_table, qe, node, dest_qp) {
		if (qe->qp_num == dest_qp) {
			target_qp = qe->qp;
			break;
		}
	}
	if (target_qp && u4_data_rx_handler)
		u4_data_rx_handler(target_qp, &hdr, payload, length);
	rcu_read_unlock();

	if (!target_qp)
		atomic64_inc(&p->rx_frames_dropped);
}

static void u4_data_rx_complete(struct tb_ring *ring, struct ring_frame *frame,
				bool canceled)
{
	struct u4_data_frame *f = container_of(frame, typeof(*f), frame);
	struct u4_wire_hdr *hdr;
	void *payload;
	u32 length;
	struct u4_data_qp_entry *qe;
	void *target_qp = NULL;
	u32 dest_qp;
	struct device *dma_dev = tb_ring_dma_device(ring);
	u32 frame_len;

	if (canceled)
		return;

	dma_sync_single_for_cpu(dma_dev, f->dma, U4_FRAME_SIZE, DMA_FROM_DEVICE);
	atomic64_inc(&f->peer->rx_frames_recv);
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
	hash_for_each_possible_rcu(u4_data_qp_table, qe, node, dest_qp) {
		if (qe->qp_num == dest_qp) {
			target_qp = qe->qp;
			break;
		}
	}
	if (target_qp && u4_data_rx_handler)
		u4_data_rx_handler(target_qp, hdr, payload, length);
	rcu_read_unlock();

	if (!target_qp)
		atomic64_inc(&f->peer->rx_frames_dropped);

repost:
	/* Re-queue this RX buffer for the next frame. */
	dma_sync_single_for_device(dma_dev, f->dma, U4_FRAME_SIZE,
				   DMA_FROM_DEVICE);
	tb_ring_rx(ring, &f->frame);
}

/* ----- public: peer attach / detach ------------------------------- */

static int u4_data_prepare_lane(struct u4_data_peer *p, struct tb_service *svc,
				int lane_idx)
{
	struct tb_xdomain *xd = tb_service_parent(svc);
	u16 sof_mask = BIT(U4_DATA_PDF_FRAME_START);
	u16 eof_mask = BIT(U4_DATA_PDF_FRAME_END);
	int out_hop, ret, i;

	p->svc = svc;
	p->xd = xd;
	p->lane_idx = lane_idx;
	p->out_hop = -1;
	p->in_hop = -1;
	atomic_set(&p->refs, 1);
	mutex_init(&p->tx_lock);
	init_waitqueue_head(&p->tx_wait);
	init_waitqueue_head(&p->ref_wait);
	INIT_LIST_HEAD(&p->list);

	out_hop = tb_xdomain_alloc_out_hopid(xd, -1);
	if (out_hop < 0)
		return out_hop;
	p->out_hop = out_hop;

	p->tx_ring = tb_ring_alloc_tx(xd->tb->nhi, -1, U4_DATA_RING_DEPTH,
				      RING_FLAG_FRAME | RING_FLAG_E2E);
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

	p->rx_ring = tb_ring_alloc_rx(xd->tb->nhi, -1, U4_DATA_RING_DEPTH,
				      RING_FLAG_FRAME | RING_FLAG_E2E,
				      p->tx_ring->hop, sof_mask, eof_mask,
				      NULL, NULL);
	if (!p->rx_ring) {
		dev_info(&svc->dev,
			 "data: lane %d unavailable: RX ring allocation failed (route=0x%llx, tx_ring_hop=%d, out_hop=%d)\n",
			 lane_idx, xd->route, p->tx_ring->hop, p->out_hop);
		ret = -ENOSPC;
		goto err_tx;
	}

	ret = alloc_frames(p, &p->tx_frames, U4_DATA_FRAMES_PER_DIR, true);
	if (ret) goto err_rx;
	ret = alloc_frames(p, &p->rx_frames, U4_DATA_FRAMES_PER_DIR, false);
	if (ret) goto err_tx_frames;

	tb_ring_start(p->tx_ring);
	tb_ring_start(p->rx_ring);

	for (i = 0; i < U4_DATA_FRAMES_PER_DIR; i++) {
		p->rx_frames[i].frame.callback = u4_data_rx_complete;
		ret = tb_ring_rx(p->rx_ring, &p->rx_frames[i].frame);
		if (ret) {
			pr_warn("lane %d: post rx %d: %d\n", lane_idx, i, ret);
			goto err_started;
		}
	}

	dev_info(&svc->dev,
		 "data: lane %d prepared, ring hops tx=%d rx=%d, local transmit path=%d\n",
		 lane_idx, p->tx_ring->hop, p->rx_ring->hop, p->out_hop);
	return 0;

err_started:
	tb_ring_stop(p->tx_ring);
	tb_ring_stop(p->rx_ring);
	free_frames(p, p->rx_frames, U4_DATA_FRAMES_PER_DIR, false);
err_tx_frames:
	free_frames(p, p->tx_frames, U4_DATA_FRAMES_PER_DIR, true);
err_rx:
	tb_ring_free(p->rx_ring);
err_tx:
	tb_ring_free(p->tx_ring);
err_out:
	tb_xdomain_release_out_hopid(xd, out_hop);
	return ret;
}

static int u4_data_enable_lane(struct u4_data_peer *p, int remote_tx_path)
{
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

	write_lock(&peer_lock);
	list_add_tail(&p->list, &peer_list);
	write_unlock(&peer_lock);

	dev_info(&p->svc->dev,
		 "data: lane %d attached, ring hops tx=%d rx=%d, local transmit path=%d remote transmit path=%d\n",
		 p->lane_idx, p->tx_ring->hop, p->rx_ring->hop,
		 p->out_hop, p->in_hop);
	return 0;
}

static void u4_data_teardown_lane(struct u4_data_peer *p)
{
	wake_up_all(&p->tx_wait);
	wait_event(p->ref_wait, atomic_read(&p->refs) == 1);

	if (p->paths_enabled)
		tb_xdomain_disable_paths(p->xd, p->out_hop, p->tx_ring->hop,
					 p->in_hop, p->rx_ring->hop);
	tb_ring_stop(p->tx_ring);
	tb_ring_stop(p->rx_ring);
	free_frames(p, p->rx_frames, U4_DATA_FRAMES_PER_DIR, false);
	free_frames(p, p->tx_frames, U4_DATA_FRAMES_PER_DIR, true);
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
	write_lock(&peer_lock);
	if (!list_empty(&p->list))
		list_del_init(&p->list);
	WRITE_ONCE(p->closing, true);
	write_unlock(&peer_lock);

	u4_data_teardown_lane(p);
}

int usb4_rdma_data_init(struct dentry *parent_dir)
{
	int ret;

	(void)parent_dir;
	INIT_LIST_HEAD(&u4_login_handler.list);
	u4_login_handler.uuid = &u4_login_uuid;
	u4_login_handler.callback = u4_login_handle_packet;
	u4_login_handler.data = NULL;

	ret = tb_register_protocol_handler(&u4_login_handler);
	if (ret)
		return ret;
	u4_login_handler_registered = true;
	return 0;
}

void usb4_rdma_data_exit(void)
{
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

int usb4_rdma_data_send(u8 opcode, u32 src_qp, u32 dest_qp, u32 psn,
			u8 flags, __be32 imm_data, u64 remote_addr, u32 rkey,
			usb4_rdma_data_fill_fn fill, void *fill_ctx,
			u32 length)
{
	struct u4_data_peer *p;
	struct u4_data_frame *f = NULL;
	int ret;

	if (length > U4_MAX_PAYLOAD)
		return -EMSGSIZE;
	if (length && !fill)
		return -EINVAL;

	p = u4_data_peer_get_by_qp(src_qp, dest_qp);
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
			wake_up(&p->tx_wait);
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
			wake_up(&p->tx_wait);
			mutex_unlock(&p->tx_lock);
			u4_data_peer_put(p);
			return ret;
		}
	}
	f->frame.size = U4_HDR_SIZE + length;
	f->frame.callback = u4_data_tx_complete;
	f->frame.sof = U4_DATA_PDF_FRAME_START;
	f->frame.eof = U4_DATA_PDF_FRAME_END;

	if (READ_ONCE(p->closing)) {
		atomic_set(&f->in_use, 0);
		wake_up(&p->tx_wait);
		mutex_unlock(&p->tx_lock);
		u4_data_peer_put(p);
		return -ENOTCONN;
	}

	dma_sync_single_for_device(tb_ring_dma_device(p->tx_ring), f->dma,
				   U4_FRAME_SIZE, DMA_TO_DEVICE);
	atomic_inc(&p->tx_inflight);
	ret = tb_ring_tx(p->tx_ring, &f->frame);
	if (ret) {
		dma_sync_single_for_cpu(tb_ring_dma_device(p->tx_ring), f->dma,
					U4_FRAME_SIZE, DMA_TO_DEVICE);
		atomic_dec(&p->tx_inflight);
		atomic_set(&f->in_use, 0);
		wake_up(&p->tx_wait);
		mutex_unlock(&p->tx_lock);
		u4_data_peer_put(p);
		return ret;
	}
	mutex_unlock(&p->tx_lock);
	u4_data_peer_put(p);
	return 0;
}

int usb4_rdma_data_send_page(u8 opcode, u32 src_qp, u32 dest_qp, u32 psn,
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

	p = u4_data_peer_get();
	if (!p)
		return -ENOTCONN;
	dma_dev = tb_ring_dma_device(p->tx_ring);

	zf = kzalloc(sizeof(*zf), GFP_KERNEL);
	if (!zf) {
		u4_data_peer_put(p);
		return -ENOMEM;
	}
	zf->peer = p;
	zf->length = length;
	zf->unmap_dma = true;
	zf->done = done;
	zf->done_ctx = done_ctx;
	zf->dma = dma_map_page(dma_dev, page, page_off, length,
			       DMA_TO_DEVICE);
	if (dma_mapping_error(dma_dev, zf->dma)) {
		kfree(zf);
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
	hdrf->frame.size = U4_HDR_SIZE;
	hdrf->frame.callback = u4_data_tx_complete;
	hdrf->frame.sof = U4_DATA_PDF_FRAME_START;
	hdrf->frame.eof = U4_DATA_PDF_FRAME_END;

	dma_sync_single_for_device(dma_dev, hdrf->dma, U4_FRAME_SIZE,
				   DMA_TO_DEVICE);
	atomic_inc(&p->tx_inflight);
	ret = tb_ring_tx(p->tx_ring, &hdrf->frame);
	if (ret) {
		dma_sync_single_for_cpu(dma_dev, hdrf->dma, U4_FRAME_SIZE,
					DMA_TO_DEVICE);
		atomic_dec(&p->tx_inflight);
		atomic_set(&hdrf->in_use, 0);
		wake_up(&p->tx_wait);
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
	ret = tb_ring_tx(p->tx_ring, &zf->frame);
	if (ret) {
		atomic_dec(&p->tx_inflight);
		wake_up(&p->tx_wait);
		goto err_unlock;
	}

	mutex_unlock(&p->tx_lock);
	u4_data_peer_put(p);
	return 0;

err_unlock:
	if (hdrf) {
		atomic_set(&hdrf->in_use, 0);
		wake_up(&p->tx_wait);
	}
	mutex_unlock(&p->tx_lock);
	dma_unmap_page(dma_dev, zf->dma, zf->length, DMA_TO_DEVICE);
	kfree(zf);
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
		kfree(zf);
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
	u8 hdr_flags = (flags | U4_F_RAW_STREAM);
	int ret;

	if (!stream_length || remote_addr > U64_MAX - stream_off)
		return -EINVAL;
	if (!final_stream)
		hdr_flags &= ~U4_F_LAST;

	mutex_lock(&p->tx_lock);
	wait_event(p->tx_wait,
		   READ_ONCE(p->closing) ||
		   (u4_data_tx_room(p, 2) &&
		    (hdrf = u4_data_claim_tx_frame(p))));
	if (READ_ONCE(p->closing)) {
		ret = -ENOTCONN;
		goto out_unlock;
	}

	u4_wire_hdr_init((struct u4_wire_hdr *)hdrf->buf, opcode, dest_qp,
			 src_qp, psn, stream_length, hdr_flags, imm_data,
			 remote_addr + stream_off, rkey);
	hdrf->frame.size = U4_HDR_SIZE;
	hdrf->frame.callback = u4_data_tx_complete;
	hdrf->frame.sof = U4_DATA_PDF_FRAME_START;
	hdrf->frame.eof = U4_DATA_PDF_FRAME_END;

	dma_sync_single_for_device(dma_dev, hdrf->dma, U4_FRAME_SIZE,
				   DMA_TO_DEVICE);
	atomic_inc(&p->tx_inflight);
	ret = tb_ring_tx(p->tx_ring, &hdrf->frame);
	if (ret) {
		dma_sync_single_for_cpu(dma_dev, hdrf->dma, U4_FRAME_SIZE,
					DMA_TO_DEVICE);
		atomic_dec(&p->tx_inflight);
		atomic_set(&hdrf->in_use, 0);
		wake_up(&p->tx_wait);
		goto out_unlock;
	}
	hdrf = NULL;

	list_for_each_entry_safe(zf, tmp, frames, prep_link) {
		list_del(&zf->prep_link);
		zf->frame.buffer_phy = zf->dma;
		zf->frame.callback = u4_data_zcopy_tx_complete;
		zf->frame.size = zf->length == U4_FRAME_SIZE ? 0 : zf->length;
		zf->frame.sof = U4_DATA_PDF_FRAME_START;
		zf->frame.eof = U4_DATA_PDF_FRAME_END;
		INIT_LIST_HEAD(&zf->frame.list);

		wait_event(p->tx_wait,
			   READ_ONCE(p->closing) || u4_data_tx_room(p, 1));
		if (READ_ONCE(p->closing)) {
			list_add(&zf->prep_link, frames);
			ret = -ENOTCONN;
			goto out_unlock;
		}

		atomic_inc(&p->tx_inflight);
		ret = tb_ring_tx(p->tx_ring, &zf->frame);
		if (ret) {
			atomic_dec(&p->tx_inflight);
			wake_up(&p->tx_wait);
			list_add(&zf->prep_link, frames);
			goto out_unlock;
		}
	}
	ret = 0;

out_unlock:
	if (hdrf) {
		atomic_set(&hdrf->in_use, 0);
		wake_up(&p->tx_wait);
	}
	mutex_unlock(&p->tx_lock);
	return ret;
}

int usb4_rdma_data_send_page_stream(u8 opcode, u32 src_qp, u32 dest_qp,
				    u32 psn, u8 flags, __be32 imm_data,
				    u64 remote_addr, u32 rkey,
				    u32 total_length,
				    usb4_rdma_data_next_page_fn next,
				    void *next_ctx)
{
	struct list_head stripes[U4_MAX_ACTIVE_LANES];
	struct u4_data_peer *peers[U4_MAX_ACTIVE_LANES];
	u32 stripe_off[U4_MAX_ACTIVE_LANES];
	u32 stripe_len[U4_MAX_ACTIVE_LANES];
	bool stripe_seen[U4_MAX_ACTIVE_LANES];
	u32 prepared = 0;
	int last_stripe = -1;
	int npeers;
	int ret = 0;
	int i;

	if (!total_length || !next)
		return -EINVAL;

	npeers = u4_data_peers_get(peers, U4_MAX_ACTIVE_LANES);
	if (!npeers)
		return -ENOTCONN;

	for (i = 0; i < U4_MAX_ACTIVE_LANES; i++) {
		INIT_LIST_HEAD(&stripes[i]);
		stripe_off[i] = 0;
		stripe_len[i] = 0;
		stripe_seen[i] = false;
	}

	while (prepared < total_length) {
		struct u4_data_zcopy_frame *zf;
		struct device *dma_dev;
		struct page *page;
		u32 page_off, length;
		dma_addr_t dma_addr = 0;
		bool dma_mapped = false;
		usb4_rdma_data_done_fn done = NULL;
		void *done_ctx = NULL;
		u64 stripe;
		int lane;

		zf = kzalloc(sizeof(*zf), GFP_KERNEL);
		if (!zf) {
			ret = -ENOMEM;
			goto err_prepared;
		}

		ret = next(next_ctx, &page, &page_off, &length,
			   &dma_addr, &dma_mapped, &done, &done_ctx);
		if (ret) {
			kfree(zf);
			goto err_prepared;
		}
		if (dma_mapped && npeers > 1)
			dma_mapped = false;

		stripe = div_u64((u64)prepared * npeers, total_length);
		if (stripe >= npeers)
			stripe = npeers - 1;
		lane = stripe;
		if (!length || length > U4_FRAME_SIZE ||
		    length > total_length - prepared ||
		    (!dma_mapped &&
		     (!page || page_off > PAGE_SIZE ||
		      length > PAGE_SIZE - page_off))) {
			if (done)
				done(done_ctx);
			kfree(zf);
			ret = -EINVAL;
			goto err_prepared;
		}

		zf->peer = peers[lane];
		zf->length = length;
		zf->unmap_dma = !dma_mapped;
		zf->done = done;
		zf->done_ctx = done_ctx;
		dma_dev = tb_ring_dma_device(zf->peer->tx_ring);
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
				kfree(zf);
				ret = -EIO;
				goto err_prepared;
			}
		}
		INIT_LIST_HEAD(&zf->prep_link);
		if (!stripe_seen[lane]) {
			stripe_seen[lane] = true;
			stripe_off[lane] = prepared;
		}
		stripe_len[lane] += length;
		list_add_tail(&zf->prep_link, &stripes[lane]);
		prepared += length;
	}

	for (i = 0; i < npeers; i++) {
		if (stripe_seen[i])
			last_stripe = i;
	}

	for (i = 0; i < npeers; i++) {
		if (!stripe_seen[i])
			continue;
		ret = u4_data_submit_zcopy_stream(peers[i], opcode, src_qp,
						  dest_qp, psn, flags,
						  imm_data, remote_addr, rkey,
						  stripe_off[i], stripe_len[i],
						  &stripes[i],
						  i == last_stripe);
		if (ret)
			goto err_prepared;
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

int usb4_rdma_data_register_qp(u32 qp_num, void *qp)
{
	struct u4_data_qp_entry *qe;
	unsigned long flags;

	qe = kzalloc(sizeof(*qe), GFP_KERNEL);
	if (!qe)
		return -ENOMEM;
	qe->qp_num = qp_num;
	qe->qp = qp;

	spin_lock_irqsave(&u4_data_qp_lock, flags);
	hash_add_rcu(u4_data_qp_table, &qe->node, qp_num);
	spin_unlock_irqrestore(&u4_data_qp_lock, flags);
	return 0;
}

void usb4_rdma_data_unregister_qp(u32 qp_num)
{
	struct u4_data_qp_entry *qe;
	struct u4_data_qp_entry *dead = NULL;
	struct hlist_node *tmp;
	struct hlist_head *head;
	unsigned long flags;

	head = &u4_data_qp_table[hash_min(qp_num, U4_DATA_QP_HASH_BITS)];
	spin_lock_irqsave(&u4_data_qp_lock, flags);
	hlist_for_each_entry_safe(qe, tmp, head, node) {
		if (qe->qp_num == qp_num) {
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

bool usb4_rdma_data_peer_attached(void)
{
	return usb4_rdma_data_active_lane_count() > 0;
}

int usb4_rdma_data_active_lane_count(void)
{
	struct u4_data_peer *p;
	int count = 0;

	read_lock(&peer_lock);
	list_for_each_entry(p, &peer_list, list) {
		if (!READ_ONCE(p->closing))
			count++;
	}
	read_unlock(&peer_lock);
	return count;
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
