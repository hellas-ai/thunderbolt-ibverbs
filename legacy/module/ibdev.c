// SPDX-License-Identifier: GPL-2.0
/*
 * ibdev.c — soft-RDMA `ib_device` and verbs implementation.
 *
 * Verbs surface: ucontext, PD, MR, CQ, QP, post_send/post_recv,
 * poll_cq. Backing data path lives in data.c (rings + wire format).
 *
 *   userspace verbs → uverbs ioctl → our verbs callbacks
 *                                   → for SEND: copy from MR pages
 *                                                into wire frame,
 *                                                tb_ring_tx
 *                                   → for RECV: queue per-QP, then
 *                                                RX handler from
 *                                                data.c finds the
 *                                                pending WR, copies
 *                                                wire payload into
 *                                                MR pages, generates
 *                                                a CQE.
 *
 * Memory model:
 *   - reg_user_mr pins the user pages via pin_user_pages_fast and
 *     stores them in struct usb4_rdma_mr. The lkey/rkey we hand back
 *     is just an atomically-incrementing counter. Lookup at post_send
 *     / post_recv time walks the PD's MR list.
 *   - On x86_64 with VMSPLIT_NONE there's no high memory, so
 *     page_address(page) gives a kernel virtual address directly —
 *     we skip kmap_local_page().
 *
 * Concurrency:
 *   - CQ has an irq-safe spinlock guarding the WC list (RX softirq
 *     pushes; userspace poll_cq drains).
 *   - QP has a spinlock for the recv WR queue (post_recv pushes;
 *     RX softirq pops).
 *   - PD's MR list is protected by spin_lock_irqsave (post path
 *     accesses from process context, but mr lookup may also happen
 *     from RX path in future RDMA-write).
 *
 * No atomics or SRQ. RC SEND/RECV, RDMA-WRITE/WRITE_WITH_IMM, and
 * RDMA-READ are implemented over the copied USB4 ring transport.
 */

#define pr_fmt(fmt) "usb4_rdma/ibdev: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/random.h>
#include <linux/etherdevice.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/highmem.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/sched/mm.h>
#include <linux/refcount.h>
#include <linux/wait.h>
#include <linux/netdevice.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/hardirq.h>
#include <linux/dma-mapping.h>
#include <linux/idr.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <net/addrconf.h>
#include <net/net_namespace.h>

#include <rdma/ib_verbs.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/uverbs_ioctl.h>
#include <rdma/ib_mad.h>

#include "usb4_rdma.h"
#include "wire.h"

#define USB4_RDMA_NPORTS       1
#define USB4_RDMA_UVERBS_ABI   1
#define USB4_RDMA_PAGE_SIZE_CAP \
	(SZ_4K | SZ_2M | SZ_1G)
#define U4_MAX_SGE             4
#define U4_IB_GRH_NEXT_HDR     0x1b
#define U4_MAX_READ_CTX        128
#define U4_SEND_CREDIT_TIMEOUT_MS 5000
#define U4R_MR_DMA_CACHE_SLOTS 4
#define U4_DEFERRED_ACK_RETRY_DELAY 1

enum u4r_rail_mode {
	U4R_RAIL_SINGLE = 0,
	U4R_RAIL_LANE,
};

enum {
	U4_RECV_CREDIT_NONE = 0,
	U4_RECV_CREDIT_PROBE_USED = 1,
	U4_RECV_CREDIT_SEEN = 2,
};

/* Which netdev to bind for the RDMA-CM control plane. The data path
 * always rides usb4_rdma's tb_ring; this just decides which
 * IP-routable interface RDMA-CM uses for handshake / GID resolution.
 *
 * Default `thunderbolt0` works as long as `thunderbolt_net` is loaded.
 * For exclusive-mode RDMA (where you `rmmod thunderbolt_net` to free
 * the second hop per controller and double the verbs bandwidth), pass
 * e.g. `cm_netdev=br0.lan` so CM can fall back to the LAN. Bootstrap
 * traffic is small (KBs per connection); the bandwidth and latency
 * the project cares about live entirely on the data path.
 */
static char *cm_netdev = "thunderbolt0";
module_param(cm_netdev, charp, 0444);
MODULE_PARM_DESC(cm_netdev,
		 "netdev for RDMA-CM control plane (default: thunderbolt0; "
		 "use e.g. br0.lan/eno1 when running without thunderbolt_net)");

/*
 * The current TX zero-copy path submits a metadata frame followed by one or
 * more raw payload frames. That saves a memcpy for large transfers, but it is
 * strictly worse for latency-sized WRs because it adds an extra tb_ring
 * descriptor and per-page DMA map/free work. Keep small messages on the
 * single-frame staging path and reserve zero-copy for bandwidth-sized payloads.
 */
static uint zcopy_min_bytes = SZ_16K;
module_param(zcopy_min_bytes, uint, 0644);
MODULE_PARM_DESC(zcopy_min_bytes,
		 "minimum non-inline payload size to use TX zero-copy; smaller WRs use staging frames (default: 16384, 0 = always)");

static bool send_ack_debug;
module_param(send_ack_debug, bool, 0644);
MODULE_PARM_DESC(send_ack_debug,
		 "log RC SEND credit ACK state transitions for debugging");

static char *rail_mode = "single";
module_param(rail_mode, charp, 0444);
MODULE_PARM_DESC(rail_mode,
		 "verbs rail exposure: single = one usb4_rdma device over all lanes; lane = one ib_device per active NHI lane");
#define U4_QPN_MIN             2
#define U4_QPN_MAX             0x00ffffff
#define U4_MAX_MSG_SIZE        SZ_1G

/* ----- types ------------------------------------------------------- */

struct usb4_rdma_ucontext {
	struct ib_ucontext base;
};

struct u4r_mr_dma_cache_entry {
	struct device *dev;
	enum dma_data_direction dir;
	dma_addr_t *addrs;
};

struct usb4_rdma_mr {
	struct ib_mr base;
	struct list_head pd_link;
	refcount_t refs;
	wait_queue_head_t ref_wait;
	bool dying;
	u64 user_va;
	u64 length;
	int access_flags;
	int npages;
	struct page **pages;
	struct mutex dma_lock;
	struct u4r_mr_dma_cache_entry dma_cache[U4R_MR_DMA_CACHE_SLOTS];
};

struct usb4_rdma_pd {
	struct ib_pd base;
	struct list_head mrs;
	spinlock_t mr_lock;
};

struct usb4_rdma_ah {
	struct ib_ah base;
	struct rdma_ah_attr attr;
};

struct usb4_rdma_wc_entry {
	struct list_head list;
	struct ib_wc wc;
};

struct usb4_rdma_cq {
	struct ib_cq base;
	int cqe_capacity;
	spinlock_t lock;
	struct list_head wc_list;	/* of usb4_rdma_wc_entry */
	struct list_head free_list;	/* preallocated usb4_rdma_wc_entry pool */
	struct usb4_rdma_wc_entry *wc_pool;
	int wc_count;
	int free_count;
	enum ib_cq_notify_flags notify;
};

struct usb4_rdma_recv_wr {
	struct list_head list;
	u64 wr_id;
	struct ib_cqe *wr_cqe;
	int num_sge;
	struct ib_sge sge[U4_MAX_SGE];
};

struct usb4_rdma_read_ctx {
	struct list_head list;
	u64 wr_id;
	struct ib_cqe *wr_cqe;
	u32 psn;
	bool signaled;
	int num_sge;
	struct ib_sge sge[U4_MAX_SGE];
	u32 total_len;
	u32 done;
	bool failed;
	bool seen_last;
	enum ib_wc_status status;
};

struct usb4_rdma_send_ctx {
	struct list_head list;
	u64 wr_id;
	struct ib_cqe *wr_cqe;
	u32 ack_psn;
	enum ib_wc_opcode opcode;
};

/* UC SEND deferral.
 *
 * u4r_send_one blocks waiting for peer-recv credit. JACCL (and any
 * pipelined caller) calls ibv_post_send from inside its poll-completion
 * loop; if post_send blocks there, the caller can't process recv
 * completions or post new recv WRs, so the peer's credit pool never
 * refills and both ends deadlock. Standard IB semantics make post_send
 * non-blocking. We restore that for UC by capturing the WR here and
 * deferring the actual transmission to a per-QP workqueue. */
struct u4r_deferred_send {
	struct list_head list;
	enum ib_wr_opcode opcode;
	int send_flags;
	u64 wr_id;
	struct ib_cqe *wr_cqe;
	int num_sge;
	struct ib_sge sges[U4_MAX_SGE];
	u64 remote_addr;	/* IB_WR_RDMA_* only */
	u32 rkey;		/* IB_WR_RDMA_* only */
	__be32 imm_data;	/* IB_WR_RDMA_WRITE_WITH_IMM only */
};

struct u4r_deferred_ack {
	struct list_head list;
	u32 dest_qp;
	u32 psn;
	__be32 status;
};

struct usb4_rdma_qp {
	struct ib_qp base;
	struct u4_data_peer *rail;
	enum ib_qp_type qp_type;
	enum ib_qp_state state;
	struct ib_qp_attr attr;
	int attr_mask;
	refcount_t refs;
	wait_queue_head_t ref_wait;
	bool qpn_allocated;
	bool registered;
	bool sq_sig_all;
	struct mutex send_lock;
	u32 send_psn;
	spinlock_t send_comp_lock;
	struct list_head send_ctxs;	/* of usb4_rdma_send_ctx */
	atomic_t remote_recv_credits;
	atomic_t remote_recv_credit_state;
	wait_queue_head_t send_credit_wait;
	u32 recv_psn;
	spinlock_t recv_lock;
	struct list_head recv_q;	/* of usb4_rdma_recv_wr */
	u32 recv_posted_count;
	u32 recv_credits_advertised;
	spinlock_t read_lock;
	struct list_head read_ctxs;	/* of usb4_rdma_read_ctx */
	u32 read_depth;

	/* Multi-frame RX reassembly. RC delivers in order; we hold the
	 * head WR popped at first-fragment time and accumulate payload
	 * across frames until U4_F_LAST. recv_byte_offset is the running
	 * scatter offset across the WR's SGE list. recv_truncated is
	 * sticky for the message duration if the WR is too small. */
	struct usb4_rdma_recv_wr *in_progress_recv;
	u32 recv_byte_offset;
	bool recv_truncated;

	/* UC SEND deferral queue + worker (see struct u4r_deferred_send). */
	struct list_head deferred_sends;
	spinlock_t deferred_send_lock;
	struct work_struct deferred_send_work;

	/* RC SEND ACKs generated from irq_work must not disappear when the
	 * immediate atomic TX path is temporarily full. */
	struct list_head deferred_acks;
	spinlock_t deferred_ack_lock;
	struct delayed_work deferred_ack_work;
	bool deferred_ack_active;
	bool deferred_ack_stopping;
};

struct usb4_rdma_ib_dev {
	struct ib_device base;
	struct list_head dev_link;
	struct u4_data_peer *rail;
	atomic_t active_peers;
	struct net_device *netdev;	/* RoCEv2 IP→GID stub; see netdev.c */
};

static struct usb4_rdma_ib_dev *u4r_dev;
static LIST_HEAD(u4r_dev_list);
static DEFINE_MUTEX(u4r_dev_list_lock);
static enum u4r_rail_mode u4r_mode = U4R_RAIL_SINGLE;
static atomic_t u4r_lkey_counter = ATOMIC_INIT(1);
static DEFINE_IDA(u4r_qpn_ida);

static int u4r_parse_rail_mode(void)
{
	if (sysfs_streq(rail_mode, "single")) {
		u4r_mode = U4R_RAIL_SINGLE;
		return 0;
	}
	if (sysfs_streq(rail_mode, "lane")) {
		u4r_mode = U4R_RAIL_LANE;
		return 0;
	}
	pr_err("invalid rail_mode=%s (expected single or lane)\n", rail_mode);
	return -EINVAL;
}

static bool u4r_lane_mode(void)
{
	return u4r_mode == U4R_RAIL_LANE;
}

static bool u4r_warn_process_path_from_atomic(const char *where)
{
	if (!WARN_ON_ONCE(in_interrupt()))
		return false;

	pr_warn_ratelimited("%s called from interrupt context; this path may sleep\n",
			    where);
	return true;
}

/* ----- helpers: PD ↔ MR lookup ------------------------------------ */

static struct usb4_rdma_mr *
u4r_pd_get_mr_by_lkey(struct usb4_rdma_pd *pd, u32 lkey)
{
	struct usb4_rdma_mr *mr;
	unsigned long flags;

	spin_lock_irqsave(&pd->mr_lock, flags);
	list_for_each_entry(mr, &pd->mrs, pd_link) {
		if (mr->base.lkey == lkey && !mr->dying) {
			refcount_inc(&mr->refs);
			spin_unlock_irqrestore(&pd->mr_lock, flags);
			return mr;
		}
	}
	spin_unlock_irqrestore(&pd->mr_lock, flags);
	return NULL;
}

static struct usb4_rdma_mr *
u4r_pd_get_mr_by_rkey(struct usb4_rdma_pd *pd, u32 rkey)
{
	struct usb4_rdma_mr *mr;
	unsigned long flags;

	spin_lock_irqsave(&pd->mr_lock, flags);
	list_for_each_entry(mr, &pd->mrs, pd_link) {
		if (mr->base.rkey == rkey && !mr->dying) {
			refcount_inc(&mr->refs);
			spin_unlock_irqrestore(&pd->mr_lock, flags);
			return mr;
		}
	}
	spin_unlock_irqrestore(&pd->mr_lock, flags);
	return NULL;
}

static void u4r_mr_put(struct usb4_rdma_mr *mr)
{
	if (refcount_dec_and_test(&mr->refs))
		WARN_ON_ONCE(1);
	if (refcount_read(&mr->refs) == 1)
		wake_up(&mr->ref_wait);
}

static void u4r_mr_unmap_dma_entry(struct usb4_rdma_mr *mr,
				   struct u4r_mr_dma_cache_entry *entry)
{
	int i;

	if (!entry->dev)
		return;

	for (i = 0; i < mr->npages; i++)
		dma_unmap_page(entry->dev, entry->addrs[i], PAGE_SIZE,
			       entry->dir);
	kvfree(entry->addrs);
	put_device(entry->dev);
	entry->dev = NULL;
	entry->addrs = NULL;
}

static void u4r_mr_unmap_dma_locked(struct usb4_rdma_mr *mr)
{
	int i;

	for (i = 0; i < U4R_MR_DMA_CACHE_SLOTS; i++)
		u4r_mr_unmap_dma_entry(mr, &mr->dma_cache[i]);
}

static void u4r_mr_unmap_dma(struct usb4_rdma_mr *mr)
{
	mutex_lock(&mr->dma_lock);
	u4r_mr_unmap_dma_locked(mr);
	mutex_unlock(&mr->dma_lock);
}

static int u4r_mr_resolve_dma(void *ctx, struct device *dma_dev,
			      u32 page_idx, u32 page_off, u32 length,
			      enum dma_data_direction dir,
			      dma_addr_t *dma_addr)
{
	struct usb4_rdma_mr *mr = ctx;
	struct u4r_mr_dma_cache_entry *free_entry = NULL;
	struct u4r_mr_dma_cache_entry *entry;
	dma_addr_t *addrs;
	int i;

	if (u4r_warn_process_path_from_atomic(__func__))
		return -EWOULDBLOCK;
	if (!dma_dev || !dma_addr || page_idx >= mr->npages ||
	    page_off >= PAGE_SIZE || length > PAGE_SIZE - page_off)
		return -EINVAL;

	mutex_lock(&mr->dma_lock);
	for (i = 0; i < U4R_MR_DMA_CACHE_SLOTS; i++) {
		entry = &mr->dma_cache[i];
		if (entry->dev == dma_dev && entry->dir == dir) {
			*dma_addr = entry->addrs[page_idx] + page_off;
			mutex_unlock(&mr->dma_lock);
			return 0;
		}
		if (!entry->dev && !free_entry)
			free_entry = entry;
	}
	if (!free_entry) {
		mutex_unlock(&mr->dma_lock);
		return -EAGAIN;
	}

	addrs = kvcalloc(mr->npages, sizeof(*addrs), GFP_KERNEL);
	if (!addrs)
		goto fallback;

	for (i = 0; i < mr->npages; i++) {
		addrs[i] = dma_map_page(dma_dev, mr->pages[i], 0, PAGE_SIZE,
					dir);
		if (dma_mapping_error(dma_dev, addrs[i])) {
			while (--i >= 0)
				dma_unmap_page(dma_dev, addrs[i], PAGE_SIZE,
					       dir);
			kvfree(addrs);
			goto fallback;
		}
	}

	free_entry->dev = get_device(dma_dev);
	free_entry->dir = dir;
	free_entry->addrs = addrs;
	*dma_addr = addrs[page_idx] + page_off;
	mutex_unlock(&mr->dma_lock);
	return 0;

fallback:
	mutex_unlock(&mr->dma_lock);
	return -EAGAIN;
}

