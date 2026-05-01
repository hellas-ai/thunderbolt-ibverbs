// SPDX-License-Identifier: GPL-2.0
/*
 * data.c — usb4_rdma data path: ring management, frame pool, RX dispatch.
 *
 * Each bound xdomain peer (a usb4rdma tb_service binding) gets one
 * TX/RX ring pair allocated here. ibdev.c routes verbs (post_send /
 * post_recv) into this layer; on RX we parse the wire header from
 * wire.h and dispatch to the matching local QP.
 *
 * Concurrency model:
 *   - Ring callbacks fire in softirq context — no sleeping, no
 *     userspace copies. We hand off to a worker for anything that
 *     needs to touch user memory.
 *   - QP table is RCU-protected for fast lookup on RX.
 *   - Per-CQ work-completion queue uses an irq-safe spinlock.
 *
 * For now there is at most one active peer per machine. The
 * usb4_rdma_data_get_peer() accessor returns the singleton; ibdev.c
 * routes all verbs through it. When we add multi-peer / multi-cable
 * support, we'll select per QP via the QP's "port" attribute.
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

#include "usb4_rdma.h"
#include "wire.h"

#define U4_DATA_RING_DEPTH         128
#define U4_DATA_FRAMES_PER_DIR     96
#define U4_DATA_PDF_FRAME_START    1
#define U4_DATA_PDF_FRAME_END      2

/* Both peers need to agree on the xdomain HopID that wire frames flow
 * over, otherwise tb_xdomain_enable_paths configures asymmetric routing
 * and frames sent by peer A arrive at the wrong tag on peer B and get
 * silently dropped.
 *
 * The proper fix is a tb_xdomain_request login dance like
 * tbnet_login_request does: each side allocates locally, then exchanges
 * the chosen hopids via xdomain control packets. That's a substantial
 * piece of work and lands later. For now, we hard-code a single
 * "rendezvous hop": both peers request the same hopid for both their
 * TX out path and their RX in path. enable_paths then sees
 * (local_tx=H, remote_tx=H) on both sides — identical and routable.
 *
 * Hopid is well above the TB_PATH_MIN_HOPID=8 floor and above what
 * thunderbolt_net (which uses 8/9) typically claims, so it's free
 * regardless of whether thunderbolt_net is loaded.
 *
 * If U4_RDV_HOP is already taken on one side, peer attach fails with
 * EBUSY. Workaround in that case: rmmod thunderbolt_net first. */
#define U4_RDV_HOP                 16

struct u4_data_frame {
	struct ring_frame frame;
	struct u4_data_peer *peer;
	void *buf;
	dma_addr_t dma;
	bool is_tx;
	atomic_t in_use;	/* 1 while submitted to ring, 0 when free */
};

struct u4_data_peer {
	struct tb_service *svc;
	struct tb_xdomain *xd;

	int out_hop;
	int in_hop;
	struct tb_ring *tx_ring;
	struct tb_ring *rx_ring;

	struct u4_data_frame *tx_frames;
	struct u4_data_frame *rx_frames;

	atomic_t tx_inflight;

	/* Stats — exposed via debugfs */
	atomic64_t tx_frames_sent;
	atomic64_t rx_frames_recv;
	atomic64_t rx_frames_dropped;
	atomic64_t rx_invalid_hdr;
};

/* Singleton for now. Switch to a list when we support multiple peers. */
static struct u4_data_peer *the_peer;
static DEFINE_RWLOCK(the_peer_lock);

/* QP routing — RX dispatch finds the local QP by qp_num. */
struct u4_data_qp_entry {
	u32 qp_num;
	void *qp;	/* opaque ib_qp; ibdev.c interprets */
	struct hlist_node node;
};

#define U4_DATA_QP_HASH_BITS  6
static DEFINE_HASHTABLE(u4_data_qp_table, U4_DATA_QP_HASH_BITS);
static DEFINE_SPINLOCK(u4_data_qp_lock);

/* RX dispatcher — called from ibdev.c (registered via init). */
static void (*u4_data_rx_handler)(void *qp,
				  const struct u4_wire_hdr *hdr,
				  const void *payload, u32 length);

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
		f->frame.size = U4_FRAME_SIZE;
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

	atomic_dec(&f->peer->tx_inflight);
	atomic_set(&f->in_use, 0);
	if (!canceled)
		atomic64_inc(&f->peer->tx_frames_sent);
}