static void u4r_qp_get(struct usb4_rdma_qp *qp)
{
	refcount_inc(&qp->refs);
}

static void u4r_qp_put(struct usb4_rdma_qp *qp)
{
	if (refcount_dec_and_test(&qp->refs))
		WARN_ON_ONCE(1);
	if (refcount_read(&qp->refs) == 1)
		wake_up(&qp->ref_wait);
}

static int u4r_mr_check_range(struct usb4_rdma_mr *mr, u64 vaddr, size_t len)
{
	if (vaddr < mr->user_va || len > mr->length ||
	    vaddr - mr->user_va > mr->length - len)
		return -ERANGE;
	return 0;
}

/* Copy `len` bytes between `kbuf` and the user-pinned MR pages,
 * starting at user virtual address `vaddr`. Returns 0 on success. */
static int u4r_mr_xfer(struct usb4_rdma_mr *mr, u64 vaddr, void *kbuf,
		       size_t len, bool from_mr_to_kbuf)
{
	u64 offset, page_idx, page_off;
	size_t copied = 0;
	int ret;

	ret = u4r_mr_check_range(mr, vaddr, len);
	if (ret)
		return ret;
	offset = (mr->user_va & ~PAGE_MASK) + (vaddr - mr->user_va);

	while (copied < len) {
		size_t chunk;
		void *page_kva;

		page_idx = (offset + copied) >> PAGE_SHIFT;
		page_off = (offset + copied) & ~PAGE_MASK;
		if (page_idx >= mr->npages)
			return -ERANGE;
		chunk = min_t(size_t, PAGE_SIZE - page_off, len - copied);
		page_kva = page_address(mr->pages[page_idx]);
		if (!page_kva)
			return -EFAULT;
		if (from_mr_to_kbuf)
			memcpy((u8 *)kbuf + copied, (u8 *)page_kva + page_off,
			       chunk);
		else
			memcpy((u8 *)page_kva + page_off, (u8 *)kbuf + copied,
			       chunk);
		copied += chunk;
	}
	return 0;
}

static int u4r_mr_page_chunk(struct usb4_rdma_mr *mr, u64 vaddr, u32 remaining,
			     struct page **page, u32 *page_idx_out,
			     u32 *page_off, u32 *chunk)
{
	u64 offset, page_idx;
	int ret;

	if (!remaining)
		return -EINVAL;
	ret = u4r_mr_check_range(mr, vaddr, remaining);
	if (ret)
		return ret;

	offset = (mr->user_va & ~PAGE_MASK) + (vaddr - mr->user_va);
	page_idx = offset >> PAGE_SHIFT;
	if (page_idx >= mr->npages)
		return -ERANGE;

	*page = mr->pages[page_idx];
	*page_idx_out = page_idx;
	*page_off = offset & ~PAGE_MASK;
	*chunk = min_t(u32, remaining, PAGE_SIZE - *page_off);
	*chunk = min_t(u32, *chunk, U4_FRAME_SIZE);
	return 0;
}

static void u4r_mr_put_done(void *ctx)
{
	u4r_mr_put(ctx);
}

static struct usb4_rdma_read_ctx *
u4r_find_read_ctx_locked(struct usb4_rdma_qp *qp, u32 psn)
{
	struct usb4_rdma_read_ctx *ctx;

	list_for_each_entry(ctx, &qp->read_ctxs, list) {
		if (ctx->psn == psn)
			return ctx;
	}
	return NULL;
}

static struct usb4_rdma_send_ctx *
u4r_find_send_ctx_locked(struct usb4_rdma_qp *qp, u32 psn)
{
	struct usb4_rdma_send_ctx *ctx;

	list_for_each_entry(ctx, &qp->send_ctxs, list) {
		if (ctx->ack_psn == psn)
			return ctx;
	}
	return NULL;
}

static bool u4r_qp_can_advertise_recv_credits(const struct usb4_rdma_qp *qp)
{
	/* Soft credit flow control was originally RC-only. We extend it to
	 * UC because Apple's JACCL was designed assuming the wire enforces
	 * "sender stalls until peer has matching recv WR posted" (TN3205
	 * docs Apple's hardware behavior); JACCL races and silently drops
	 * frames on strict-textbook UC. Re-using our RC credit machinery
	 * for UC fixes the JACCL bench hang without needing wire changes
	 * — both ends still need to be running this driver to benefit. */
	if ((qp->qp_type != IB_QPT_RC && qp->qp_type != IB_QPT_UC) ||
	    !qp->registered ||
	    !qp->attr.dest_qp_num)
		return false;

	return qp->state == IB_QPS_RTR || qp->state == IB_QPS_RTS ||
	       qp->state == IB_QPS_SQD || qp->state == IB_QPS_SQE;
}

static struct usb4_rdma_recv_wr *
u4r_pop_recv_wr_locked(struct usb4_rdma_qp *qp)
{
	struct usb4_rdma_recv_wr *r;

	r = list_first_entry_or_null(&qp->recv_q, struct usb4_rdma_recv_wr,
				     list);
	if (!r)
		return NULL;

	list_del(&r->list);
	if (qp->qp_type == IB_QPT_RC || qp->qp_type == IB_QPT_UC) {
		if (qp->recv_posted_count)
			qp->recv_posted_count--;
		if (qp->recv_credits_advertised)
			qp->recv_credits_advertised--;
	}
	return r;
}

static void u4r_deferred_send_work_fn(struct work_struct *w);
static void u4r_deferred_ack_work_fn(struct work_struct *w);

static void u4r_advertise_recv_credits(struct usb4_rdma_qp *qp)
{
	unsigned long flags;
	u32 credits = 0;
	u32 dest_qp = 0;
	int ret;

	spin_lock_irqsave(&qp->recv_lock, flags);
	if (u4r_qp_can_advertise_recv_credits(qp) &&
	    qp->recv_posted_count > qp->recv_credits_advertised) {
		credits = qp->recv_posted_count -
			  qp->recv_credits_advertised;
		qp->recv_credits_advertised += credits;
		dest_qp = qp->attr.dest_qp_num;
	}
	spin_unlock_irqrestore(&qp->recv_lock, flags);

	if (!credits)
		return;

	ret = usb4_rdma_data_send(qp->rail, U4_OP_RECV_CREDIT, qp->base.qp_num,
				  dest_qp, 0, U4_F_LAST,
				  cpu_to_be32(credits), 0, 0,
				  NULL, NULL, 0);
	if (ret) {
		spin_lock_irqsave(&qp->recv_lock, flags);
		if (qp->recv_credits_advertised >= credits)
			qp->recv_credits_advertised -= credits;
		else
			qp->recv_credits_advertised = 0;
		spin_unlock_irqrestore(&qp->recv_lock, flags);
	}
	if (READ_ONCE(send_ack_debug))
		pr_info("recv_credit tx: qp=%u dest=%u credits=%u ret=%d\n",
			qp->base.qp_num, dest_qp, credits, ret);
}

static int u4r_reserve_remote_recv_credit(struct usb4_rdma_qp *qp)
{
	long wait;

	for (;;) {
		int credits = atomic_read(&qp->remote_recv_credits);

		while (credits > 0) {
			if (atomic_cmpxchg(&qp->remote_recv_credits, credits,
					   credits - 1) == credits) {
				atomic_set(&qp->remote_recv_credit_state,
					   U4_RECV_CREDIT_SEEN);
				return 1;
			}
			credits = atomic_read(&qp->remote_recv_credits);
		}

		/* RC tolerates losing the bootstrap probe because RC ACK/retry
		 * recovers it. UC has no retry: if we send before the peer has
		 * a recv WR posted (and thus advertised a credit to us), the
		 * peer's wire-level RX path silently drops the frame and the
		 * application hangs. So skip the probe path for UC and wait
		 * unconditionally for the peer's first credit advertisement. */
		if (qp->qp_type != IB_QPT_UC &&
		    atomic_cmpxchg(&qp->remote_recv_credit_state,
				   U4_RECV_CREDIT_NONE,
				   U4_RECV_CREDIT_PROBE_USED) ==
		    U4_RECV_CREDIT_NONE) {
			if (READ_ONCE(send_ack_debug))
				pr_info("recv_credit bootstrap probe: qp=%u dest=%u\n",
					qp->base.qp_num,
					qp->attr.dest_qp_num);
			return 0;
		}

		wait = wait_event_interruptible_timeout(
			qp->send_credit_wait,
			atomic_read(&qp->remote_recv_credits) > 0 ||
			(qp->state != IB_QPS_RTS && qp->state != IB_QPS_SQD &&
			 qp->state != IB_QPS_SQE),
			msecs_to_jiffies(U4_SEND_CREDIT_TIMEOUT_MS));
		if (wait < 0)
			return wait;
		if (!wait) {
			if (READ_ONCE(send_ack_debug))
				pr_info("recv_credit wait timeout: qp=%u dest=%u\n",
					qp->base.qp_num,
					qp->attr.dest_qp_num);
			return -ETIMEDOUT;
		}
		if (qp->state != IB_QPS_RTS && qp->state != IB_QPS_SQD &&
		    qp->state != IB_QPS_SQE)
			return -EINVAL;
	}
}

static void u4r_refund_remote_recv_credit(struct usb4_rdma_qp *qp)
{
	atomic_inc(&qp->remote_recv_credits);
	atomic_set(&qp->remote_recv_credit_state, U4_RECV_CREDIT_SEEN);
	wake_up(&qp->send_credit_wait);
}

/* ----- helpers: CQ enqueue ---------------------------------------- */

static int u4r_cq_push_wc(struct usb4_rdma_cq *cq, const struct ib_wc *wc)
{
	struct usb4_rdma_wc_entry *e;
	unsigned long flags;

	spin_lock_irqsave(&cq->lock, flags);
	if (list_empty(&cq->free_list)) {
		spin_unlock_irqrestore(&cq->lock, flags);
		return -ENOMEM;
	}
	e = list_first_entry(&cq->free_list, struct usb4_rdma_wc_entry, list);
	list_del(&e->list);
	cq->free_count--;
	e->wc = *wc;
	list_add_tail(&e->list, &cq->wc_list);
	cq->wc_count++;
	if (cq->notify) {
		enum ib_cq_notify_flags n = cq->notify;
		cq->notify = 0;
		spin_unlock_irqrestore(&cq->lock, flags);
		if (cq->base.comp_handler)
			cq->base.comp_handler(&cq->base, cq->base.cq_context);
		(void)n;
	} else {
		spin_unlock_irqrestore(&cq->lock, flags);
	}
	return 0;
}

static void u4r_flush_qp(struct usb4_rdma_qp *qp)
{
	struct usb4_rdma_cq *recv_cq =
		container_of(qp->base.recv_cq, struct usb4_rdma_cq, base);
	struct usb4_rdma_cq *send_cq =
		container_of(qp->base.send_cq, struct usb4_rdma_cq, base);
	LIST_HEAD(recv_flush);
	LIST_HEAD(read_flush);
	LIST_HEAD(send_flush);
	struct usb4_rdma_recv_wr *r, *rtmp, *in_progress;
	struct usb4_rdma_read_ctx *read_ctx, *read_tmp;
	struct usb4_rdma_send_ctx *send_ctx, *send_tmp;
	unsigned long flags;

	spin_lock_irqsave(&qp->send_comp_lock, flags);
	list_splice_init(&qp->send_ctxs, &send_flush);
	spin_unlock_irqrestore(&qp->send_comp_lock, flags);

	list_for_each_entry_safe(send_ctx, send_tmp, &send_flush, list) {
		struct ib_wc wc = {};

		list_del(&send_ctx->list);
		wc.wr_id = send_ctx->wr_id;
		wc.wr_cqe = send_ctx->wr_cqe;
		wc.status = IB_WC_WR_FLUSH_ERR;
		wc.opcode = send_ctx->opcode;
		wc.qp = &qp->base;
		u4r_cq_push_wc(send_cq, &wc);
		kfree(send_ctx);
	}

	spin_lock_irqsave(&qp->recv_lock, flags);
	list_splice_init(&qp->recv_q, &recv_flush);
	in_progress = qp->in_progress_recv;
	qp->in_progress_recv = NULL;
	qp->recv_byte_offset = 0;
	qp->recv_truncated = false;
	qp->recv_posted_count = 0;
	qp->recv_credits_advertised = 0;
	spin_unlock_irqrestore(&qp->recv_lock, flags);
	atomic_set(&qp->remote_recv_credits, 0);
	atomic_set(&qp->remote_recv_credit_state, U4_RECV_CREDIT_NONE);
	wake_up(&qp->send_credit_wait);

	list_for_each_entry_safe(r, rtmp, &recv_flush, list) {
		struct ib_wc wc = {};

		list_del(&r->list);
		wc.wr_id = r->wr_id;
		wc.wr_cqe = r->wr_cqe;
		wc.status = IB_WC_WR_FLUSH_ERR;
		wc.opcode = IB_WC_RECV;
		wc.qp = &qp->base;
		u4r_cq_push_wc(recv_cq, &wc);
		kfree(r);
	}
	if (in_progress) {
		struct ib_wc wc = {};

		wc.wr_id = in_progress->wr_id;
		wc.wr_cqe = in_progress->wr_cqe;
		wc.status = IB_WC_WR_FLUSH_ERR;
		wc.opcode = IB_WC_RECV;
		wc.qp = &qp->base;
		u4r_cq_push_wc(recv_cq, &wc);
		kfree(in_progress);
	}

	spin_lock_irqsave(&qp->read_lock, flags);
	list_splice_init(&qp->read_ctxs, &read_flush);
	qp->read_depth = 0;
	spin_unlock_irqrestore(&qp->read_lock, flags);

	list_for_each_entry_safe(read_ctx, read_tmp, &read_flush, list) {
		struct ib_wc wc = {};

		list_del(&read_ctx->list);
		wc.wr_id = read_ctx->wr_id;
		wc.wr_cqe = read_ctx->wr_cqe;
		wc.status = IB_WC_WR_FLUSH_ERR;
		wc.opcode = IB_WC_RDMA_READ;
		wc.qp = &qp->base;
		u4r_cq_push_wc(send_cq, &wc);
		kfree(read_ctx);
	}
}

/* ----- ucontext, PD, MR ------------------------------------------- */

static int u4r_alloc_ucontext(struct ib_ucontext *ibuc, struct ib_udata *udata)
{
	return 0;
}
static void u4r_dealloc_ucontext(struct ib_ucontext *ibuc) {}

static int u4r_alloc_pd(struct ib_pd *ibpd, struct ib_udata *udata)
{
	struct usb4_rdma_pd *pd =
		container_of(ibpd, struct usb4_rdma_pd, base);
	INIT_LIST_HEAD(&pd->mrs);
	spin_lock_init(&pd->mr_lock);
	/* No pr_info here — would fire on every userspace ibv_alloc_pd
	 * call, which can happen many times per benchmark iteration. */
	return 0;
}

static int u4r_dealloc_pd(struct ib_pd *ibpd, struct ib_udata *udata)
{
	return 0;
}

static struct ib_mr *u4r_reg_user_mr(struct ib_pd *ibpd, u64 start, u64 length,
				     u64 virt_addr, int access_flags,
				     struct ib_dmah *dmah,
				     struct ib_udata *udata)
{
	struct usb4_rdma_pd *pd =
		container_of(ibpd, struct usb4_rdma_pd, base);
	struct usb4_rdma_mr *mr;
	unsigned long flags;
	long got;
	u64 page_off, va_aligned;
	int npages;
	u32 lkey;
	int err;

	page_off    = start & ~PAGE_MASK;
	va_aligned  = start & PAGE_MASK;
	npages = (page_off + length + PAGE_SIZE - 1) >> PAGE_SHIFT;
	if (npages <= 0 || npages > 1024 * 1024)
		return ERR_PTR(-EINVAL);

	mr = kzalloc(sizeof(*mr), GFP_KERNEL);
	if (!mr)
		return ERR_PTR(-ENOMEM);

	mr->user_va     = start;
	mr->length      = length;
	mr->access_flags = access_flags;
	mr->npages      = npages;
	refcount_set(&mr->refs, 1);
	init_waitqueue_head(&mr->ref_wait);
	mutex_init(&mr->dma_lock);
	mr->pages = kvcalloc(npages, sizeof(*mr->pages), GFP_KERNEL);
	if (!mr->pages) {
		err = -ENOMEM;
		goto err_free_mr;
	}

	got = pin_user_pages_fast(va_aligned, npages,
				  FOLL_WRITE | FOLL_LONGTERM, mr->pages);
	if (got < 0) { err = got; goto err_free_pages; }
	if (got < npages) {
		unpin_user_pages(mr->pages, got);
		err = -EFAULT;
		goto err_free_pages;
	}

	lkey = atomic_inc_return(&u4r_lkey_counter);
	mr->base.lkey = lkey;
	mr->base.rkey = lkey;
	mr->base.length = length;
	mr->base.iova   = virt_addr;
	mr->base.pd     = ibpd;
	mr->base.device = ibpd->device;

	spin_lock_irqsave(&pd->mr_lock, flags);
	list_add(&mr->pd_link, &pd->mrs);
	spin_unlock_irqrestore(&pd->mr_lock, flags);

	pr_info("reg_user_mr ok: va=0x%llx len=%llu npages=%d lkey=0x%x\n",
		(u64)start, length, npages, lkey);
	return &mr->base;

err_free_pages:
	kvfree(mr->pages);
err_free_mr:
	kfree(mr);
	return ERR_PTR(err);
}

static int u4r_dereg_mr(struct ib_mr *ibmr, struct ib_udata *udata)
{
	struct usb4_rdma_mr *mr = container_of(ibmr, struct usb4_rdma_mr, base);
	struct usb4_rdma_pd *pd =
		container_of(ibmr->pd, struct usb4_rdma_pd, base);
	unsigned long flags;

	spin_lock_irqsave(&pd->mr_lock, flags);
	mr->dying = true;
	list_del(&mr->pd_link);
	spin_unlock_irqrestore(&pd->mr_lock, flags);

	wait_event(mr->ref_wait, refcount_read(&mr->refs) == 1);

	u4r_mr_unmap_dma(mr);
	if (mr->pages) {
		unpin_user_pages(mr->pages, mr->npages);
		kvfree(mr->pages);
	}
	kfree(mr);
	return 0;
}

static struct ib_mr *u4r_get_dma_mr(struct ib_pd *ibpd, int access_flags)
{
	return ERR_PTR(-EOPNOTSUPP);
}

/* ----- AH --------------------------------------------------------- */

static int u4r_create_ah(struct ib_ah *ibah,
			 struct rdma_ah_init_attr *init_attr,
			 struct ib_udata *udata)
{
	struct usb4_rdma_ah *ah = container_of(ibah, struct usb4_rdma_ah, base);

	if (!init_attr || !init_attr->ah_attr)
		return -EINVAL;

	rdma_copy_ah_attr(&ah->attr, init_attr->ah_attr);
	return 0;
}

static int u4r_query_ah(struct ib_ah *ibah, struct rdma_ah_attr *attr)
{
	struct usb4_rdma_ah *ah = container_of(ibah, struct usb4_rdma_ah, base);

	rdma_copy_ah_attr(attr, &ah->attr);
	return 0;
}

static int u4r_destroy_ah(struct ib_ah *ibah, u32 flags)
{
	struct usb4_rdma_ah *ah = container_of(ibah, struct usb4_rdma_ah, base);

	rdma_destroy_ah_attr(&ah->attr);
	return 0;
}

/* ----- CQ --------------------------------------------------------- */

static int u4r_create_cq(struct ib_cq *ibcq, const struct ib_cq_init_attr *attr,
			 struct uverbs_attr_bundle *attrs)
{
	struct usb4_rdma_cq *cq = container_of(ibcq, struct usb4_rdma_cq, base);
	int i;

	if (attr->cqe <= 0)
		return -EINVAL;
	cq->cqe_capacity = attr->cqe;
	spin_lock_init(&cq->lock);
	INIT_LIST_HEAD(&cq->wc_list);
	INIT_LIST_HEAD(&cq->free_list);
	cq->wc_pool = kvcalloc(attr->cqe, sizeof(*cq->wc_pool), GFP_KERNEL);
	if (!cq->wc_pool)
		return -ENOMEM;
	for (i = 0; i < attr->cqe; i++)
		list_add_tail(&cq->wc_pool[i].list, &cq->free_list);
	cq->wc_count = 0;
	cq->free_count = attr->cqe;
	cq->notify = 0;
	pr_info("create_cq ok (cqe=%u, comp_vector=%u)\n",
		attr->cqe, attr->comp_vector);
	return 0;
}

static int u4r_destroy_cq(struct ib_cq *ibcq, struct ib_udata *udata)
{
	struct usb4_rdma_cq *cq = container_of(ibcq, struct usb4_rdma_cq, base);
	struct usb4_rdma_wc_entry *e, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&cq->lock, flags);
	list_for_each_entry_safe(e, tmp, &cq->wc_list, list)
		list_del(&e->list);
	list_for_each_entry_safe(e, tmp, &cq->free_list, list)
		list_del(&e->list);
	cq->wc_count = 0;
	cq->free_count = 0;
	spin_unlock_irqrestore(&cq->lock, flags);
	kvfree(cq->wc_pool);
	cq->wc_pool = NULL;
	return 0;
}

static int u4r_poll_cq(struct ib_cq *ibcq, int num_entries, struct ib_wc *wc)
{
	struct usb4_rdma_cq *cq = container_of(ibcq, struct usb4_rdma_cq, base);
	struct usb4_rdma_wc_entry *e, *tmp;
	int n = 0;
	unsigned long flags;

	usb4_rdma_data_poll_rx();

	spin_lock_irqsave(&cq->lock, flags);
	list_for_each_entry_safe(e, tmp, &cq->wc_list, list) {
		if (n >= num_entries)
			break;
		wc[n++] = e->wc;
		list_del(&e->list);
		cq->wc_count--;
		list_add_tail(&e->list, &cq->free_list);
		cq->free_count++;
	}
	spin_unlock_irqrestore(&cq->lock, flags);
	return n;
}

static int u4r_req_notify_cq(struct ib_cq *ibcq, enum ib_cq_notify_flags flags)
{
	struct usb4_rdma_cq *cq = container_of(ibcq, struct usb4_rdma_cq, base);
	unsigned long irqf;

	spin_lock_irqsave(&cq->lock, irqf);
	cq->notify = flags & IB_CQ_SOLICITED_MASK;
	if ((flags & IB_CQ_REPORT_MISSED_EVENTS) && cq->wc_count) {
		spin_unlock_irqrestore(&cq->lock, irqf);
		return 1;
	}
	spin_unlock_irqrestore(&cq->lock, irqf);
	return 0;
}

/* ----- QP --------------------------------------------------------- */

static int u4r_create_qp(struct ib_qp *ibqp, struct ib_qp_init_attr *attr,
			 struct ib_udata *udata)
{
	struct usb4_rdma_qp *qp = container_of(ibqp, struct usb4_rdma_qp, base);
	struct usb4_rdma_ib_dev *u4r =
		container_of(ibqp->device, struct usb4_rdma_ib_dev, base);
	int qpn, ret;

	if (attr->qp_type != IB_QPT_RC && attr->qp_type != IB_QPT_GSI &&
	    attr->qp_type != IB_QPT_UC)
		return -EOPNOTSUPP;

	qp->rail = u4r->rail;
	if (qp->rail && !usb4_rdma_data_rail_get(qp->rail))
		return -ENOTCONN;

	qp->qp_type = attr->qp_type;
	qp->state = attr->qp_type == IB_QPT_GSI ? IB_QPS_RTS : IB_QPS_RESET;
	refcount_set(&qp->refs, 1);
	init_waitqueue_head(&qp->ref_wait);
	qp->qpn_allocated = false;
	qp->registered = false;
	qp->sq_sig_all = attr->sq_sig_type == IB_SIGNAL_ALL_WR;
	mutex_init(&qp->send_lock);
	qp->send_psn = 0;
	spin_lock_init(&qp->send_comp_lock);
	INIT_LIST_HEAD(&qp->send_ctxs);
	atomic_set(&qp->remote_recv_credits, 0);
	atomic_set(&qp->remote_recv_credit_state, U4_RECV_CREDIT_NONE);
	init_waitqueue_head(&qp->send_credit_wait);
	qp->recv_psn = 0;
	spin_lock_init(&qp->recv_lock);
	INIT_LIST_HEAD(&qp->recv_q);
	qp->recv_posted_count = 0;
	qp->recv_credits_advertised = 0;
	spin_lock_init(&qp->read_lock);
	INIT_LIST_HEAD(&qp->read_ctxs);
	qp->read_depth = 0;
	spin_lock_init(&qp->deferred_send_lock);
	INIT_LIST_HEAD(&qp->deferred_sends);
	INIT_WORK(&qp->deferred_send_work, u4r_deferred_send_work_fn);
	spin_lock_init(&qp->deferred_ack_lock);
	INIT_LIST_HEAD(&qp->deferred_acks);
	INIT_DELAYED_WORK(&qp->deferred_ack_work, u4r_deferred_ack_work_fn);
	qp->deferred_ack_active = false;
	qp->deferred_ack_stopping = false;
	if (attr->qp_type == IB_QPT_GSI) {
		ibqp->qp_num = 1;
		ret = usb4_rdma_data_register_qp(ibqp->qp_num, qp, qp->rail);
		if (ret) {
			usb4_rdma_data_rail_put(qp->rail);
			qp->rail = NULL;
			return ret;
		}
		qp->registered = true;
	} else {
		qpn = ida_alloc_range(&u4r_qpn_ida, U4_QPN_MIN, U4_QPN_MAX,
				      GFP_KERNEL);
		if (qpn < 0) {
			usb4_rdma_data_rail_put(qp->rail);
			qp->rail = NULL;
			return qpn;
		}
		ibqp->qp_num = qpn;
		qp->qpn_allocated = true;
	}

	/* Register RC QPs with the data layer on RESET->INIT, matching
	 * verbs lifetime. QP1 is fixed-numbered and registered immediately. */
	pr_info("create_qp ok (%s qpn=%u, max_send_wr=%u max_recv_wr=%u)\n",
		attr->qp_type == IB_QPT_GSI ? "GSI" :
		(attr->qp_type == IB_QPT_UC ? "UC" : "RC"),
		ibqp->qp_num, attr->cap.max_send_wr, attr->cap.max_recv_wr);
	return 0;
}

static int u4r_modify_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr,
			 int attr_mask, struct ib_udata *udata)
{
	struct usb4_rdma_qp *qp = container_of(ibqp, struct usb4_rdma_qp, base);
	enum ib_qp_state old = qp->state;
	bool flush = false;

	if (attr_mask & IB_QP_STATE) {
		qp->state = attr->qp_state;
		flush = old != IB_QPS_ERR && attr->qp_state == IB_QPS_ERR;
		pr_info("modify_qp[%u]: %d -> %d\n",
			ibqp->qp_num, old, attr->qp_state);
	}
	if (attr_mask & IB_QP_DEST_QPN) {
		qp->attr.dest_qp_num = attr->dest_qp_num;
		pr_info("modify_qp[%u]: dest_qp = %u\n",
			ibqp->qp_num, attr->dest_qp_num);
	}
	if (attr_mask & IB_QP_AV) {
		/* Store dgid from av.grh on the QP. Single-peer routing
		 * ignores it today; multi-peer (multi-cable) will resolve
		 * dgid → peer at this point. */
		const struct ib_global_route *grh = rdma_ah_read_grh(&attr->ah_attr);

		qp->attr.ah_attr = attr->ah_attr;
		pr_info("modify_qp[%u]: AV set (sgid_index=%u, hop=%u)\n",
			ibqp->qp_num, grh->sgid_index, grh->hop_limit);
	}
	if (attr_mask & IB_QP_SQ_PSN)
		qp->send_psn = attr->sq_psn;
	if (attr_mask & IB_QP_RQ_PSN)
		qp->recv_psn = attr->rq_psn;
	qp->attr_mask |= attr_mask;
	if (attr_mask & ~IB_QP_STATE) {
		struct ib_qp_attr *a = &qp->attr;
		if (attr_mask & IB_QP_DEST_QPN)  a->dest_qp_num = attr->dest_qp_num;
		if (attr_mask & IB_QP_PORT)      a->port_num = attr->port_num;
		if (attr_mask & IB_QP_PATH_MTU)  a->path_mtu = attr->path_mtu;
	}

	/* UC and RC follow the same lifecycle from the data-layer's
	 * perspective: register with the dispatcher on RESET → INIT, unregister
	 * on RESET/ERR. UC just doesn't generate ACK frames or use credits. */
	if ((qp->qp_type == IB_QPT_RC || qp->qp_type == IB_QPT_UC) &&
	    !qp->registered &&
	    old == IB_QPS_RESET && qp->state == IB_QPS_INIT) {
		int ret = usb4_rdma_data_register_qp(ibqp->qp_num, qp,
						     qp->rail);

		if (ret)
			return ret;
		qp->registered = true;
	}
	if (qp->qp_type == IB_QPT_RC && flush)
		u4r_flush_qp(qp);
	/* Soft credit flow control runs for both RC and UC. RC needs it for
	 * RNR semantics; UC needs it because Apple-style JACCL races on
	 * out-of-order recv-post timing without it (silent drop on the wire). */
	if (qp->qp_type == IB_QPT_RC || qp->qp_type == IB_QPT_UC)
		u4r_advertise_recv_credits(qp);
	if ((qp->qp_type == IB_QPT_RC || qp->qp_type == IB_QPT_UC) &&
	    qp->registered &&
	    (qp->state == IB_QPS_RESET || qp->state == IB_QPS_ERR)) {
		usb4_rdma_data_unregister_qp(ibqp->qp_num, qp->rail);
		qp->registered = false;
		atomic_set(&qp->remote_recv_credits, 0);
		atomic_set(&qp->remote_recv_credit_state, U4_RECV_CREDIT_NONE);
		wake_up(&qp->send_credit_wait);
	}
	return 0;
}

static int u4r_query_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr,
			int attr_mask, struct ib_qp_init_attr *init_attr)
{
	struct usb4_rdma_qp *qp = container_of(ibqp, struct usb4_rdma_qp, base);
	*attr = qp->attr;
	attr->qp_state = qp->state;
	attr->sq_psn   = qp->send_psn;
	attr->rq_psn   = qp->recv_psn;
	memset(init_attr, 0, sizeof(*init_attr));
	init_attr->qp_type = qp->qp_type;
	init_attr->sq_sig_type = qp->sq_sig_all ? IB_SIGNAL_ALL_WR :
				 IB_SIGNAL_REQ_WR;
	return 0;
}

static void u4r_set_bad_wr(const struct ib_send_wr **bad_wr,
			   const struct ib_send_wr *wr)
{
	if (bad_wr)
		*bad_wr = wr;
}

static int u4r_destroy_qp(struct ib_qp *ibqp, struct ib_udata *udata)
{
	struct usb4_rdma_qp *qp = container_of(ibqp, struct usb4_rdma_qp, base);
	struct usb4_rdma_recv_wr *r, *tmp;
	struct usb4_rdma_read_ctx *read_ctx, *tmp_read_ctx;
	struct usb4_rdma_send_ctx *send_ctx, *tmp_send_ctx;
	unsigned long flags;

	if (qp->registered) {
		usb4_rdma_data_unregister_qp(ibqp->qp_num, qp->rail);
		qp->registered = false;
	}
	WRITE_ONCE(qp->deferred_ack_stopping, true);
	cancel_delayed_work_sync(&qp->deferred_ack_work);
	{
		struct u4r_deferred_ack *a, *atmp;

		spin_lock_irqsave(&qp->deferred_ack_lock, flags);
		list_for_each_entry_safe(a, atmp, &qp->deferred_acks, list) {
			list_del(&a->list);
			kfree(a);
		}
		spin_unlock_irqrestore(&qp->deferred_ack_lock, flags);
	}
	wait_event(qp->ref_wait, refcount_read(&qp->refs) == 1);

	/* The deferred-send worker may be blocked on credit reservation
	 * via wait_event_interruptible_timeout; ERR + wake unblocks it,
	 * cancel_work_sync waits for the worker to exit. After that
	 * nothing new can queue and we can free remaining entries. */
	if (qp->state != IB_QPS_ERR && qp->state != IB_QPS_RESET) {
		qp->state = IB_QPS_ERR;
		wake_up(&qp->send_credit_wait);
	}
	cancel_work_sync(&qp->deferred_send_work);
	{
		struct u4r_deferred_send *d, *dtmp;

		list_for_each_entry_safe(d, dtmp, &qp->deferred_sends, list) {
			list_del(&d->list);
			kfree(d);
		}
	}

	spin_lock_irqsave(&qp->send_comp_lock, flags);
	list_for_each_entry_safe(send_ctx, tmp_send_ctx, &qp->send_ctxs, list) {
		list_del(&send_ctx->list);
		kfree(send_ctx);
	}
	spin_unlock_irqrestore(&qp->send_comp_lock, flags);

	spin_lock_irqsave(&qp->recv_lock, flags);
	list_for_each_entry_safe(r, tmp, &qp->recv_q, list) {
		list_del(&r->list);
		kfree(r);
	}
	kfree(qp->in_progress_recv);
	qp->in_progress_recv = NULL;
	qp->recv_byte_offset = 0;
	qp->recv_truncated = false;
	qp->recv_posted_count = 0;
	qp->recv_credits_advertised = 0;
	spin_unlock_irqrestore(&qp->recv_lock, flags);
	atomic_set(&qp->remote_recv_credits, 0);
	atomic_set(&qp->remote_recv_credit_state, U4_RECV_CREDIT_NONE);
	wake_up(&qp->send_credit_wait);

	spin_lock_irqsave(&qp->read_lock, flags);
	list_for_each_entry_safe(read_ctx, tmp_read_ctx, &qp->read_ctxs, list) {
		list_del(&read_ctx->list);
		kfree(read_ctx);
	}
	qp->read_depth = 0;
	spin_unlock_irqrestore(&qp->read_lock, flags);
	/* Do not recycle RC QPNs during the module lifetime. The USB4 data
	 * rings can still deliver late control frames after a QP is destroyed;
	 * immediate QPN reuse lets those stale frames mutate a new QP's credit
	 * state. The 24-bit QPN space is large enough for debug/runtime use,
	 * and ida_destroy() releases the bitmap when the module unloads. */
	usb4_rdma_data_rail_put(qp->rail);
	qp->rail = NULL;
	return 0;
}