static void u4_data_rx_complete(struct tb_ring *ring, struct ring_frame *frame,
				bool canceled)
{
	struct u4_data_frame *f = container_of(frame, typeof(*f), frame);
	struct u4_wire_hdr *hdr;
	void *payload;
	u32 length;
	struct hlist_head *head;
	struct u4_data_qp_entry *qe;
	void *target_qp = NULL;
	u32 dest_qp;
	unsigned long flags;

	if (canceled)
		return;

	atomic64_inc(&f->peer->rx_frames_recv);

	if (frame->size < U4_HDR_SIZE) {
		atomic64_inc(&f->peer->rx_invalid_hdr);
		goto repost;
	}

	hdr = (struct u4_wire_hdr *)f->buf;
	if (!u4_wire_hdr_ok(hdr)) {
		atomic64_inc(&f->peer->rx_invalid_hdr);
		goto repost;
	}

	length = le32_to_cpu(hdr->length);
	if (length > U4_MAX_PAYLOAD || U4_HDR_SIZE + length > frame->size) {
		atomic64_inc(&f->peer->rx_invalid_hdr);
		goto repost;
	}

	dest_qp = le32_to_cpu(hdr->dest_qp);
	head = &u4_data_qp_table[hash_min(dest_qp, U4_DATA_QP_HASH_BITS)];
	spin_lock_irqsave(&u4_data_qp_lock, flags);
	hlist_for_each_entry(qe, head, node) {
		if (qe->qp_num == dest_qp) {
			target_qp = qe->qp;
			break;
		}
	}
	spin_unlock_irqrestore(&u4_data_qp_lock, flags);

	if (!target_qp) {
		atomic64_inc(&f->peer->rx_frames_dropped);
		goto repost;
	}

	payload = (u8 *)f->buf + U4_HDR_SIZE;
	if (u4_data_rx_handler)
		u4_data_rx_handler(target_qp, hdr, payload, length);

repost:
	/* Re-queue this RX buffer for the next frame. */
	tb_ring_rx(ring, &f->frame);
}

/* ----- public: peer attach / detach ------------------------------- */

int usb4_rdma_data_attach_peer(struct tb_service *svc)
{
	struct u4_data_peer *p;
	struct tb_xdomain *xd = tb_service_parent(svc);
	u16 sof_mask = BIT(U4_DATA_PDF_FRAME_START);
	u16 eof_mask = BIT(U4_DATA_PDF_FRAME_END);
	int out_hop, in_hop, ret, i;
	const char *name;

	if (!xd)
		return -ENODEV;

	/* Deterministic cable selection. The service device name has the
	 * form "<domain>-<route>.<service>", where domain 0 is the first
	 * host router and 1 is the second. We always bind to domain 0 so
	 * both peers pick the same physical cable. Without this, peer A
	 * might bind on its cable 0 while peer B binds on its cable 1
	 * (the probes don't fire in a deterministic order across machines)
	 * and tb_xdomain_enable_paths configures unrelated routes —
	 * frames sent never arrive. */
	name = dev_name(&svc->dev);
	if (name && name[0] != '0') {
		dev_info(&svc->dev,
			 "data: skipping non-cable-0 peer (use cable 0 only for now)\n");
		return 0;
	}

	write_lock(&the_peer_lock);
	if (the_peer) {
		write_unlock(&the_peer_lock);
		dev_warn(&svc->dev,
			 "data: another peer already attached, skipping\n");
		return 0;
	}
	write_unlock(&the_peer_lock);

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->svc = svc;
	p->xd = xd;

	/* Both peers request the same rendezvous hopid — see U4_RDV_HOP comment. */
	out_hop = tb_xdomain_alloc_out_hopid(xd, U4_RDV_HOP);
	if (out_hop < 0) { ret = out_hop; goto err_free; }
	in_hop = tb_xdomain_alloc_in_hopid(xd, U4_RDV_HOP);
	if (in_hop < 0)  { ret = in_hop; goto err_out; }
	p->out_hop = out_hop;
	p->in_hop  = in_hop;

	p->tx_ring = tb_ring_alloc_tx(xd->tb->nhi, -1, U4_DATA_RING_DEPTH,
				      RING_FLAG_FRAME | RING_FLAG_E2E);
	if (!p->tx_ring) { ret = -ENOMEM; goto err_in; }

	p->rx_ring = tb_ring_alloc_rx(xd->tb->nhi, -1, U4_DATA_RING_DEPTH,
				      RING_FLAG_FRAME | RING_FLAG_E2E,
				      p->tx_ring->hop, sof_mask, eof_mask,
				      NULL, NULL);
	if (!p->rx_ring) { ret = -ENOMEM; goto err_tx; }

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
			pr_warn("post rx %d: %d\n", i, ret);
			goto err_started;
		}
	}

	ret = tb_xdomain_enable_paths(xd, p->out_hop, p->tx_ring->hop,
				      p->in_hop, p->rx_ring->hop);
	if (ret) {
		pr_warn("enable_paths failed: %d\n", ret);
		goto err_started;
	}

	write_lock(&the_peer_lock);
	the_peer = p;
	write_unlock(&the_peer_lock);

	dev_info(&svc->dev,
		 "data: peer attached, ring hops tx=%d rx=%d, xdomain hops out=%d in=%d\n",
		 p->tx_ring->hop, p->rx_ring->hop, p->out_hop, p->in_hop);
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
err_in:
	tb_xdomain_release_in_hopid(xd, in_hop);