/* ----- post_send -------------------------------------------------- */

struct u4r_sge_cursor {
	struct usb4_rdma_pd *pd;
	const struct ib_sge *sg_list;
	int num_sge;
	int idx;
	u32 off;
	bool inline_data;
};

struct u4r_gsi_send_ctx {
	struct ib_device *ibdev;
	struct ib_grh grh;
	const struct ib_sge *sg_list;
	int num_sge;
	int idx;
	u32 off;
};

static int u4r_wr_total_len(const struct ib_send_wr *wr, u32 *total_len)
{
	u32 total = 0;
	int i;

	for (i = 0; i < wr->num_sge; i++) {
		if (wr->sg_list[i].length > U32_MAX - total)
			return -EMSGSIZE;
		total += wr->sg_list[i].length;
	}
	*total_len = total;
	return 0;
}

static bool u4r_use_tx_zcopy(u32 total_len)
{
	return total_len >= READ_ONCE(zcopy_min_bytes);
}

static int u4r_fill_from_dma_sges(void *dst, u32 length,
				  struct u4r_gsi_send_ctx *c)
{
	u32 copied = 0;

	if (!ib_uses_virt_dma(c->ibdev))
		return -EOPNOTSUPP;

	while (copied < length) {
		const struct ib_sge *sge;
		u32 chunk;

		while (c->idx < c->num_sge &&
		       c->off == c->sg_list[c->idx].length) {
			c->idx++;
			c->off = 0;
		}
		if (c->idx >= c->num_sge)
			return -EINVAL;

		sge = &c->sg_list[c->idx];
		if (!sge->length) {
			c->idx++;
			c->off = 0;
			continue;
		}
		if (sge->lkey != c->ibdev->local_dma_lkey)
			return -EINVAL;

		chunk = min_t(u32, sge->length - c->off, length - copied);
		memcpy((u8 *)dst + copied,
		       (const void *)(uintptr_t)(sge->addr + c->off), chunk);
		copied += chunk;
		c->off += chunk;
	}
	return 0;
}

static int u4r_fill_gsi_mad(void *dst, u32 length, void *ctx)
{
	struct u4r_gsi_send_ctx *c = ctx;

	if (length < sizeof(c->grh))
		return -EINVAL;

	memcpy(dst, &c->grh, sizeof(c->grh));
	return u4r_fill_from_dma_sges((u8 *)dst + sizeof(c->grh),
				      length - sizeof(c->grh), c);
}

static int u4r_build_gsi_grh(const struct ib_ud_wr *uwr, u32 mad_len,
			     struct ib_grh *out)
{
	struct rdma_ah_attr attr = {};
	const struct ib_global_route *grh;
	union ib_gid sgid;
	u32 flow;
	int ret;

	if (!uwr->ah)
		return -EINVAL;

	ret = rdma_query_ah(uwr->ah, &attr);
	if (ret)
		return ret;
	if (!(rdma_ah_get_ah_flags(&attr) & IB_AH_GRH)) {
		ret = -EINVAL;
		goto out;
	}

	grh = rdma_ah_read_grh(&attr);
	if (!grh->sgid_attr) {
		ret = -EINVAL;
		goto out;
	}
	sgid = grh->sgid_attr->gid;

	memset(out, 0, sizeof(*out));
	flow = grh->flow_label & IB_GRH_FLOWLABEL_MASK;
	out->version_tclass_flow =
		cpu_to_be32((6u << 28) | ((u32)grh->traffic_class << 20) |
			    flow);
	out->paylen = cpu_to_be16(mad_len);
	out->next_hdr = U4_IB_GRH_NEXT_HDR;
	out->hop_limit = grh->hop_limit;
	out->sgid = sgid;
	out->dgid = grh->dgid;

out:
	rdma_destroy_ah_attr(&attr);
	return ret;
}

static int u4r_validate_sges(struct usb4_rdma_pd *pd,
			     const struct ib_send_wr *wr,
			     bool inline_data)
{
	int i;

	for (i = 0; i < wr->num_sge; i++) {
		const struct ib_sge *sge = &wr->sg_list[i];
		struct usb4_rdma_mr *mr;

		if (!sge->length)
			continue;
		if (inline_data) {
			if (!access_ok((const void __user *)(uintptr_t)sge->addr,
				       sge->length))
				return -EFAULT;
			continue;
		}
		mr = u4r_pd_get_mr_by_lkey(pd, sge->lkey);
		if (!mr)
			return -EINVAL;
		if (u4r_mr_check_range(mr, sge->addr, sge->length)) {
			u4r_mr_put(mr);
			return -ERANGE;
		}
		u4r_mr_put(mr);
	}
	return 0;
}

static int u4r_gsi_send_one(struct usb4_rdma_qp *qp,
			    const struct ib_send_wr *wr)
{
	const struct ib_ud_wr *uwr = ud_wr(wr);
	struct u4r_gsi_send_ctx ctx = {
		.ibdev = qp->base.device,
		.sg_list = wr->sg_list,
		.num_sge = wr->num_sge,
	};
	u32 mad_len;
	int ret;

	ret = u4r_wr_total_len(wr, &mad_len);
	if (ret)
		return ret;
	if (mad_len > U4_MAX_PAYLOAD - sizeof(ctx.grh))
		return -EMSGSIZE;
	ret = u4r_build_gsi_grh(uwr, mad_len, &ctx.grh);
	if (ret)
		return ret;

	mutex_lock(&qp->send_lock);
	ret = usb4_rdma_data_send(qp->rail, U4_OP_SEND, qp->base.qp_num,
				  uwr->remote_qpn, qp->send_psn++,
				  U4_F_LAST | U4_F_SOLICITED,
				  0, 0, 0, u4r_fill_gsi_mad, &ctx,
				  sizeof(ctx.grh) + mad_len);
	mutex_unlock(&qp->send_lock);
	return ret;
}

static int u4r_fill_from_sges(void *dst, u32 length, void *ctx)
{
	struct u4r_sge_cursor *c = ctx;
	u32 copied = 0;

	while (copied < length) {
		const struct ib_sge *sge;
		struct usb4_rdma_mr *mr;
		u32 chunk;

		while (c->idx < c->num_sge &&
		       c->off == c->sg_list[c->idx].length) {
			c->idx++;
			c->off = 0;
		}
		if (c->idx >= c->num_sge)
			return -EINVAL;

		sge = &c->sg_list[c->idx];
		if (!sge->length) {
			c->idx++;
			c->off = 0;
			continue;
		}

		chunk = min_t(u32, sge->length - c->off, length - copied);
		if (c->inline_data) {
			if (copy_from_user((u8 *)dst + copied,
					   (const void __user *)(uintptr_t)
						   (sge->addr + c->off),
					   chunk))
				return -EFAULT;
			copied += chunk;
			c->off += chunk;
			continue;
		}

		mr = u4r_pd_get_mr_by_lkey(c->pd, sge->lkey);
		if (!mr)
			return -EINVAL;
		if (u4r_mr_xfer(mr, sge->addr + c->off, (u8 *)dst + copied,
				chunk, true)) {
			u4r_mr_put(mr);
			return -EFAULT;
		}
		u4r_mr_put(mr);

		copied += chunk;
		c->off += chunk;
	}
	return 0;
}

/* Zero-copy send path: walk a WR's SGE list and emit one (struct page *,
 * page_off, length) tuple per call to data.c's send_page_stream. The DMA
 * mapping happens inside data.c (dma_map_page on the user's pinned MR
 * page directly), avoiding the staging-frame memcpy used by
 * u4r_fill_from_sges. Each segment hands an MR refcount off to the done
 * callback (u4r_mr_put_done) which fires once tb_ring signals TX
 * completion, keeping the MR alive across the in-flight DMA window. */
struct u4r_zc_send_cursor {
	struct usb4_rdma_pd *pd;
	const struct ib_sge *sg_list;
	int num_sge;
	int sge_idx;
	u32 sge_off;
	struct usb4_rdma_mr *cur_mr;	/* one ref held while traversing this SGE */
};

static void u4r_zc_send_cursor_init(struct u4r_zc_send_cursor *c,
				    struct usb4_rdma_pd *pd,
				    const struct ib_sge *sg_list, int num_sge)
{
	c->pd = pd;
	c->sg_list = sg_list;
	c->num_sge = num_sge;
	c->sge_idx = 0;
	c->sge_off = 0;
	c->cur_mr = NULL;
}

static void u4r_zc_send_cursor_fini(struct u4r_zc_send_cursor *c)
{
	if (c->cur_mr) {
		u4r_mr_put(c->cur_mr);
		c->cur_mr = NULL;
	}
}

static int u4r_zc_next_send_page(void *opaque, struct page **page,
				 u32 *page_idx_out, u32 *page_off,
				 u32 *length,
				 usb4_rdma_data_dma_resolve_fn *resolve,
				 void **resolve_ctx,
				 usb4_rdma_data_done_fn *done,
				 void **done_ctx)
{
	struct u4r_zc_send_cursor *c = opaque;
	const struct ib_sge *sge;
	u32 page_idx, remaining;
	int ret;

	/* Advance past empty / fully-consumed SGEs; acquire MR for the
	 * current one if we don't already hold it. */
	while (c->sge_idx < c->num_sge) {
		sge = &c->sg_list[c->sge_idx];
		if (sge->length == 0 || c->sge_off == sge->length) {
			if (c->cur_mr) {
				u4r_mr_put(c->cur_mr);
				c->cur_mr = NULL;
			}
			c->sge_idx++;
			c->sge_off = 0;
			continue;
		}
		if (!c->cur_mr) {
			c->cur_mr = u4r_pd_get_mr_by_lkey(c->pd, sge->lkey);
			if (!c->cur_mr)
				return -EINVAL;
		}
		break;
	}
	if (c->sge_idx >= c->num_sge)
		return -ENOENT;

	sge = &c->sg_list[c->sge_idx];
	remaining = sge->length - c->sge_off;
	ret = u4r_mr_page_chunk(c->cur_mr, sge->addr + c->sge_off, remaining,
				page, &page_idx, page_off, length);
	if (ret)
		return ret;

	c->sge_off += *length;

	/* Hand the done callback an extra MR reference; it'll drop it via
	 * u4r_mr_put_done when this segment's DMA completes. */
	refcount_inc(&c->cur_mr->refs);
	*page_idx_out = page_idx;
	*resolve = u4r_mr_resolve_dma;
	*resolve_ctx = c->cur_mr;
	*done = u4r_mr_put_done;
	*done_ctx = c->cur_mr;
	return 0;
}

/* Build wire frames directly from one WR's SGE list. For non-INLINE WRs
 * the SGEs reference MR-backed pinned pages and we use the zero-copy
 * page-stream path (data.c dma_map_page's the source page directly into
 * the NHI descriptor). For IB_SEND_INLINE the source is user memory not
 * an MR, so we fall back to the staging-frame fill-callback path. */
static int u4r_send_one(struct usb4_rdma_qp *qp, const struct ib_send_wr *wr)
{
	struct usb4_rdma_pd *pd =
		container_of(qp->base.pd, struct usb4_rdma_pd, base);
	struct u4r_sge_cursor cursor = {
		.pd = pd,
		.sg_list = wr->sg_list,
		.num_sge = wr->num_sge,
		.inline_data = !!(wr->send_flags & IB_SEND_INLINE),
	};
	struct usb4_rdma_send_ctx *sctx = NULL;
	bool is_uc = (qp->qp_type == IB_QPT_UC);
	/* UC has no wire-level ACK and no credit semantics. The caller decides
	 * whether the WR should generate a CQE; for UC we always post the CQE
	 * inline in u4r_post_send (see defer_wc logic), so ack_req is irrelevant
	 * to the UC SEND path. */
	bool ack_req = !is_uc &&
		(qp->sq_sig_all || (wr->send_flags & IB_SEND_SIGNALED));
	bool credit_reserved = false;
	bool data_started = false;
	u32 total_len, sent = 0;
	int ret;

	ret = u4r_wr_total_len(wr, &total_len);
	if (ret)
		return ret;
	ret = u4r_validate_sges(pd, wr, cursor.inline_data);
	if (ret)
		return ret;

	/* Reserve a recv credit on the peer before transmitting. UC needs
	 * this too despite being "unreliable" — without it the peer's
	 * recv queue can be empty when our SEND lands, which causes a
	 * silent drop (JACCL hang). We re-use RC's credit machinery. */
	ret = u4r_reserve_remote_recv_credit(qp);
	if (ret) {
		if (ret < 0)
			return ret;
		credit_reserved = true;
		ret = 0;
	}

	if (ack_req) {
		sctx = kzalloc(sizeof(*sctx), GFP_KERNEL);
		if (!sctx) {
			ret = -ENOMEM;
			goto refund_credit;
		}
		INIT_LIST_HEAD(&sctx->list);
		sctx->wr_id = wr->wr_id;
		sctx->wr_cqe = wr->wr_cqe;
		sctx->opcode = IB_WC_SEND;
	}

	mutex_lock(&qp->send_lock);
	/* Zero-byte SEND is legal — emit one frame with U4_F_LAST. */
	if (total_len == 0) {
		u32 psn = qp->send_psn++;

		if (sctx) {
			unsigned long flags;

			sctx->ack_psn = psn;
			spin_lock_irqsave(&qp->send_comp_lock, flags);
			list_add_tail(&sctx->list, &qp->send_ctxs);
			spin_unlock_irqrestore(&qp->send_comp_lock, flags);
			if (READ_ONCE(send_ack_debug))
				pr_info("send_ack wait: qp=%u dest=%u psn=%u len=0\n",
					qp->base.qp_num, qp->attr.dest_qp_num,
					psn);
		}
		ret = usb4_rdma_data_send(qp->rail, U4_OP_SEND, qp->base.qp_num,
					  qp->attr.dest_qp_num, psn,
					  U4_F_LAST |
					  (ack_req ? U4_F_SOLICITED : 0),
					  0, 0, 0, NULL, NULL, 0);
		if (!ret)
			data_started = true;
		if (ret && sctx) {
			unsigned long flags;

			spin_lock_irqsave(&qp->send_comp_lock, flags);
			list_del_init(&sctx->list);
			spin_unlock_irqrestore(&qp->send_comp_lock, flags);
		}
		goto out;
	}

	/* Zero-copy SEND is only safe with active_lane_count == 1.
	 *
	 * usb4_rdma_data_send_page_stream() stripes payload across lanes for
	 * bandwidth. RC SEND's RX-side reassembly (u4r_rx_send_handler →
	 * recv_byte_offset) is order-dependent: each frame is appended at
	 * the running offset, so out-of-order delivery between cables would
	 * scatter bytes to the wrong positions and U4_F_LAST could fire on
	 * an incomplete WR. RDMA WRITE doesn't have this issue because each
	 * wire frame carries its own remote_addr in the header — the
	 * receiver writes each frame at the address it names, no implicit
	 * order required.
	 *
	 * Until we add offset metadata to the SEND wire frames (or move to
	 * per-fragment-PSN reassembly on RX), restrict zero-copy SEND to
	 * single-lane. Multi-lane SENDs fall back to the staging-frame path
	 * which serializes through one peer and preserves PSN order.
	 */
	if (!cursor.inline_data && u4r_use_tx_zcopy(total_len) &&
	    (qp->rail || usb4_rdma_data_active_lane_count() == 1)) {
		struct u4r_zc_send_cursor zc;
		u32 psn = qp->send_psn++;

		if (sctx) {
			unsigned long flags;

			sctx->ack_psn = psn;
			spin_lock_irqsave(&qp->send_comp_lock, flags);
			list_add_tail(&sctx->list, &qp->send_ctxs);
			spin_unlock_irqrestore(&qp->send_comp_lock, flags);
			if (READ_ONCE(send_ack_debug))
				pr_info("send_ack wait: qp=%u dest=%u psn=%u len=%u zcopy=1\n",
					qp->base.qp_num, qp->attr.dest_qp_num,
					psn, total_len);
		}
		u4r_zc_send_cursor_init(&zc, pd, wr->sg_list, wr->num_sge);
		ret = usb4_rdma_data_send_page_stream(qp->rail, U4_OP_SEND,
						      qp->base.qp_num,
						      qp->attr.dest_qp_num,
						      psn,
						      U4_F_LAST |
						      (ack_req ? U4_F_SOLICITED : 0),
						      0, 0, 0, total_len,
						      u4r_zc_next_send_page,
						      &zc);
		u4r_zc_send_cursor_fini(&zc);
		if (!ret)
			data_started = true;
		if (ret && sctx) {
			unsigned long flags;

			spin_lock_irqsave(&qp->send_comp_lock, flags);
			list_del_init(&sctx->list);
			spin_unlock_irqrestore(&qp->send_comp_lock, flags);
		}
		goto out;
	}

	while (sent < total_len) {
		u32 chunk = min_t(u32, total_len - sent, U4_MAX_PAYLOAD);
		bool last = (sent + chunk == total_len);
		u8 wire_flags = last ? U4_F_LAST : 0;
		u32 psn = qp->send_psn++;

		if (last && ack_req) {
			unsigned long flags;

			wire_flags |= U4_F_SOLICITED;
			sctx->ack_psn = psn;
			spin_lock_irqsave(&qp->send_comp_lock, flags);
			list_add_tail(&sctx->list, &qp->send_ctxs);
			spin_unlock_irqrestore(&qp->send_comp_lock, flags);
			if (READ_ONCE(send_ack_debug))
				pr_info("send_ack wait: qp=%u dest=%u psn=%u len=%u zcopy=0\n",
					qp->base.qp_num, qp->attr.dest_qp_num,
					psn, total_len);
		}

		ret = usb4_rdma_data_send(qp->rail, U4_OP_SEND, qp->base.qp_num,
					  qp->attr.dest_qp_num,
					  psn, wire_flags,
					  0, 0, 0, u4r_fill_from_sges,
					  &cursor, chunk);
		if (ret) {
			if (last && ack_req) {
				unsigned long flags;

				spin_lock_irqsave(&qp->send_comp_lock, flags);
				list_del_init(&sctx->list);
				spin_unlock_irqrestore(&qp->send_comp_lock, flags);
			}
			goto out;
		}
		data_started = true;
		sent += chunk;
	}
out:
	mutex_unlock(&qp->send_lock);
	if (ret && credit_reserved && !data_started)
		u4r_refund_remote_recv_credit(qp);
	if (ret || !ack_req)
		kfree(sctx);
	return ret;

refund_credit:
	u4r_refund_remote_recv_credit(qp);
	return ret;
}