err_out:
	tb_xdomain_release_out_hopid(xd, out_hop);
err_free:
	kfree(p);
	return ret;
}

void usb4_rdma_data_detach_peer(struct tb_service *svc)
{
	struct u4_data_peer *p;

	write_lock(&the_peer_lock);
	p = the_peer;
	if (p && p->svc != svc)
		p = NULL;
	if (p)
		the_peer = NULL;
	write_unlock(&the_peer_lock);

	if (!p)
		return;

	tb_xdomain_disable_paths(p->xd, p->out_hop, p->tx_ring->hop,
				 p->in_hop, p->rx_ring->hop);
	tb_ring_stop(p->tx_ring);
	tb_ring_stop(p->rx_ring);
	free_frames(p, p->rx_frames, U4_DATA_FRAMES_PER_DIR, false);
	free_frames(p, p->tx_frames, U4_DATA_FRAMES_PER_DIR, true);
	tb_ring_free(p->rx_ring);
	tb_ring_free(p->tx_ring);
	tb_xdomain_release_in_hopid(p->xd, p->in_hop);
	tb_xdomain_release_out_hopid(p->xd, p->out_hop);
	kfree(p);
	dev_info(&svc->dev, "data: peer detached\n");
}

/* ----- public: TX submit (called from post_send) ------------------ */

int usb4_rdma_data_send(u32 src_qp, u32 dest_qp, u32 psn, u8 flags,
			const void *payload, u32 length)
{
	struct u4_data_peer *p;
	struct u4_data_frame *f = NULL;
	int i, ret;

	if (length > U4_MAX_PAYLOAD)
		return -EMSGSIZE;

	read_lock(&the_peer_lock);
	p = the_peer;
	if (!p) {
		read_unlock(&the_peer_lock);
		return -ENOTCONN;
	}

	/* Find a free TX slot — atomically claim one whose in_use is 0.
	 * The previous version checked tx_inflight against the pool size
	 * and then always reused slot 0; under any concurrent / pipelined
	 * post_send that trampled the ring_frame of an in-flight frame.
	 *
	 * Linear scan with cmpxchg-style atomic claim is fine for our
	 * 96-slot pool; if every slot is busy we return -EBUSY and the
	 * caller (verbs post_send) returns the WR to userspace as bad. */
	for (i = 0; i < U4_DATA_FRAMES_PER_DIR; i++) {
		struct u4_data_frame *cand = &p->tx_frames[i];
		if (atomic_cmpxchg(&cand->in_use, 0, 1) == 0) {
			f = cand;
			break;
		}
	}
	if (!f) {
		read_unlock(&the_peer_lock);
		return -EBUSY;
	}

	u4_wire_hdr_init((struct u4_wire_hdr *)f->buf, U4_OP_SEND,
			 dest_qp, src_qp, psn, length, flags);
	if (length)
		memcpy((u8 *)f->buf + U4_HDR_SIZE, payload, length);

	f->frame.size = U4_HDR_SIZE + length;
	f->frame.callback = u4_data_tx_complete;
	f->frame.sof = U4_DATA_PDF_FRAME_START;
	f->frame.eof = U4_DATA_PDF_FRAME_END;

	atomic_inc(&p->tx_inflight);
	ret = tb_ring_tx(p->tx_ring, &f->frame);
	if (ret) {
		atomic_dec(&p->tx_inflight);
		atomic_set(&f->in_use, 0);
		read_unlock(&the_peer_lock);
		return ret;
	}
	read_unlock(&the_peer_lock);
	return 0;
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
	hash_add(u4_data_qp_table, &qe->node, qp_num);
	spin_unlock_irqrestore(&u4_data_qp_lock, flags);
	return 0;
}

void usb4_rdma_data_unregister_qp(u32 qp_num)
{
	struct u4_data_qp_entry *qe;
	struct hlist_node *tmp;
	struct hlist_head *head;
	unsigned long flags;

	head = &u4_data_qp_table[hash_min(qp_num, U4_DATA_QP_HASH_BITS)];
	spin_lock_irqsave(&u4_data_qp_lock, flags);
	hlist_for_each_entry_safe(qe, tmp, head, node) {
		if (qe->qp_num == qp_num) {
			hash_del(&qe->node);
			kfree(qe);
			break;
		}
	}
	spin_unlock_irqrestore(&u4_data_qp_lock, flags);
}

void usb4_rdma_data_set_rx_handler(void (*h)(void *qp,
					     const struct u4_wire_hdr *hdr,
					     const void *payload, u32 length))
{
	u4_data_rx_handler = h;
}

bool usb4_rdma_data_peer_attached(void)
{
	bool yes;
	read_lock(&the_peer_lock);
	yes = the_peer != NULL;
	read_unlock(&the_peer_lock);
	return yes;
}