static int u4r_write_one(struct usb4_rdma_qp *qp, const struct ib_send_wr *wr)
{
	const struct ib_rdma_wr *rwr = rdma_wr(wr);
	struct usb4_rdma_pd *pd =
		container_of(qp->base.pd, struct usb4_rdma_pd, base);
	struct u4r_sge_cursor cursor = {
		.pd = pd,
		.sg_list = wr->sg_list,
		.num_sge = wr->num_sge,
		.inline_data = !!(wr->send_flags & IB_SEND_INLINE),
	};
	u8 opcode = wr->opcode == IB_WR_RDMA_WRITE_WITH_IMM ?
		    U4_OP_RDMA_WRITE_WITH_IMM : U4_OP_RDMA_WRITE;
	__be32 imm_data = wr->opcode == IB_WR_RDMA_WRITE_WITH_IMM ?
			  wr->ex.imm_data : 0;
	u32 total_len, sent = 0;
	int ret;

	ret = u4r_wr_total_len(wr, &total_len);
	if (ret)
		return ret;
	ret = u4r_validate_sges(pd, wr, cursor.inline_data);
	if (ret)
		return ret;
	if (total_len && rwr->remote_addr > U64_MAX - total_len)
		return -EINVAL;

	mutex_lock(&qp->send_lock);
	if (total_len == 0) {
		ret = usb4_rdma_data_send(qp->rail, opcode, qp->base.qp_num,
					  qp->attr.dest_qp_num,
					  qp->send_psn++, U4_F_LAST,
					  imm_data, rwr->remote_addr,
					  rwr->rkey, NULL, NULL, 0);
		goto out;
	}

	if (!cursor.inline_data && u4r_use_tx_zcopy(total_len)) {
		struct u4r_zc_send_cursor zc;

		u4r_zc_send_cursor_init(&zc, pd, wr->sg_list, wr->num_sge);
		ret = usb4_rdma_data_send_page_stream(qp->rail, opcode,
						      qp->base.qp_num,
						      qp->attr.dest_qp_num,
						      qp->send_psn++,
						      U4_F_LAST, imm_data,
						      rwr->remote_addr,
						      rwr->rkey, total_len,
						      u4r_zc_next_send_page,
						      &zc);
		u4r_zc_send_cursor_fini(&zc);
		goto out;
	}

	while (sent < total_len) {
		u32 chunk = min_t(u32, total_len - sent, U4_MAX_PAYLOAD);
		bool last = sent + chunk == total_len;
		u8 flags = last ? U4_F_LAST : 0;

		ret = usb4_rdma_data_send(qp->rail, opcode, qp->base.qp_num,
					  qp->attr.dest_qp_num,
					  qp->send_psn++, flags, imm_data,
					  rwr->remote_addr + sent, rwr->rkey,
					  u4r_fill_from_sges, &cursor, chunk);
		if (ret)
			goto out;
		sent += chunk;
	}
out:
	mutex_unlock(&qp->send_lock);
	return ret;
}

static int u4r_read_one(struct usb4_rdma_qp *qp, const struct ib_send_wr *wr)
{
	const struct ib_rdma_wr *rwr = rdma_wr(wr);
	struct usb4_rdma_pd *pd =
		container_of(qp->base.pd, struct usb4_rdma_pd, base);
	struct usb4_rdma_read_ctx *ctx;
	unsigned long flags;
	u32 total_len;
	int ret, i;

	ret = u4r_wr_total_len(wr, &total_len);
	if (ret)
		return ret;
	ret = u4r_validate_sges(pd, wr, false);
	if (ret)
		return ret;
	if (total_len && rwr->remote_addr > U64_MAX - total_len)
		return -EINVAL;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	INIT_LIST_HEAD(&ctx->list);
	ctx->wr_id = wr->wr_id;
	ctx->wr_cqe = wr->wr_cqe;
	ctx->signaled = qp->sq_sig_all || (wr->send_flags & IB_SEND_SIGNALED);
	ctx->num_sge = wr->num_sge;
	ctx->total_len = total_len;
	ctx->status = IB_WC_SUCCESS;
	for (i = 0; i < wr->num_sge; i++)
		ctx->sge[i] = wr->sg_list[i];

	mutex_lock(&qp->send_lock);
	spin_lock_irqsave(&qp->read_lock, flags);
	if (qp->read_depth >= U4_MAX_READ_CTX) {
		spin_unlock_irqrestore(&qp->read_lock, flags);
		mutex_unlock(&qp->send_lock);
		kfree(ctx);
		return -EBUSY;
	}
	ctx->psn = qp->send_psn++;
	list_add_tail(&ctx->list, &qp->read_ctxs);
	qp->read_depth++;
	spin_unlock_irqrestore(&qp->read_lock, flags);

	ret = usb4_rdma_data_send(qp->rail, U4_OP_RDMA_READ_REQ, qp->base.qp_num,
				  qp->attr.dest_qp_num, ctx->psn,
				  U4_F_LAST, cpu_to_be32(total_len),
				  rwr->remote_addr, rwr->rkey,
				  NULL, NULL, 0);
	if (ret) {
		spin_lock_irqsave(&qp->read_lock, flags);
		if (!list_empty(&ctx->list)) {
			list_del_init(&ctx->list);
			qp->read_depth--;
		}
		spin_unlock_irqrestore(&qp->read_lock, flags);
		kfree(ctx);
	}
	mutex_unlock(&qp->send_lock);
	return ret;
}

/* Worker that drains qp->deferred_sends. Runs in process context, so
 * blocking on credit reservation in u4r_send_one is fine — the user
 * thread that called post_send returned long ago. */
static void u4r_deferred_send_work_fn(struct work_struct *w)
{
	struct usb4_rdma_qp *qp = container_of(w, struct usb4_rdma_qp,
					       deferred_send_work);
	struct usb4_rdma_cq *send_cq =
		container_of(qp->base.send_cq, struct usb4_rdma_cq, base);
	struct u4r_deferred_send *d;
	unsigned long flags;

	for (;;) {
		struct ib_send_wr swr = {};
		struct ib_rdma_wr rwr = {};
		const struct ib_send_wr *wr_ptr;
		enum ib_wc_opcode wc_opcode;
		bool signaled;
		int ret;

		spin_lock_irqsave(&qp->deferred_send_lock, flags);
		d = list_first_entry_or_null(&qp->deferred_sends,
					     struct u4r_deferred_send, list);
		if (d)
			list_del(&d->list);
		spin_unlock_irqrestore(&qp->deferred_send_lock, flags);

		if (!d)
			break;

		signaled = qp->sq_sig_all ||
			   (d->send_flags & IB_SEND_SIGNALED);

		if (qp->state != IB_QPS_RTS && qp->state != IB_QPS_SQD &&
		    qp->state != IB_QPS_SQE) {
			if (signaled) {
				struct ib_wc wc = {};

				wc.wr_id = d->wr_id;
				wc.wr_cqe = d->wr_cqe;
				wc.status = IB_WC_WR_FLUSH_ERR;
				wc.opcode = (d->opcode == IB_WR_SEND) ?
					    IB_WC_SEND : IB_WC_RDMA_WRITE;
				wc.qp = &qp->base;
				u4r_cq_push_wc(send_cq, &wc);
			}
			kfree(d);
			continue;
		}

		if (d->opcode == IB_WR_SEND) {
			swr.wr_id = d->wr_id;
			swr.wr_cqe = d->wr_cqe;
			swr.sg_list = d->sges;
			swr.num_sge = d->num_sge;
			swr.opcode = d->opcode;
			swr.send_flags = d->send_flags;
			wr_ptr = &swr;
			wc_opcode = IB_WC_SEND;
			ret = u4r_send_one(qp, wr_ptr);
		} else {
			rwr.wr.wr_id = d->wr_id;
			rwr.wr.wr_cqe = d->wr_cqe;
			rwr.wr.sg_list = d->sges;
			rwr.wr.num_sge = d->num_sge;
			rwr.wr.opcode = d->opcode;
			rwr.wr.send_flags = d->send_flags;
			rwr.wr.ex.imm_data = d->imm_data;
			rwr.remote_addr = d->remote_addr;
			rwr.rkey = d->rkey;
			wr_ptr = &rwr.wr;
			wc_opcode = IB_WC_RDMA_WRITE;
			ret = u4r_write_one(qp, wr_ptr);
		}

		if (signaled) {
			struct ib_wc wc = {};

			wc.wr_id = d->wr_id;
			wc.wr_cqe = d->wr_cqe;
			wc.status = ret ? IB_WC_GENERAL_ERR : IB_WC_SUCCESS;
			wc.opcode = wc_opcode;
			wc.qp = &qp->base;
			u4r_cq_push_wc(send_cq, &wc);
		}
		if (ret)
			pr_warn_ratelimited("deferred send[qp=%u]: failed %d\n",
					    qp->base.qp_num, ret);
		kfree(d);
	}
}

static int u4r_capture_deferred_send(struct usb4_rdma_qp *qp,
				     const struct ib_send_wr *wr)
{
	struct u4r_deferred_send *d;
	unsigned long flags;

	if (wr->num_sge > U4_MAX_SGE)
		return -EINVAL;
	/* Inline data would need a private copy because the user buffer
	 * can be reused after post_send returns. JACCL doesn't use inline
	 * (max_inline_data=0); reject if anyone tries it for now. */
	if (wr->send_flags & IB_SEND_INLINE)
		return -EOPNOTSUPP;

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;

	INIT_LIST_HEAD(&d->list);
	d->opcode = wr->opcode;
	d->send_flags = wr->send_flags;
	d->wr_id = wr->wr_id;
	d->wr_cqe = wr->wr_cqe;
	d->num_sge = wr->num_sge;
	if (wr->num_sge && wr->sg_list)
		memcpy(d->sges, wr->sg_list,
		       sizeof(d->sges[0]) * wr->num_sge);
	if (wr->opcode == IB_WR_RDMA_WRITE ||
	    wr->opcode == IB_WR_RDMA_WRITE_WITH_IMM) {
		const struct ib_rdma_wr *rwr = rdma_wr(wr);

		d->remote_addr = rwr->remote_addr;
		d->rkey = rwr->rkey;
	}
	if (wr->opcode == IB_WR_RDMA_WRITE_WITH_IMM)
		d->imm_data = wr->ex.imm_data;

	spin_lock_irqsave(&qp->deferred_send_lock, flags);
	list_add_tail(&d->list, &qp->deferred_sends);
	spin_unlock_irqrestore(&qp->deferred_send_lock, flags);

	queue_work(system_unbound_wq, &qp->deferred_send_work);
	return 0;
}

static bool u4r_deferred_ack_pending(struct usb4_rdma_qp *qp)
{
	unsigned long flags;
	bool pending;

	spin_lock_irqsave(&qp->deferred_ack_lock, flags);
	pending = qp->deferred_ack_active ||
		  !list_empty(&qp->deferred_acks);
	spin_unlock_irqrestore(&qp->deferred_ack_lock, flags);
	return pending;
}

static bool u4r_can_send_deferred_ack(const struct usb4_rdma_qp *qp)
{
	enum ib_qp_state state = READ_ONCE(qp->state);

	if (READ_ONCE(qp->deferred_ack_stopping) || !READ_ONCE(qp->registered))
		return false;

	return state == IB_QPS_RTS || state == IB_QPS_SQD ||
	       state == IB_QPS_SQE;
}

static int u4r_queue_deferred_ack(struct usb4_rdma_qp *qp, u32 dest_qp,
				  u32 psn, __be32 status)
{
	struct u4r_deferred_ack *a;
	unsigned long flags;
	gfp_t gfp = in_interrupt() ? GFP_ATOMIC : GFP_KERNEL;
	bool queue_work = false;

	if (READ_ONCE(qp->deferred_ack_stopping))
		return -ESHUTDOWN;

	a = kzalloc(sizeof(*a), gfp);
	if (!a)
		return -ENOMEM;

	INIT_LIST_HEAD(&a->list);
	a->dest_qp = dest_qp;
	a->psn = psn;
	a->status = status;

	spin_lock_irqsave(&qp->deferred_ack_lock, flags);
	if (READ_ONCE(qp->deferred_ack_stopping)) {
		spin_unlock_irqrestore(&qp->deferred_ack_lock, flags);
		kfree(a);
		return -ESHUTDOWN;
	}
	list_add_tail(&a->list, &qp->deferred_acks);
	if (!qp->deferred_ack_active) {
		qp->deferred_ack_active = true;
		queue_work = true;
	}
	spin_unlock_irqrestore(&qp->deferred_ack_lock, flags);

	if (queue_work)
		queue_delayed_work(system_unbound_wq, &qp->deferred_ack_work, 0);
	return 0;
}

static void u4r_deferred_ack_work_fn(struct work_struct *w)
{
	struct usb4_rdma_qp *qp =
		container_of(to_delayed_work(w), struct usb4_rdma_qp,
			     deferred_ack_work);
	unsigned long flags;

	for (;;) {
		struct u4r_deferred_ack *a;
		int ret;

		spin_lock_irqsave(&qp->deferred_ack_lock, flags);
		a = list_first_entry_or_null(&qp->deferred_acks,
					     struct u4r_deferred_ack, list);
		if (a) {
			list_del_init(&a->list);
		} else {
			qp->deferred_ack_active = false;
		}
		spin_unlock_irqrestore(&qp->deferred_ack_lock, flags);

		if (!a)
			break;

		if (!u4r_can_send_deferred_ack(qp)) {
			kfree(a);
			continue;
		}

		ret = usb4_rdma_data_send_ack_try(qp->rail, qp->base.qp_num, a->dest_qp,
						  a->psn, a->status);
		if (ret == -EAGAIN) {
			spin_lock_irqsave(&qp->deferred_ack_lock, flags);
			list_add(&a->list, &qp->deferred_acks);
			spin_unlock_irqrestore(&qp->deferred_ack_lock, flags);
			if (!READ_ONCE(qp->deferred_ack_stopping))
				mod_delayed_work(system_unbound_wq,
						 &qp->deferred_ack_work,
						 U4_DEFERRED_ACK_RETRY_DELAY);
			break;
		}

		if (ret)
			pr_warn_ratelimited("send_ack deferred[qp=%u]: dest=%u psn=%u failed %d\n",
					    qp->base.qp_num, a->dest_qp,
					    a->psn, ret);
		else if (READ_ONCE(send_ack_debug))
			pr_info("send_ack deferred[qp=%u]: dest=%u psn=%u sent\n",
				qp->base.qp_num, a->dest_qp, a->psn);
		kfree(a);
	}
}

static int u4r_post_send(struct ib_qp *ibqp, const struct ib_send_wr *wr,
			 const struct ib_send_wr **bad_wr)
{
	struct usb4_rdma_qp *qp = container_of(ibqp, struct usb4_rdma_qp, base);
	struct usb4_rdma_cq *send_cq =
		container_of(ibqp->send_cq, struct usb4_rdma_cq, base);

	if (qp->state != IB_QPS_RTS && qp->state != IB_QPS_SQD &&
	    qp->state != IB_QPS_SQE) {
		u4r_set_bad_wr(bad_wr, wr);
		return -EINVAL;
	}

	for (; wr; wr = wr->next) {
		struct ib_wc wc = {};
		enum ib_wc_opcode wc_opcode;
		bool defer_wc = false;
		int ret;

		if (wr->num_sge > U4_MAX_SGE || (wr->num_sge && !wr->sg_list)) {
			u4r_set_bad_wr(bad_wr, wr);
			return -EINVAL;
		}

		if (qp->qp_type == IB_QPT_GSI) {
			if (wr->opcode != IB_WR_SEND) {
				u4r_set_bad_wr(bad_wr, wr);
				return -EOPNOTSUPP;
			}
			ret = u4r_gsi_send_one(qp, wr);
			if (ret) {
				u4r_set_bad_wr(bad_wr, wr);
				return ret;
			}
			wc.wr_cqe = wr->wr_cqe;
			wc.status = IB_WC_SUCCESS;
			wc.opcode = IB_WC_SEND;
			wc.qp = ibqp;
			u4r_cq_push_wc(send_cq, &wc);
			continue;
		}

		switch (wr->opcode) {
		case IB_WR_SEND:
			/* UC SEND must not block — JACCL calls post_send
			 * from inside its poll-completion loop and a block
			 * here deadlocks both peers. Defer the actual TX
			 * (and its credit reservation) to a workqueue. The
			 * CQE is generated by the worker on completion. */
			if (qp->qp_type == IB_QPT_UC) {
				ret = u4r_capture_deferred_send(qp, wr);
				if (ret) {
					u4r_set_bad_wr(bad_wr, wr);
					return ret;
				}
				continue;
			}
			ret = u4r_send_one(qp, wr);
			wc_opcode = IB_WC_SEND;
			/* RC defers the SEND CQE until the wire ACK arrives. */
			defer_wc = (qp->qp_type == IB_QPT_RC) &&
				(qp->sq_sig_all ||
				 (wr->send_flags & IB_SEND_SIGNALED));
			break;
		case IB_WR_RDMA_WRITE:
		case IB_WR_RDMA_WRITE_WITH_IMM:
			ret = u4r_write_one(qp, wr);
			wc_opcode = IB_WC_RDMA_WRITE;
			break;
		case IB_WR_RDMA_READ:
			ret = u4r_read_one(qp, wr);
			wc_opcode = IB_WC_RDMA_READ;
			defer_wc = true;
			break;
		default:
			pr_warn("post_send: opcode %d not implemented\n",
				wr->opcode);
			u4r_set_bad_wr(bad_wr, wr);
			return -EOPNOTSUPP;
		}
		if (ret) {
			u4r_set_bad_wr(bad_wr, wr);
			return ret;
		}

		if (!defer_wc &&
		    (qp->sq_sig_all || (wr->send_flags & IB_SEND_SIGNALED))) {
			wc.wr_id        = wr->wr_id;
			wc.status       = IB_WC_SUCCESS;
			wc.opcode       = wc_opcode;
			wc.qp           = ibqp;
			wc.byte_len     = 0;
			u4r_cq_push_wc(send_cq, &wc);
		}
	}
	return 0;
}

/* ----- post_recv -------------------------------------------------- */

static void u4r_set_bad_recv_wr(const struct ib_recv_wr **bad_wr,
				const struct ib_recv_wr *wr)
{
	if (bad_wr)
		*bad_wr = wr;
}

static int u4r_post_recv(struct ib_qp *ibqp, const struct ib_recv_wr *wr,
			 const struct ib_recv_wr **bad_wr)
{
	struct usb4_rdma_qp *qp = container_of(ibqp, struct usb4_rdma_qp, base);
	unsigned long flags;

	for (; wr; wr = wr->next) {
		struct usb4_rdma_recv_wr *r;
		int i;

		if (wr->num_sge > U4_MAX_SGE) {
			u4r_set_bad_recv_wr(bad_wr, wr);
			return -EINVAL;
		}
		r = kmalloc(sizeof(*r), GFP_KERNEL);
		if (!r) {
			u4r_set_bad_recv_wr(bad_wr, wr);
			return -ENOMEM;
		}
		r->wr_id   = wr->wr_id;
		r->wr_cqe  = wr->wr_cqe;
		r->num_sge = wr->num_sge;
		for (i = 0; i < wr->num_sge; i++)
			r->sge[i] = wr->sg_list[i];
		spin_lock_irqsave(&qp->recv_lock, flags);
		list_add_tail(&r->list, &qp->recv_q);
		if (qp->qp_type == IB_QPT_RC || qp->qp_type == IB_QPT_UC)
			qp->recv_posted_count++;
		spin_unlock_irqrestore(&qp->recv_lock, flags);
	}
	u4r_advertise_recv_credits(qp);
	return 0;
}

/* ----- RX dispatch (called from data.c softirq context) ----------- */

/* Scatter `len` bytes from `payload` into a recv WR's SGE list,
 * starting at byte offset `dst_off` from the start of the WR.
 * Returns 0 on success, -ERANGE if the WR is full, or other negative
 * on MR/lkey errors. */
static int u4r_recv_scatter(struct usb4_rdma_pd *pd,
			    struct usb4_rdma_recv_wr *r,
			    u32 dst_off, const void *payload, u32 len)
{
	u32 cur = 0, copied = 0;
	int i;

	for (i = 0; i < r->num_sge && copied < len; i++) {
		const struct ib_sge *sge = &r->sge[i];
		struct usb4_rdma_mr *mr;
		u32 in_sge_off, chunk;

		if (cur + sge->length <= dst_off) {
			cur += sge->length;
			continue;
		}
		in_sge_off = (dst_off + copied) - cur;
		chunk = min(sge->length - in_sge_off, len - copied);
		mr = u4r_pd_get_mr_by_lkey(pd, sge->lkey);
		if (!mr)
			return -EINVAL;
		if (u4r_mr_xfer(mr, sge->addr + in_sge_off,
				(void *)payload + copied, chunk, false)) {
			u4r_mr_put(mr);
			return -EFAULT;
		}
		u4r_mr_put(mr);
		copied += chunk;
		cur += sge->length;
	}
	return copied < len ? -ERANGE : 0;
}

static int u4r_recv_dma_scatter(struct ib_device *ibdev,
				struct usb4_rdma_recv_wr *r,
				const void *payload, u32 len, u32 *copied_out)
{
	u32 copied = 0;
	int i;

	if (!ib_uses_virt_dma(ibdev))
		return -EOPNOTSUPP;

	for (i = 0; i < r->num_sge && copied < len; i++) {
		const struct ib_sge *sge = &r->sge[i];
		u32 chunk;

		if (!sge->length)
			continue;
		if (sge->lkey != ibdev->local_dma_lkey)
			return -EINVAL;

		chunk = min_t(u32, sge->length, len - copied);
		memcpy((void *)(uintptr_t)sge->addr,
		       (const u8 *)payload + copied, chunk);
		copied += chunk;
	}

	*copied_out = copied;
	return copied < len ? -ERANGE : 0;
}

static void u4r_rx_gsi_handler(struct usb4_rdma_qp *qp,
			       const struct u4_wire_hdr *hdr,
			       const void *payload, u32 length)
{
	struct usb4_rdma_cq *recv_cq =
		container_of(qp->base.recv_cq, struct usb4_rdma_cq, base);
	struct usb4_rdma_recv_wr *r;
	unsigned long flags;
	struct ib_wc wc = {};
	u32 copied = 0;
	int ret;

	spin_lock_irqsave(&qp->recv_lock, flags);
	r = u4r_pop_recv_wr_locked(qp);
	spin_unlock_irqrestore(&qp->recv_lock, flags);

	if (!r) {
		pr_warn_ratelimited("gsi rx: no pending recv WR, dropping %u bytes\n",
				    length);
		return;
	}

	ret = u4r_recv_dma_scatter(qp->base.device, r, payload, length,
				   &copied);
	if (ret == -ERANGE)
		wc.status = IB_WC_LOC_LEN_ERR;
	else if (ret)
		wc.status = IB_WC_LOC_PROT_ERR;
	else
		wc.status = IB_WC_SUCCESS;

	wc.wr_id = r->wr_id;
	wc.wr_cqe = r->wr_cqe;
	wc.opcode = IB_WC_RECV;
	wc.qp = &qp->base;
	wc.byte_len = copied;
	wc.src_qp = le32_to_cpu(hdr->src_qp);
	wc.pkey_index = 0;
	wc.port_num = 1;
	wc.wc_flags = IB_WC_GRH;
	u4r_cq_push_wc(recv_cq, &wc);
	kfree(r);
}

static void u4r_send_send_ack(struct usb4_rdma_qp *qp,
			      const struct u4_wire_hdr *hdr,
			      enum ib_wc_status status)
{
	u32 dest_qp = le32_to_cpu(hdr->src_qp);
	u32 psn = le32_to_cpu(hdr->psn);
	bool deferred = false;
	int ret;

	if (in_interrupt()) {
		if (u4r_deferred_ack_pending(qp))
			ret = -EAGAIN;
		else
			ret = usb4_rdma_data_send_ack_atomic(qp->rail, qp->base.qp_num,
							     dest_qp, psn,
							     cpu_to_be32(status));
		if (ret == -EAGAIN || ret == -EOPNOTSUPP) {
			ret = u4r_queue_deferred_ack(qp, dest_qp, psn,
						     cpu_to_be32(status));
			deferred = !ret;
		}
	} else {
		ret = usb4_rdma_data_send(qp->rail, U4_OP_SEND_ACK, qp->base.qp_num,
					  dest_qp, psn, U4_F_LAST,
					  cpu_to_be32(status), 0, 0,
					  NULL, NULL, 0);
	}
	if (READ_ONCE(send_ack_debug))
		pr_info("send_ack tx: qp=%u dest=%u psn=%u status=%u ret=%d deferred=%d\n",
			qp->base.qp_num, dest_qp, psn, status, ret, deferred);
	if (ret && ret != -ENOTCONN && ret != -ESHUTDOWN)
		pr_warn_ratelimited("send_ack tx[qp=%u]: dest=%u psn=%u status=%u failed %d\n",
				    qp->base.qp_num, dest_qp, psn, status, ret);
}

static void u4r_rx_send_handler(struct usb4_rdma_qp *qp,
				const struct u4_wire_hdr *hdr,
				const void *payload, u32 length)
{
	struct usb4_rdma_pd *pd =
		container_of(qp->base.pd, struct usb4_rdma_pd, base);
	struct usb4_rdma_cq *recv_cq =
		container_of(qp->base.recv_cq, struct usb4_rdma_cq, base);
	struct usb4_rdma_recv_wr *r;
	unsigned long flags;
	struct ib_wc wc = {};
	bool last = !!(hdr->flags & U4_F_LAST);
	bool send_ack = false;
	int sret = 0;

	spin_lock_irqsave(&qp->recv_lock, flags);
	if (!qp->in_progress_recv) {
		r = u4r_pop_recv_wr_locked(qp);
		if (r) {
			qp->in_progress_recv = r;
			qp->recv_byte_offset = 0;
			qp->recv_truncated = false;
		}
	}
	r = qp->in_progress_recv;
	spin_unlock_irqrestore(&qp->recv_lock, flags);

	if (!r) {
		pr_warn_ratelimited("rx[qp=%u]: no pending recv WR, dropping %u bytes\n",
				    qp->base.qp_num, length);
		if (last && (hdr->flags & U4_F_SOLICITED))
			u4r_send_send_ack(qp, hdr, IB_WC_RNR_RETRY_EXC_ERR);
		return;
	}

	if (length && !qp->recv_truncated) {
		sret = u4r_recv_scatter(pd, r, qp->recv_byte_offset,
					payload, length);
		if (sret == -ERANGE) {
			qp->recv_truncated = true;
		} else if (sret == 0) {
			qp->recv_byte_offset += length;
		}
		/* Other errors (lkey, MR-fault) are fatal for this WR;
		 * we still drain remaining fragments via in_progress_recv
		 * so the next WR isn't contaminated. */
	}

	if (!last)
		return;

	/* End of message — complete the WR. */
	if (sret < 0 && sret != -ERANGE)
		wc.status = IB_WC_LOC_PROT_ERR;
	else if (qp->recv_truncated)
		wc.status = IB_WC_LOC_LEN_ERR;
	else
		wc.status = IB_WC_SUCCESS;

	wc.wr_id    = r->wr_id;
	wc.wr_cqe   = r->wr_cqe;
	wc.opcode   = IB_WC_RECV;
	wc.qp       = &qp->base;
	wc.byte_len = qp->recv_byte_offset;
	wc.src_qp   = le32_to_cpu(hdr->src_qp);

	spin_lock_irqsave(&qp->recv_lock, flags);
	qp->in_progress_recv = NULL;
	qp->recv_byte_offset = 0;
	qp->recv_truncated = false;
	send_ack = !!(hdr->flags & U4_F_SOLICITED);
	if (READ_ONCE(send_ack_debug) && (hdr->flags & U4_F_SOLICITED))
		pr_info("send_ack rx: qp=%u src=%u psn=%u status=%u recv_q_empty=%d posted=%u advertised=%u\n",
			qp->base.qp_num, le32_to_cpu(hdr->src_qp),
			le32_to_cpu(hdr->psn), wc.status,
			list_empty(&qp->recv_q), qp->recv_posted_count,
			qp->recv_credits_advertised);
	spin_unlock_irqrestore(&qp->recv_lock, flags);

	u4r_cq_push_wc(recv_cq, &wc);
	if (send_ack)
		u4r_send_send_ack(qp, hdr, wc.status);
	kfree(r);
}

static void u4r_rx_rdma_write_handler(struct usb4_rdma_qp *qp,
				      const struct u4_wire_hdr *hdr,
				      const void *payload, u32 length)
{
	struct usb4_rdma_pd *pd =
		container_of(qp->base.pd, struct usb4_rdma_pd, base);
	struct usb4_rdma_cq *recv_cq =
		container_of(qp->base.recv_cq, struct usb4_rdma_cq, base);
	struct usb4_rdma_mr *mr;
	u64 remote_addr = le64_to_cpu(hdr->remote_addr);
	u32 rkey = le32_to_cpu(hdr->rkey);
	bool last = !!(hdr->flags & U4_F_LAST);
	int ret = 0;

	if (length) {
		mr = u4r_pd_get_mr_by_rkey(pd, rkey);
		if (!mr || !(mr->access_flags & IB_ACCESS_REMOTE_WRITE)) {
			pr_warn_ratelimited("rdma_write[qp=%u]: bad rkey 0x%x\n",
					    qp->base.qp_num, rkey);
			if (mr)
				u4r_mr_put(mr);
			return;
		}
		ret = u4r_mr_xfer(mr, remote_addr, (void *)payload, length, false);
		u4r_mr_put(mr);
		if (ret) {
			pr_warn_ratelimited("rdma_write[qp=%u]: xfer failed %d addr=0x%llx len=%u rkey=0x%x\n",
					    qp->base.qp_num, ret, remote_addr,
					    length, rkey);
			return;
		}
	}

	if (hdr->opcode == U4_OP_RDMA_WRITE_WITH_IMM && last) {
		struct usb4_rdma_recv_wr *r;
		struct ib_wc wc = {};
		unsigned long flags;

		spin_lock_irqsave(&qp->recv_lock, flags);
		r = u4r_pop_recv_wr_locked(qp);
		spin_unlock_irqrestore(&qp->recv_lock, flags);

		if (!r) {
			pr_warn_ratelimited("rdma_write_imm[qp=%u]: no pending recv WR\n",
					    qp->base.qp_num);
			return;
		}

		wc.wr_id = r->wr_id;
		wc.wr_cqe = r->wr_cqe;
		wc.status = IB_WC_SUCCESS;
		wc.opcode = IB_WC_RECV_RDMA_WITH_IMM;
		wc.qp = &qp->base;
		wc.byte_len = 0;
		wc.ex.imm_data = cpu_to_be32(le32_to_cpu(hdr->imm_data));
		wc.wc_flags = IB_WC_WITH_IMM;
		wc.src_qp = le32_to_cpu(hdr->src_qp);
		u4r_cq_push_wc(recv_cq, &wc);
		kfree(r);
	}
}

struct u4r_zc_recv_ctx {
	struct usb4_rdma_mr *mr;
	u64 addr;
	u32 remaining;
};

static int u4r_zc_next_recv_page(void *opaque, struct page **page,
				 u32 *page_off, u32 *length,
				 usb4_rdma_data_done_fn *done,
				 void **done_ctx)
{
	struct u4r_zc_recv_ctx *ctx = opaque;
	u32 page_idx;
	int ret;

	ret = u4r_mr_page_chunk(ctx->mr, ctx->addr, ctx->remaining, page,
				&page_idx, page_off, length);
	if (ret)
		return ret;

	ctx->addr += *length;
	ctx->remaining -= *length;
	refcount_inc(&ctx->mr->refs);
	*done = u4r_mr_put_done;
	*done_ctx = ctx->mr;
	return 0;
}

static void u4r_zc_recv_finish(void *opaque)
{
	struct u4r_zc_recv_ctx *ctx = opaque;

	u4r_mr_put(ctx->mr);
	kfree(ctx);
}

static int u4r_rx_zcopy_prepare(void *qp_opaque, const struct u4_wire_hdr *hdr,
				u32 total_length,
				usb4_rdma_data_rx_next_page_fn *next,
				void **next_ctx,
				usb4_rdma_data_rx_finish_fn *finish)
{
	struct usb4_rdma_qp *qp = qp_opaque;
	struct usb4_rdma_pd *pd =
		container_of(qp->base.pd, struct usb4_rdma_pd, base);
	struct u4r_zc_recv_ctx *ctx;
	struct usb4_rdma_mr *mr;
	u64 remote_addr = le64_to_cpu(hdr->remote_addr);
	u32 rkey = le32_to_cpu(hdr->rkey);
	int ret;

	if (hdr->opcode != U4_OP_RDMA_WRITE || !total_length)
		return -EOPNOTSUPP;

	mr = u4r_pd_get_mr_by_rkey(pd, rkey);
	if (!mr || !(mr->access_flags & IB_ACCESS_REMOTE_WRITE)) {
		if (mr)
			u4r_mr_put(mr);
		return -EACCES;
	}

	ret = u4r_mr_check_range(mr, remote_addr, total_length);
	if (ret) {
		u4r_mr_put(mr);
		return ret;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_ATOMIC);
	if (!ctx) {
		u4r_mr_put(mr);
		return -ENOMEM;
	}
	ctx->mr = mr;
	ctx->addr = remote_addr;
	ctx->remaining = total_length;
	*next = u4r_zc_next_recv_page;
	*next_ctx = ctx;
	*finish = u4r_zc_recv_finish;
	return 0;
}

struct u4r_read_req_work {
	struct work_struct work;
	struct usb4_rdma_qp *qp;
	u64 remote_addr;
	u32 rkey;
	u32 read_len;
	u32 dest_qp;
	u32 req_psn;
};

struct u4r_read_stream_ctx {
	struct usb4_rdma_mr *mr;
	u64 addr;
	u32 remaining;
};

static int u4r_send_read_status_locked(struct usb4_rdma_qp *qp, u32 dest_qp,
				       u32 req_psn,
				       enum ib_wc_status status)
{
	return usb4_rdma_data_send(qp->rail, U4_OP_RDMA_READ_RESP, qp->base.qp_num,
				   dest_qp, req_psn, U4_F_LAST,
				   cpu_to_be32(status), 0, 0, NULL, NULL, 0);
}

static int u4r_next_read_page(void *opaque, struct page **page,
			      u32 *page_idx_out, u32 *page_off, u32 *length,
			      usb4_rdma_data_dma_resolve_fn *resolve,
			      void **resolve_ctx,
			      usb4_rdma_data_done_fn *done,
			      void **done_ctx)
{
	struct u4r_read_stream_ctx *ctx = opaque;
	u32 page_idx;
	int ret;

	ret = u4r_mr_page_chunk(ctx->mr, ctx->addr, ctx->remaining, page,
				&page_idx, page_off, length);
	if (ret)
		return ret;
	ctx->addr += *length;
	ctx->remaining -= *length;
	refcount_inc(&ctx->mr->refs);
	*page_idx_out = page_idx;
	*resolve = u4r_mr_resolve_dma;
	*resolve_ctx = ctx->mr;
	*done = u4r_mr_put_done;
	*done_ctx = ctx->mr;
	return 0;
}

static void u4r_rdma_read_req_work(struct work_struct *work)
{
	struct u4r_read_req_work *rw =
		container_of(work, struct u4r_read_req_work, work);
	struct usb4_rdma_qp *qp = rw->qp;
	struct usb4_rdma_pd *pd =
		container_of(qp->base.pd, struct usb4_rdma_pd, base);
	struct usb4_rdma_mr *mr;
	struct u4r_read_stream_ctx stream_ctx;
	enum ib_wc_status err_status = IB_WC_SUCCESS;
	int ret = 0;

	if (u4r_warn_process_path_from_atomic(__func__))
		goto out_free;

	if (rw->read_len && rw->remote_addr > U64_MAX - rw->read_len) {
		err_status = IB_WC_REM_INV_REQ_ERR;
		goto send_error;
	}

	mr = u4r_pd_get_mr_by_rkey(pd, rw->rkey);
	if (!mr || !(mr->access_flags & IB_ACCESS_REMOTE_READ)) {
		pr_warn_ratelimited("rdma_read_req[qp=%u]: bad rkey 0x%x\n",
				    qp->base.qp_num, rw->rkey);
		if (mr)
			u4r_mr_put(mr);
		err_status = IB_WC_REM_ACCESS_ERR;
		goto send_error;
	}
	if (u4r_mr_check_range(mr, rw->remote_addr, rw->read_len)) {
		pr_warn_ratelimited("rdma_read_req[qp=%u]: bad range addr=0x%llx len=%u rkey=0x%x\n",
				    qp->base.qp_num, rw->remote_addr,
				    rw->read_len, rw->rkey);
		u4r_mr_put(mr);
		err_status = IB_WC_REM_ACCESS_ERR;
		goto send_error;
	}
	stream_ctx.mr = mr;
	stream_ctx.addr = rw->remote_addr;
	stream_ctx.remaining = rw->read_len;
	mutex_lock(&qp->send_lock);
	if (!rw->read_len) {
		ret = u4r_send_read_status_locked(qp, rw->dest_qp,
						  rw->req_psn,
						  IB_WC_SUCCESS);
		goto out;
	}

	ret = usb4_rdma_data_send_page_stream(qp->rail, U4_OP_RDMA_READ_RESP,
					      qp->base.qp_num, rw->dest_qp,
					      rw->req_psn, U4_F_LAST,
					      cpu_to_be32(IB_WC_SUCCESS),
					      0, 0, rw->read_len,
					      u4r_next_read_page,
					      &stream_ctx);

out:
	mutex_unlock(&qp->send_lock);
	if (ret)
		pr_warn_ratelimited("rdma_read_req[qp=%u]: response send failed %d\n",
				    qp->base.qp_num, ret);
	u4r_mr_put(mr);
	u4r_qp_put(qp);
	kfree(rw);
	return;

send_error:
	mutex_lock(&qp->send_lock);
	ret = u4r_send_read_status_locked(qp, rw->dest_qp, rw->req_psn,
					  err_status);
	mutex_unlock(&qp->send_lock);
	if (ret)
		pr_warn_ratelimited("rdma_read_req[qp=%u]: error response failed %d\n",
				    qp->base.qp_num, ret);
out_free:
	u4r_qp_put(qp);
	kfree(rw);
}

static void u4r_rx_rdma_read_req_handler(struct usb4_rdma_qp *qp,
					 const struct u4_wire_hdr *hdr)
{
	struct u4r_read_req_work *rw;

	rw = kzalloc(sizeof(*rw), GFP_ATOMIC);
	if (!rw) {
		pr_warn_ratelimited("rdma_read_req[qp=%u]: work alloc failed\n",
				    qp->base.qp_num);
		return;
	}

	u4r_qp_get(qp);
	INIT_WORK(&rw->work, u4r_rdma_read_req_work);
	rw->qp = qp;
	rw->remote_addr = le64_to_cpu(hdr->remote_addr);
	rw->rkey = le32_to_cpu(hdr->rkey);
	rw->read_len = le32_to_cpu(hdr->imm_data);
	rw->dest_qp = le32_to_cpu(hdr->src_qp);
	rw->req_psn = le32_to_cpu(hdr->psn);
	queue_work(system_unbound_wq, &rw->work);
}

static enum ib_wc_status u4r_read_wire_status(u32 status)
{
	switch (status) {
	case IB_WC_SUCCESS:
	case IB_WC_LOC_LEN_ERR:
	case IB_WC_RNR_RETRY_EXC_ERR:
	case IB_WC_REM_INV_REQ_ERR:
	case IB_WC_REM_ACCESS_ERR:
	case IB_WC_REM_OP_ERR:
		return status;
	default:
		return IB_WC_GENERAL_ERR;
	}
}

static void u4r_rx_send_ack_handler(struct usb4_rdma_qp *qp,
				    const struct u4_wire_hdr *hdr)
{
	struct usb4_rdma_cq *send_cq =
		container_of(qp->base.send_cq, struct usb4_rdma_cq, base);
	struct usb4_rdma_send_ctx *ctx;
	unsigned long flags;
	struct ib_wc wc = {};
	u32 ack_psn = le32_to_cpu(hdr->psn);
	enum ib_wc_status status =
		u4r_read_wire_status(le32_to_cpu(hdr->imm_data));

	spin_lock_irqsave(&qp->send_comp_lock, flags);
	ctx = u4r_find_send_ctx_locked(qp, ack_psn);
	if (!ctx) {
		spin_unlock_irqrestore(&qp->send_comp_lock, flags);
		pr_warn_ratelimited("send_ack[qp=%u]: no pending send psn=%u\n",
				    qp->base.qp_num, ack_psn);
		return;
	}
	if (READ_ONCE(send_ack_debug))
		pr_info("send_ack rx_ack: qp=%u src=%u psn=%u status=%u\n",
			qp->base.qp_num, le32_to_cpu(hdr->src_qp), ack_psn,
			status);
	list_del_init(&ctx->list);
	wc.wr_id = ctx->wr_id;
	wc.wr_cqe = ctx->wr_cqe;
	wc.status = status;
	wc.opcode = ctx->opcode;
	wc.qp = &qp->base;
	spin_unlock_irqrestore(&qp->send_comp_lock, flags);

	u4r_cq_push_wc(send_cq, &wc);
	kfree(ctx);
}

static void u4r_rx_recv_credit_handler(struct usb4_rdma_qp *qp,
				       const struct u4_wire_hdr *hdr)
{
	u32 credits = le32_to_cpu(hdr->imm_data);

	if (!credits)
		return;
	atomic_add(credits, &qp->remote_recv_credits);
	atomic_set(&qp->remote_recv_credit_state, U4_RECV_CREDIT_SEEN);
	wake_up(&qp->send_credit_wait);
	if (READ_ONCE(send_ack_debug))
		pr_info("recv_credit rx: qp=%u src=%u credits=%u total=%d\n",
			qp->base.qp_num, le32_to_cpu(hdr->src_qp), credits,
			atomic_read(&qp->remote_recv_credits));
}

static void u4r_rx_rdma_read_resp_handler(struct usb4_rdma_qp *qp,
					  const struct u4_wire_hdr *hdr,
					  const void *payload, u32 length)
{
	struct usb4_rdma_pd *pd =
		container_of(qp->base.pd, struct usb4_rdma_pd, base);
	struct usb4_rdma_cq *send_cq =
		container_of(qp->base.send_cq, struct usb4_rdma_cq, base);
	struct usb4_rdma_recv_wr scatter = {};
	struct usb4_rdma_read_ctx *ctx;
	unsigned long flags;
	struct ib_wc wc = {};
	bool last = !!(hdr->flags & U4_F_LAST);
	enum ib_wc_status status = IB_WC_SUCCESS;
	bool push_cqe = false;
	u32 wire_status = le32_to_cpu(hdr->imm_data);
	u32 resp_psn = le32_to_cpu(hdr->psn);
	u64 resp_off64 = le64_to_cpu(hdr->remote_addr);
	u32 resp_off = 0;
	int ret = 0;

	spin_lock_irqsave(&qp->read_lock, flags);
	ctx = u4r_find_read_ctx_locked(qp, resp_psn);
	if (!ctx) {
		spin_unlock_irqrestore(&qp->read_lock, flags);
		pr_warn_ratelimited("rdma_read_resp[qp=%u]: no pending read psn=%u, dropping %u bytes\n",
				    qp->base.qp_num, resp_psn, length);
		return;
	}

	if (last)
		ctx->seen_last = true;

	if (wire_status) {
		ctx->failed = true;
		ctx->status = u4r_read_wire_status(wire_status);
	}

	if (length && !ctx->failed) {
		if (resp_off64 > U32_MAX || resp_off64 > ctx->total_len ||
		    length > ctx->total_len - (u32)resp_off64) {
			ctx->failed = true;
			ctx->status = IB_WC_LOC_LEN_ERR;
		} else {
			resp_off = resp_off64;
			scatter.num_sge = ctx->num_sge;
			memcpy(scatter.sge, ctx->sge, sizeof(ctx->sge));
			ret = u4r_recv_scatter(pd, &scatter, resp_off,
					       payload, length);
			if (ret == -ERANGE) {
				ctx->failed = true;
				ctx->status = IB_WC_LOC_LEN_ERR;
			} else if (ret) {
				ctx->failed = true;
				ctx->status = IB_WC_LOC_PROT_ERR;
			} else {
				ctx->done += length;
			}
		}
	}

	if ((ctx->failed && !ctx->seen_last) ||
	    (!ctx->failed && ctx->done < ctx->total_len)) {
		spin_unlock_irqrestore(&qp->read_lock, flags);
		return;
	}

	list_del_init(&ctx->list);
	qp->read_depth--;

	if (ctx->failed)
		status = ctx->status;
	else if (ctx->done != ctx->total_len)
		status = IB_WC_LOC_LEN_ERR;

	push_cqe = ctx->signaled || status != IB_WC_SUCCESS;
	if (push_cqe) {
		wc.status = status;
		wc.wr_id = ctx->wr_id;
		wc.wr_cqe = ctx->wr_cqe;
		wc.opcode = IB_WC_RDMA_READ;
		wc.qp = &qp->base;
		wc.byte_len = min(ctx->done, ctx->total_len);
	}
	spin_unlock_irqrestore(&qp->read_lock, flags);

	if (push_cqe)
		u4r_cq_push_wc(send_cq, &wc);
	kfree(ctx);
}

static void u4r_rx_handler(void *qp_opaque, const struct u4_wire_hdr *hdr,
			   const void *payload, u32 length)
{
	struct usb4_rdma_qp *qp = qp_opaque;

	switch (hdr->opcode) {
	case U4_OP_SEND:
		if (qp->qp_type == IB_QPT_GSI)
			u4r_rx_gsi_handler(qp, hdr, payload, length);
		else
			u4r_rx_send_handler(qp, hdr, payload, length);
		break;
	case U4_OP_SEND_ACK:
		u4r_rx_send_ack_handler(qp, hdr);
		break;
	case U4_OP_RECV_CREDIT:
		u4r_rx_recv_credit_handler(qp, hdr);
		break;
	case U4_OP_RDMA_WRITE:
	case U4_OP_RDMA_WRITE_WITH_IMM:
		u4r_rx_rdma_write_handler(qp, hdr, payload, length);
		break;
	case U4_OP_RDMA_READ_REQ:
		u4r_rx_rdma_read_req_handler(qp, hdr);
		break;
	case U4_OP_RDMA_READ_RESP:
		u4r_rx_rdma_read_resp_handler(qp, hdr, payload, length);
		break;
	default:
		pr_warn_ratelimited("rx[qp=%u]: unsupported opcode %u\n",
				    qp->base.qp_num, hdr->opcode);
		break;
	}
}

/* ----- query_* (unchanged from skeleton) -------------------------- */

static int u4r_query_device(struct ib_device *ibdev,
			    struct ib_device_attr *attr,
			    struct ib_udata *udata)
{
	memset(attr, 0, sizeof(*attr));
	attr->vendor_id           = 0x1022;
	attr->vendor_part_id      = 0x158d;
	attr->hw_ver              = 1;
	attr->fw_ver              = 0;
	attr->sys_image_guid      = ibdev->node_guid;
	attr->device_cap_flags    = IB_DEVICE_CHANGE_PHY_PORT;
	attr->kernel_cap_flags    = IBK_LOCAL_DMA_LKEY;
	attr->max_mr_size         = ~0ull;
	attr->page_size_cap       = USB4_RDMA_PAGE_SIZE_CAP;
	attr->max_qp              = 256;
	attr->max_qp_wr           = 1024;
	attr->max_send_sge        = U4_MAX_SGE;
	attr->max_recv_sge        = U4_MAX_SGE;
	attr->max_sge_rd          = U4_MAX_SGE;
	attr->max_cq              = 256;
	attr->max_cqe             = 4096;
	attr->max_mr              = 1024;
	attr->max_pd              = 256;
	attr->max_ah              = 1024;
	attr->max_qp_rd_atom      = U4_MAX_READ_CTX;
	attr->max_res_rd_atom     = 256;
	attr->max_qp_init_rd_atom = U4_MAX_READ_CTX;
	attr->atomic_cap          = IB_ATOMIC_NONE;
	attr->max_pkeys           = 1;
	attr->local_ca_ack_delay  = 15;
	return 0;
}

static int u4r_query_port(struct ib_device *ibdev, u32 port_num,
			  struct ib_port_attr *attr)
{
	struct usb4_rdma_ib_dev *u4r =
		container_of(ibdev, struct usb4_rdma_ib_dev, base);
	bool active;

	if (port_num != 1)
		return -EINVAL;
	memset(attr, 0, sizeof(*attr));
	active = atomic_read(&u4r->active_peers) > 0;
	attr->state          = active ? IB_PORT_ACTIVE : IB_PORT_DOWN;
	attr->phys_state     = active
				? IB_PORT_PHYS_STATE_LINK_UP
				: IB_PORT_PHYS_STATE_DISABLED;
	attr->max_mtu        = IB_MTU_4096;
	attr->active_mtu     = IB_MTU_4096;
	attr->max_msg_sz     = U4_MAX_MSG_SIZE;
	/* Hold link-local v1 + per-IP v2 GIDs (×2 for v4 and v6 per IP).
	 * Kernel populates from add_gid(); we only need a sane upper bound. */
	attr->gid_tbl_len    = 32;
	attr->pkey_tbl_len   = 1;
	attr->max_vl_num     = 1;
	attr->active_width   = IB_WIDTH_4X;
	attr->active_speed   = IB_SPEED_FDR10;
	return 0;
}

static int u4r_get_port_immutable(struct ib_device *ibdev, u32 port_num,
				  struct ib_port_immutable *immutable)
{
	struct ib_port_attr attr;
	int err = u4r_query_port(ibdev, port_num, &attr);

	if (err)
		return err;
	immutable->pkey_tbl_len = attr.pkey_tbl_len;
	immutable->gid_tbl_len  = attr.gid_tbl_len;
	/* Declare RoCEv2: closest match to "RDMA over Ethernet-class
	 * fabric with explicit IP-routed CM". The kernel has no
	 * RDMA_CORE_PORT_USB4 (yet), and we do behave like RoCEv2 to
	 * userspace — IP→GID resolution via netdev, RDMA-CM does the
	 * out-of-band handshake, our wire format takes over for data.
	 * The on-the-wire bytes are NOT actually UDP/IP encapsulated;
	 * that's an implementation detail libfabric/RCCL/MPI don't see. */
	/* RoCEv2 only (UDP-encap), matching RXE's choice. Earlier we also
	 * advertised RoCEv1 but that confused at least one userspace
	 * (Apple's JACCL hardcodes sgid_index=1, expecting that to resolve
	 * to a v2 GID, and our v1+v2 dual advertisement breaks the AH
	 * validation in the kernel core). */
	immutable->core_cap_flags = RDMA_CORE_PORT_IBA_ROCE_UDP_ENCAP;
	immutable->max_mad_size = IB_MGMT_MAD_SIZE;
	return 0;
}

static int u4r_query_pkey(struct ib_device *ibdev, u32 port, u16 index,
			  u16 *pkey)
{
	if (index > 0)
		return -EINVAL;
	*pkey = 0xffff;
	return 0;
}

/* The RoCE GID cache (rdma/ib_cache + roce_gid_mgmt.c) maintains the
 * authoritative GID table by listening to inet/inet6 events on our
 * netdev. Drivers that participate in that flow don't implement
 * query_gid themselves — the core does it. We omit the op entry from
 * the ops table so the core's default kicks in. */

static enum rdma_link_layer u4r_get_link_layer(struct ib_device *ibdev,
					       u32 port_num)
{
	return IB_LINK_LAYER_ETHERNET;
}

/* ----- netdev / GID plumbing -------------------------------------- */

/* Hand the netdev back to the RDMA core so RDMA-CM can resolve
 * source addrs against it. The core takes a reference; we just
 * pin our shared netdev pointer and bump it. */
static struct net_device *u4r_get_netdev(struct ib_device *ibdev, u32 port_num)
{
	struct usb4_rdma_ib_dev *u4r =
		container_of(ibdev, struct usb4_rdma_ib_dev, base);
	struct net_device *ndev;

	if (port_num != 1)
		return NULL;
	ndev = u4r->netdev;
	if (ndev)
		dev_hold(ndev);
	return ndev;
}

/* The RoCE GID cache calls these whenever an IP is added/removed on
 * our netdev. The kernel itself maintains the GID table that
 * query_gid serves; our jobs are (a) accept the call so the caller
 * doesn't error out, and (b) eventually plumb gid → peer routing
 * for multi-cable. For now: no-op, single-peer routing ignores GID. */
static int u4r_add_gid(const struct ib_gid_attr *attr, void **context)
{
	pr_info("add_gid: idx=%u type=%u\n",
		attr->index, attr->gid_type);
	return 0;
}

static int u4r_del_gid(const struct ib_gid_attr *attr, void **context)
{
	pr_info("del_gid: idx=%u\n", attr->index);
	return 0;
}

/* ----- ops table -------------------------------------------------- */

static const struct ib_device_ops u4r_dev_ops = {
	.owner             = THIS_MODULE,
	.driver_id         = RDMA_DRIVER_UNKNOWN,
	.uverbs_abi_ver    = USB4_RDMA_UVERBS_ABI,
	/* No RDMA_DRIVER_USB4_RDMA enum value upstream yet, so we bind
	 * via VERBS_NAME_MATCH on the userspace side. Tell uverbs not to
	 * enforce a driver-id match on ioctl headers (otherwise the
	 * dispatcher rejects our post_recv/post_send ioctls with
	 * EOPNOTSUPP because the userspace ABI version mismatches a
	 * generic driver_id=UNKNOWN binding). */
	.uverbs_no_driver_id_binding = 1,

	.query_device      = u4r_query_device,
	.query_port        = u4r_query_port,
	.query_pkey        = u4r_query_pkey,
	.add_gid           = u4r_add_gid,
	.del_gid           = u4r_del_gid,
	.get_netdev        = u4r_get_netdev,
	.get_port_immutable= u4r_get_port_immutable,
	.get_link_layer    = u4r_get_link_layer,

	.alloc_ucontext    = u4r_alloc_ucontext,
	.dealloc_ucontext  = u4r_dealloc_ucontext,

	.alloc_pd          = u4r_alloc_pd,
	.dealloc_pd        = u4r_dealloc_pd,
	.create_qp         = u4r_create_qp,
	.destroy_qp        = u4r_destroy_qp,
	.modify_qp         = u4r_modify_qp,
	.query_qp          = u4r_query_qp,
	.create_ah         = u4r_create_ah,
	.create_user_ah    = u4r_create_ah,
	.query_ah          = u4r_query_ah,
	.destroy_ah        = u4r_destroy_ah,
	.create_cq         = u4r_create_cq,
	.destroy_cq        = u4r_destroy_cq,
	.post_send         = u4r_post_send,
	.post_recv         = u4r_post_recv,
	.poll_cq           = u4r_poll_cq,
	.req_notify_cq     = u4r_req_notify_cq,
	.reg_user_mr       = u4r_reg_user_mr,
	.dereg_mr          = u4r_dereg_mr,
	.get_dma_mr        = u4r_get_dma_mr,

	INIT_RDMA_OBJ_SIZE(ib_ucontext, usb4_rdma_ucontext, base),
	INIT_RDMA_OBJ_SIZE(ib_ah,       usb4_rdma_ah,       base),
	INIT_RDMA_OBJ_SIZE(ib_pd,        usb4_rdma_pd,       base),
	INIT_RDMA_OBJ_SIZE(ib_cq,        usb4_rdma_cq,       base),
	INIT_RDMA_OBJ_SIZE(ib_qp,        usb4_rdma_qp,       base),
};

static int u4r_register_ibdev(struct u4_data_peer *rail, bool active,
			      bool list_device,
			      struct usb4_rdma_ib_dev **out)
{
	struct usb4_rdma_ib_dev *u4r;
	char name[IB_DEVICE_NAME_MAX];
	u8 mac[ETH_ALEN];
	int err;

	if (rail && !usb4_rdma_data_rail_get(rail))
		return -ENOTCONN;

	u4r = ib_alloc_device(usb4_rdma_ib_dev, base);
	if (!u4r) {
		usb4_rdma_data_rail_put(rail);
		return -ENOMEM;
	}
	INIT_LIST_HEAD(&u4r->dev_link);
	u4r->rail = rail;
	atomic_set(&u4r->active_peers, active ? 1 : 0);
	u4r->base.phys_port_cnt    = USB4_RDMA_NPORTS;
	u4r->base.num_comp_vectors = num_possible_cpus();
	u4r->base.local_dma_lkey   = 0;
	/* RDMA_NODE_RNIC was iWARP territory and libfabric classifies us as
	 * iWARP if we say that — even though we don't speak MPA/TCP. We're
	 * fundamentally RoCE-shaped (IB transport over Ethernet-class link
	 * layer + IP-based addressing), so present as IB_CA. */
	u4r->base.node_type        = RDMA_NODE_IB_CA;
	/* The default uverbs_cmd_mask in _ib_alloc_device doesn't include
	 * POST_SEND/POST_RECV/POLL_CQ/REQ_NOTIFY_CQ — drivers opt in
	 * explicitly. Without these bits, ib_uverbs_run_method returns
	 * EOPNOTSUPP for the corresponding ioctls and our kernel
	 * callback never fires. */
	u4r->base.uverbs_cmd_mask |=
		BIT_ULL(IB_USER_VERBS_CMD_POST_SEND) |
		BIT_ULL(IB_USER_VERBS_CMD_POST_RECV) |
		BIT_ULL(IB_USER_VERBS_CMD_POLL_CQ)   |
		BIT_ULL(IB_USER_VERBS_CMD_REQ_NOTIFY_CQ);

	/* Borrow `thunderbolt0` as the netdev for RDMA-CM control plane.
	 *
	 * Why not our own netdev? RDMA-CM uses real UDP/IP packets (port
	 * 4791 for RoCEv2 GID queries, 4791 for ConnectMgr exchange) for
	 * connection establishment. A pure-stub netdev that drops xmit
	 * leaves CM hung at handshake. thunderbolt0 already has working
	 * IPs end-to-end (assigned by the user, tested at 10 Gbps), and
	 * the CM control plane is small handshake traffic — using it for
	 * that is fine. Our actual data path is tb_ring on a separate
	 * hop and never traverses thunderbolt0. rxe uses the same trick
	 * over the same netdev simultaneously without interference.
	 *
	 * Future: switch to a per-peer usb4r%d netdev once we implement
	 * IP-over-tb_ring (essentially: integrate with `tbnetmq`). The
	 * stub netdev infrastructure lives on in netdev.c for that.
	 *
	 * MAC + node_guid still derived from a random EUI here so the
	 * GID we synthesize is unique per machine even though we share
	 * the netdev with rxe.
	 */
	eth_random_addr(mac);
	addrconf_addr_eui48((u8 *)&u4r->base.node_guid, mac);

	u4r->netdev = dev_get_by_name(&init_net, cm_netdev);
	if (!u4r->netdev) {
		pr_err("CM netdev '%s' not found — bring it up first, or set "
		       "cm_netdev=<iface> at module load\n", cm_netdev);
		ib_dealloc_device(&u4r->base);
		usb4_rdma_data_rail_put(rail);
		return -ENODEV;
	}
	pr_info("CM control plane via netdev %s\n", cm_netdev);

	ib_set_device_ops(&u4r->base, &u4r_dev_ops);

	err = ib_device_set_netdev(&u4r->base, u4r->netdev, 1);
	if (err) {
		pr_err("ib_device_set_netdev failed: %d\n", err);
		dev_put(u4r->netdev);
		ib_dealloc_device(&u4r->base);
		usb4_rdma_data_rail_put(rail);
		return err;
	}

	if (rail) {
		int idx = usb4_rdma_data_rail_index(rail);

		if (idx < 0) {
			pr_err("failed to derive deterministic rail name: %d\n",
			       idx);
			dev_put(u4r->netdev);
			ib_dealloc_device(&u4r->base);
			usb4_rdma_data_rail_put(rail);
			return idx;
		}
		snprintf(name, sizeof(name), "usb4_rdma%d", idx);
	} else {
		strscpy(name, "usb4_rdma%d", sizeof(name));
	}

	err = ib_register_device(&u4r->base, name, NULL);
	if (err) {
		pr_err("ib_register_device failed: %d\n", err);
		dev_put(u4r->netdev);
		ib_dealloc_device(&u4r->base);
		usb4_rdma_data_rail_put(rail);
		return err;
	}

	if (list_device) {
		mutex_lock(&u4r_dev_list_lock);
		list_add_tail(&u4r->dev_link, &u4r_dev_list);
		mutex_unlock(&u4r_dev_list_lock);
	}

	if (out)
		*out = u4r;
	pr_info("registered ib_device %s (1 port%s)\n",
		dev_name(&u4r->base.dev), rail ? ", lane rail" : "");
	return 0;
}

static void u4r_unregister_ibdev(struct usb4_rdma_ib_dev *u4r)
{
	struct net_device *ndev;
	struct u4_data_peer *rail;

	if (!u4r)
		return;

	ndev = u4r->netdev;
	rail = u4r->rail;
	u4r->netdev = NULL;
	u4r->rail = NULL;
	atomic_set(&u4r->active_peers, 0);
	ib_unregister_device(&u4r->base);
	ib_dealloc_device(&u4r->base);
	if (ndev)
		dev_put(ndev);
	usb4_rdma_data_rail_put(rail);
}

static struct usb4_rdma_ib_dev *
u4r_find_rail_dev_locked(struct u4_data_peer *rail)
{
	struct usb4_rdma_ib_dev *u4r;

	list_for_each_entry(u4r, &u4r_dev_list, dev_link) {
		if (u4r->rail == rail)
			return u4r;
	}
	return NULL;
}

void usb4_rdma_ibdev_rail_event(struct u4_data_peer *rail, bool joined)
{
	struct usb4_rdma_ib_dev *u4r;
	int ret;

	if (!u4r_lane_mode() || !rail)
		return;

	if (joined) {
		mutex_lock(&u4r_dev_list_lock);
		u4r = u4r_find_rail_dev_locked(rail);
		mutex_unlock(&u4r_dev_list_lock);
		if (u4r)
			return;
		ret = u4r_register_ibdev(rail, true, true, NULL);
		if (ret)
			pr_warn("lane rail ib_device registration failed: %d\n",
				ret);
		return;
	}

	mutex_lock(&u4r_dev_list_lock);
	u4r = u4r_find_rail_dev_locked(rail);
	if (u4r)
		list_del_init(&u4r->dev_link);
	mutex_unlock(&u4r_dev_list_lock);
	if (u4r)
		u4r_unregister_ibdev(u4r);
}

void usb4_rdma_ibdev_peer_event(bool joined)
{
	int n;

	if (u4r_lane_mode() || !u4r_dev)
		return;
	if (joined) {
		n = atomic_inc_return(&u4r_dev->active_peers);
	} else {
		int old;

		do {
			old = atomic_read(&u4r_dev->active_peers);
			if (old <= 0) {
				atomic_set(&u4r_dev->active_peers, 0);
				pr_warn("peer left while no peers were active; port count clamped at 0\n");
				n = 0;
				goto log;
			}
		} while (atomic_cmpxchg(&u4r_dev->active_peers, old, old - 1) != old);
		n = old - 1;
	}

log:
	pr_info("peer %s, %d active — port 1 %s\n",
		joined ? "joined" : "left", n,
		n > 0 ? "ACTIVE" : "DOWN");
}

int usb4_rdma_ibdev_init(void)
{
	int err;

	err = u4r_parse_rail_mode();
	if (err)
		return err;

	usb4_rdma_data_set_rx_handler(u4r_rx_handler);
	usb4_rdma_data_set_rx_zcopy_prepare(u4r_rx_zcopy_prepare);

	if (u4r_lane_mode()) {
		pr_info("rail_mode=lane: ib_devices will be registered per active data lane\n");
		return 0;
	}

	err = u4r_register_ibdev(NULL, false, false, &u4r_dev);
	if (err) {
		usb4_rdma_data_set_rx_zcopy_prepare(NULL);
		usb4_rdma_data_set_rx_handler(NULL);
		return err;
	}
	return 0;
}

void usb4_rdma_ibdev_exit(void)
{
	struct usb4_rdma_ib_dev *u4r, *tmp;

	usb4_rdma_data_set_rx_zcopy_prepare(NULL);
	usb4_rdma_data_set_rx_handler(NULL);

	mutex_lock(&u4r_dev_list_lock);
	list_for_each_entry_safe(u4r, tmp, &u4r_dev_list, dev_link) {
		list_del_init(&u4r->dev_link);
		mutex_unlock(&u4r_dev_list_lock);
		u4r_unregister_ibdev(u4r);
		mutex_lock(&u4r_dev_list_lock);
	}
	mutex_unlock(&u4r_dev_list_lock);

	u4r_unregister_ibdev(u4r_dev);
	u4r_dev = NULL;
	ida_destroy(&u4r_qpn_ida);
}
