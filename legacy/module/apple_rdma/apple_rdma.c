// SPDX-License-Identifier: GPL-2.0
/*
 * apple_rdma.c - experimental Apple RDMA-over-Thunderbolt verbs peer.
 *
 * This is intentionally separate from the Linux/Linux usb4_rdma module. It
 * claims Apple's AD/FA57 Thunderbolt service, advertises the reciprocal
 * service that macOS requires for rdma_enN PORT_ACTIVE, programs the E2E path
 * shape that made Mac -> Linux SEND complete in capture, and exposes a small
 * UC-only verbs device.
 *
 * First target: let a Mac UC SEND on QPN 0x900 land in a Linux posted RECV WR
 * and produce a normal RECV CQE. Linux -> Mac SEND is also shaped like the
 * captured Apple descriptor stream, but previous raw replay showed that this
 * direction still needs more Apple-specific state.
 */

#define pr_fmt(fmt) "apple_rdma: " fmt

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/highmem.h>
#include <linux/refcount.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/idr.h>
#include <linux/uuid.h>
#include <linux/thunderbolt.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/dma-mapping.h>
#include <linux/crc32.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/string.h>
#include <net/addrconf.h>
#include <net/net_namespace.h>

#include <rdma/ib_verbs.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/uverbs_ioctl.h>
#include <rdma/ib_mad.h>

#define ARDMA_DRV_NAME		"apple_rdma"

#define APPLE_RDMA_PRTCID	0xFA57
#define APPLE_RDMA_PRTCVERS	1
#define APPLE_RDMA_PRTCREVS	0

#define ARDMA_FRAME_SIZE	SZ_4K
#define ARDMA_RING_DEPTH	8192
#define ARDMA_RX_FRAMES		(ARDMA_RING_DEPTH - 64)
#define ARDMA_MAX_SGE		4
#define ARDMA_MAX_CQE		4096
#define ARDMA_MAX_MSG_SIZE	SZ_16M
#define ARDMA_PENDING_RX_BYTES	SZ_512K
#define ARDMA_PENDING_RX_SLOTS	16
#define ARDMA_RX_MARKER_LOG_ENTRIES	64
#define ARDMA_RX_MARKER_PREFIX	16
#define ARDMA_UVERBS_ABI	1
#define ARDMA_QPN_MIN		0x900
#define ARDMA_QPN_MAX		0x00ffffff

#define ARDMA_RX_SOF_MASK	0xffff
#define ARDMA_RX_EOF_MASK	0xffff
#define ARDMA_MAX_CTRL_MSGS	32
#define ARDMA_MAX_CTRL_BYTES	256
#define ARDMA_NHI_REG_TX_RING_BASE	0x00000
#define ARDMA_NHI_REG_RX_RING_BASE	0x08000
#define ARDMA_NHI_REG_TX_OPTIONS_BASE	0x19800
#define ARDMA_NHI_REG_RX_OPTIONS_BASE	0x29800
#define ARDMA_NHI_RING_SLOT_SIZE	16
#define ARDMA_NHI_OPTIONS_SLOT_SIZE	32
#define ARDMA_DESC_FLAGS_SHIFT	20

struct ardma_ring_desc {
	u64 phys;
	u32 meta;
	u32 time;
} __packed;

static_assert(sizeof(struct ardma_ring_desc) == 16);

static const char apple_rdma_key[9] = {
	(char)0xff, (char)0xff, (char)0xff, (char)0xff,
	(char)0xff, (char)0xff, 'A', 'D', '\0',
};

static const char apple_rdma_ca_key[9] = {
	(char)0xff, (char)0xff, (char)0xff, (char)0xff,
	(char)0xff, (char)0xff, 'C', 'A', '\0',
};

static const uuid_t apple_rdma_default_service_uuid =
	UUID_INIT(0x49bf223e, 0xd4aa, 0x44d7,
		  0x87, 0x91, 0x50, 0x44, 0x5a, 0xc5, 0x2d, 0x5e);
static uuid_t apple_rdma_service_uuid =
	UUID_INIT(0x49bf223e, 0xd4aa, 0x44d7,
		  0x87, 0x91, 0x50, 0x44, 0x5a, 0xc5, 0x2d, 0x5e);

static bool advertise_service = true;
module_param(advertise_service, bool, 0444);
MODULE_PARM_DESC(advertise_service,
		 "Advertise reciprocal Apple AD/FA57 service (default: true)");

static char *service_uuid;
module_param(service_uuid, charp, 0444);
MODULE_PARM_DESC(service_uuid,
		 "Override advertised Apple AD/FA57 service UUID");

static int receive_path = 9;
module_param(receive_path, int, 0444);
MODULE_PARM_DESC(receive_path,
		 "Incoming Apple transmit HopID to bind as our RX path (default: 9)");

static bool apple_vendor_only = true;
module_param(apple_vendor_only, bool, 0444);
MODULE_PARM_DESC(apple_vendor_only,
		 "Bind only peers whose xdomain vendor_name is Apple Inc.");

static char *peer_device_name;
module_param(peer_device_name, charp, 0444);
MODULE_PARM_DESC(peer_device_name,
		 "Bind only a peer whose xdomain device_name exactly matches this value");

static char *cm_netdev = "thunderbolt0";
module_param(cm_netdev, charp, 0444);
MODULE_PARM_DESC(cm_netdev,
		 "netdev used by RDMA core for RoCE GID table (default: thunderbolt0)");

static bool tx_enabled;
module_param(tx_enabled, bool, 0644);
MODULE_PARM_DESC(tx_enabled,
		 "Allow experimental Linux -> Mac Apple-shaped SEND descriptors (default: false)");

static bool tx_raw_mode;
module_param(tx_raw_mode, bool, 0444);
MODULE_PARM_DESC(tx_raw_mode,
		 "Use raw NHI TX rings and append Apple's 4-byte end-of-group trailer (default: false)");

static bool tx_e2e = true;
module_param(tx_e2e, bool, 0444);
MODULE_PARM_DESC(tx_e2e,
		 "Enable E2E flow control on TX rings (default: true)");

static bool rx_raw_mode = true;
module_param(rx_raw_mode, bool, 0444);
MODULE_PARM_DESC(rx_raw_mode,
		 "Use raw NHI RX rings and trim Apple's 4-byte end-of-group trailer (default: true)");

enum {
	ARDMA_TX_MARKER_APPLE_EOF = 0,
	ARDMA_TX_MARKER_GEMINI_SOF = 1,
	ARDMA_TX_MARKER_BOTH_START = 2,
	ARDMA_TX_MARKER_EVERY_DESC = 3,
	ARDMA_TX_MARKER_EVERY_SOF_APPLE_EOF = 4,
};

static unsigned int tx_marker_mode = ARDMA_TX_MARKER_APPLE_EOF;
module_param(tx_marker_mode, uint, 0644);
MODULE_PARM_DESC(tx_marker_mode,
		 "TX marker mode: 0=apple_eof, 1=gemini_sof, 2=both_start, 3=every_desc, 4=every_sof_apple_eof (default: 0)");

/* Diagnostic-only pacing knobs. Default 0 (no pacing). When nonzero,
 * ardma_send_apple sleeps tx_pace_us microseconds between 4 KiB blocks
 * - used to test whether the JACCL bracket-transition failure is a
 * software-timing overrun (would be fixed by enabling raw+E2E hardware
 * flow control). Settable at runtime via sysfs. */
static unsigned int tx_pace_us;
module_param(tx_pace_us, uint, 0644);
MODULE_PARM_DESC(tx_pace_us,
		 "Sleep N microseconds between 4 KiB TX blocks in ardma_send_apple (diagnostic; default: 0)");

struct ardma_peer;
struct ardma_qp;

struct ardma_ucontext {
	struct ib_ucontext base;
};

struct ardma_mr {
	struct ib_mr base;
	struct list_head pd_link;
	refcount_t refs;
	wait_queue_head_t ref_wait;
	bool dying;
	u64 user_va;
	u64 length;
	int npages;
	struct page **pages;
};

struct ardma_pd {
	struct ib_pd base;
	struct list_head mrs;
	spinlock_t mr_lock;
};

struct ardma_ah {
	struct ib_ah base;
	struct rdma_ah_attr attr;
};

struct ardma_wc_entry {
	struct list_head list;
	struct ib_wc wc;
};

struct ardma_cq {
	struct ib_cq base;
	spinlock_t lock;
	struct list_head wc_list;
	struct list_head free_list;
	struct ardma_wc_entry *pool;
	int cqe_capacity;
	int wc_count;
	int free_count;
	enum ib_cq_notify_flags notify;
};

struct ardma_recv_wr {
	struct list_head list;
	u64 wr_id;
	struct ib_cqe *wr_cqe;
	int num_sge;
	struct ib_sge sge[ARDMA_MAX_SGE];
};

struct ardma_pending_rx {
	u8 *buf;
	u32 len;
	bool active;
	bool ready;
	bool truncated;
};

struct ardma_qp {
	struct ib_qp base;
	struct ardma_peer *peer;
	enum ib_qp_type qp_type;
	enum ib_qp_state state;
	struct ib_qp_attr attr;
	struct ib_qp_init_attr init_attr;
	int attr_mask;
	refcount_t refs;
	wait_queue_head_t ref_wait;
	bool qpn_allocated;
	bool registered;
	bool sq_sig_all;

	spinlock_t recv_lock;
	struct list_head recv_q;
	struct ardma_recv_wr *rx_wr;
	u32 rx_group_base;
	u32 rx_piece;
	u32 rx_byte_len;
	bool rx_tail_pending;
	bool rx_truncated;
	struct ardma_pending_rx rx_pending[ARDMA_PENDING_RX_SLOTS];
	u8 rx_pending_head;
	u8 rx_pending_tail;
	u8 rx_pending_ready_count;
	int rx_pending_active;

	spinlock_t list_lock;
	struct list_head qps_link;
};

struct ardma_ibdev {
	struct ib_device base;
	struct net_device *netdev;
	atomic_t active_peers;
};

struct ardma_rx_frame {
	struct ring_frame frame;
	struct ardma_peer *peer;
	void *data;
	dma_addr_t dma;
};

struct ardma_send_ctx {
	struct ardma_qp *qp;
	u64 wr_id;
	struct ib_cqe *wr_cqe;
	atomic_t pending;
	atomic_t failed;
	bool signaled;
};

struct ardma_tx_frame {
	struct ring_frame frame;
	struct ardma_peer *peer;
	struct ardma_send_ctx *ctx;
	void *data;
	dma_addr_t dma;
};

struct ardma_rx_marker_log_entry {
	u64 seq;
	u32 len;
	u32 meta;
	u16 flags;
	u8 sof;
	u8 eof;
	u8 prefix_len;
	u8 prefix[ARDMA_RX_MARKER_PREFIX];
};

struct ardma_peer {
	struct tb_service *svc;
	struct tb_xdomain *xd;
	refcount_t refs;
	wait_queue_head_t ref_wait;
	bool closing;

	int local_in_hop;
	int local_out_hop;
	bool paths_enabled;
	struct tb_ring *tx_ring;
	struct tb_ring *rx_ring;
	struct ardma_rx_frame *rx_frames;
	struct mutex tx_lock;
	bool tx_ring_running;

	struct dentry *debugfs_dir;
	spinlock_t rx_marker_lock;
	u64 rx_marker_seq;
	u32 rx_marker_pos;
	struct ardma_rx_marker_log_entry
		rx_marker_log[ARDMA_RX_MARKER_LOG_ENTRIES];
	atomic64_t rx_frame_count;
	atomic64_t rx_eof[4];
	atomic64_t rx_eof_other;
	atomic64_t rx_messages;
	atomic64_t rx_drops;
	atomic64_t rx_bad_shape;
	atomic64_t rx_no_qp;
	atomic64_t tx_frames;
	atomic64_t tx_completions;
	atomic64_t tx_errors;
};

struct ardma_ctrl_log_entry {
	struct list_head list;
	u32 size;
	u32 dump_len;
	u8 dump[ARDMA_MAX_CTRL_BYTES];
};

static struct tb_property_dir *ardma_property_dir;
static struct tb_protocol_handler ardma_protocol_handler;
static struct tb_service_driver ardma_service_driver;
static struct dentry *ardma_debugfs_root;

static DEFINE_MUTEX(ardma_peer_lock);
static struct ardma_peer *ardma_active_peer;
static struct ardma_ibdev *ardma_ibdev;
static DEFINE_IDA(ardma_qpn_ida);
static LIST_HEAD(ardma_qp_list);
static DEFINE_SPINLOCK(ardma_qp_lock);
static atomic_t ardma_ctrl_logged;
static atomic_t ardma_ctrl_received;
static DEFINE_SPINLOCK(ardma_ctrl_lock);
static LIST_HEAD(ardma_ctrl_list);

/* ----- MR helpers ------------------------------------------------ */

static struct ardma_mr *ardma_pd_get_mr(struct ardma_pd *pd, u32 lkey)
{
	struct ardma_mr *mr;
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

static void ardma_mr_put(struct ardma_mr *mr)
{
	if (refcount_dec_and_test(&mr->refs))
		WARN_ON_ONCE(1);
	if (refcount_read(&mr->refs) == 1)
		wake_up(&mr->ref_wait);
}

static int ardma_mr_check_range(struct ardma_mr *mr, u64 vaddr, size_t len)
{
	if (vaddr < mr->user_va || len > mr->length ||
	    vaddr - mr->user_va > mr->length - len)
		return -ERANGE;
	return 0;
}

static int ardma_mr_xfer(struct ardma_mr *mr, u64 vaddr, void *kbuf,
			 size_t len, bool from_mr)
{
	u64 offset, page_idx, page_off;
	size_t copied = 0;
	int ret;

	ret = ardma_mr_check_range(mr, vaddr, len);
	if (ret)
		return ret;

	offset = (mr->user_va & ~PAGE_MASK) + (vaddr - mr->user_va);
	while (copied < len) {
		void *page_kva;
		size_t chunk;

		page_idx = (offset + copied) >> PAGE_SHIFT;
		page_off = (offset + copied) & ~PAGE_MASK;
		if (page_idx >= mr->npages)
			return -ERANGE;

		chunk = min_t(size_t, PAGE_SIZE - page_off, len - copied);
		page_kva = page_address(mr->pages[page_idx]);
		if (!page_kva)
			return -EFAULT;

		if (from_mr)
			memcpy((u8 *)kbuf + copied,
			       (u8 *)page_kva + page_off, chunk);
		else
			memcpy((u8 *)page_kva + page_off,
			       (u8 *)kbuf + copied, chunk);
		copied += chunk;
	}
	return 0;
}

static int ardma_recv_scatter(struct ardma_pd *pd, struct ardma_recv_wr *r,
			      u32 dst_off, const void *payload, u32 len)
{
	u32 cur = 0, copied = 0;
	int i;

	for (i = 0; i < r->num_sge && copied < len; i++) {
		const struct ib_sge *sge = &r->sge[i];
		struct ardma_mr *mr;
		u32 in_sge_off, chunk;

		if (cur + sge->length <= dst_off) {
			cur += sge->length;
			continue;
		}

		in_sge_off = (dst_off + copied) - cur;
		chunk = min_t(u32, sge->length - in_sge_off, len - copied);
		mr = ardma_pd_get_mr(pd, sge->lkey);
		if (!mr)
			return -EINVAL;
		if (ardma_mr_xfer(mr, sge->addr + in_sge_off,
				  (void *)payload + copied, chunk, false)) {
			ardma_mr_put(mr);
			return -EFAULT;
		}
		ardma_mr_put(mr);
		copied += chunk;
	}

	return copied < len ? -ERANGE : 0;
}

static int ardma_copy_sges_to_buf(struct ardma_pd *pd,
				  const struct ib_sge *sg_list, int num_sge,
				  u32 src_off, void *dst, u32 len)
{
	u32 cur = 0, copied = 0;
	int i;

	for (i = 0; i < num_sge && copied < len; i++) {
		const struct ib_sge *sge = &sg_list[i];
		struct ardma_mr *mr;
		u32 in_sge_off, chunk;

		if (cur + sge->length <= src_off) {
			cur += sge->length;
			continue;
		}

		in_sge_off = (src_off + copied) - cur;
		chunk = min_t(u32, sge->length - in_sge_off, len - copied);
		mr = ardma_pd_get_mr(pd, sge->lkey);
		if (!mr)
			return -EINVAL;
		if (ardma_mr_xfer(mr, sge->addr + in_sge_off,
				  (u8 *)dst + copied, chunk, true)) {
			ardma_mr_put(mr);
			return -EFAULT;
		}
		ardma_mr_put(mr);
		copied += chunk;
	}

	return copied < len ? -ERANGE : 0;
}

/* ----- peer/QP refs ---------------------------------------------- */

static struct ardma_peer *ardma_peer_get_active(void)
{
	struct ardma_peer *peer;

	mutex_lock(&ardma_peer_lock);
	peer = ardma_active_peer;
	if (peer && !READ_ONCE(peer->closing))
		refcount_inc(&peer->refs);
	else
		peer = NULL;
	mutex_unlock(&ardma_peer_lock);
	return peer;
}

static void ardma_peer_put(struct ardma_peer *peer)
{
	if (!peer)
		return;
	if (refcount_dec_and_test(&peer->refs))
		WARN_ON_ONCE(1);
	if (refcount_read(&peer->refs) == 1)
		wake_up(&peer->ref_wait);
}

static void ardma_qp_get(struct ardma_qp *qp)
{
	refcount_inc(&qp->refs);
}

static void ardma_qp_put(struct ardma_qp *qp)
{
	if (refcount_dec_and_test(&qp->refs))
		WARN_ON_ONCE(1);
	if (refcount_read(&qp->refs) == 1)
		wake_up(&qp->ref_wait);
}

static void ardma_stop_tx_ring(struct ardma_peer *peer)
{
	if (!peer || !peer->tx_ring)
		return;

	mutex_lock(&peer->tx_lock);
	if (peer->tx_ring_running) {
		tb_ring_stop(peer->tx_ring);
		peer->tx_ring_running = false;
	}
	mutex_unlock(&peer->tx_lock);
}

static struct ardma_qp *ardma_lookup_qp(u32 qpn)
{
	struct ardma_qp *qp;
	unsigned long flags;

	spin_lock_irqsave(&ardma_qp_lock, flags);
	list_for_each_entry(qp, &ardma_qp_list, qps_link) {
		if (qp->base.qp_num == qpn && qp->registered) {
			ardma_qp_get(qp);
			spin_unlock_irqrestore(&ardma_qp_lock, flags);
			return qp;
		}
	}
	spin_unlock_irqrestore(&ardma_qp_lock, flags);
	return NULL;
}

/* ----- CQ helpers ------------------------------------------------ */

static int ardma_cq_push_wc(struct ardma_cq *cq, const struct ib_wc *wc)
{
	struct ardma_wc_entry *e;
	unsigned long flags;

	spin_lock_irqsave(&cq->lock, flags);
	if (list_empty(&cq->free_list)) {
		spin_unlock_irqrestore(&cq->lock, flags);
		return -ENOMEM;
	}
	e = list_first_entry(&cq->free_list, struct ardma_wc_entry, list);
	list_del(&e->list);
	cq->free_count--;
	e->wc = *wc;
	list_add_tail(&e->list, &cq->wc_list);
	cq->wc_count++;
	if (cq->notify) {
		cq->notify = 0;
		spin_unlock_irqrestore(&cq->lock, flags);
		if (cq->base.comp_handler)
			cq->base.comp_handler(&cq->base, cq->base.cq_context);
	} else {
		spin_unlock_irqrestore(&cq->lock, flags);
	}
	return 0;
}

/* ----- Apple RX reassembly --------------------------------------- */

static struct ardma_recv_wr *ardma_pop_recv_locked(struct ardma_qp *qp)
{
	struct ardma_recv_wr *r;

	r = list_first_entry_or_null(&qp->recv_q, struct ardma_recv_wr, list);
	if (!r)
		return NULL;
	list_del(&r->list);
	return r;
}

static int ardma_recv_wr_total_len(const struct ardma_recv_wr *r, u32 *total)
{
	u32 n = 0;
	int i;

	for (i = 0; i < r->num_sge; i++) {
		if (r->sge[i].length > U32_MAX - n)
			return -EMSGSIZE;
		n += r->sge[i].length;
	}
	*total = n;
	return 0;
}

static struct ardma_pending_rx *ardma_pending_active_locked(struct ardma_qp *qp)
{
	struct ardma_pending_rx *p;

	if (qp->rx_pending_active >= 0)
		return &qp->rx_pending[qp->rx_pending_active];
	if (qp->rx_pending_ready_count >= ARDMA_PENDING_RX_SLOTS)
		return NULL;

	p = &qp->rx_pending[qp->rx_pending_tail];
	p->len = 0;
	p->truncated = false;
	p->active = true;
	p->ready = false;
	qp->rx_pending_active = qp->rx_pending_tail;
	return p;
}

static void ardma_pending_finish_locked(struct ardma_qp *qp)
{
	struct ardma_pending_rx *p;

	if (qp->rx_pending_active < 0)
		return;

	p = &qp->rx_pending[qp->rx_pending_active];
	p->active = false;
	p->ready = true;
	qp->rx_pending_active = -1;
	qp->rx_pending_ready_count++;
	qp->rx_pending_tail = (qp->rx_pending_tail + 1) %
			      ARDMA_PENDING_RX_SLOTS;
}

static void ardma_push_rx_wc(struct ardma_qp *qp, struct ardma_recv_wr *r,
			     u32 byte_len, enum ib_wc_status status)
{
	struct ardma_cq *recv_cq =
		container_of(qp->base.recv_cq, struct ardma_cq, base);
	struct ib_wc wc = {};

	wc.wr_id = r->wr_id;
	wc.wr_cqe = r->wr_cqe;
	wc.byte_len = byte_len;
	wc.status = status;
	wc.opcode = IB_WC_RECV;
	wc.qp = &qp->base;
	wc.src_qp = qp->attr.dest_qp_num;
	wc.port_num = 1;
	ardma_cq_push_wc(recv_cq, &wc);
	kfree(r);
}

static struct ardma_recv_wr *
ardma_flush_pending_locked(struct ardma_qp *qp, u32 *byte_len,
			   enum ib_wc_status *status)
{
	struct ardma_pd *pd = container_of(qp->base.pd, struct ardma_pd, base);
	struct ardma_pending_rx *p;
	struct ardma_recv_wr *r;
	u32 expected_len;
	int ret;

	if (!qp->rx_pending_ready_count)
		return NULL;

	r = ardma_pop_recv_locked(qp);
	if (!r)
		return NULL;

	p = &qp->rx_pending[qp->rx_pending_head];
	*byte_len = p->len;
	*status = p->truncated ? IB_WC_LOC_LEN_ERR : IB_WC_SUCCESS;
	if (!ardma_recv_wr_total_len(r, &expected_len) &&
	    p->len != expected_len)
		*status = IB_WC_LOC_LEN_ERR;
	ret = ardma_recv_scatter(pd, r, 0, p->buf, p->len);
	if (ret == -ERANGE)
		*status = IB_WC_LOC_LEN_ERR;
	else if (ret)
		*status = IB_WC_LOC_PROT_ERR;

	p->ready = false;
	p->len = 0;
	p->truncated = false;
	qp->rx_pending_head = (qp->rx_pending_head + 1) %
			      ARDMA_PENDING_RX_SLOTS;
	qp->rx_pending_ready_count--;
	return r;
}

static void ardma_complete_rx_wr(struct ardma_qp *qp, enum ib_wc_status status)
{
	struct ardma_recv_wr *r;
	u32 byte_len;
	unsigned long flags;

	spin_lock_irqsave(&qp->recv_lock, flags);
	r = qp->rx_wr;
	qp->rx_wr = NULL;
	byte_len = qp->rx_byte_len;
	qp->rx_group_base = 0;
	qp->rx_piece = 0;
	qp->rx_byte_len = 0;
	qp->rx_tail_pending = false;
	qp->rx_truncated = false;
	spin_unlock_irqrestore(&qp->recv_lock, flags);

	if (!r)
		return;

	ardma_push_rx_wc(qp, r, byte_len, status);
}

static bool ardma_known_piece(struct ardma_qp *qp, u32 len, u32 *offset,
			      bool *group_done)
{
	u32 piece = qp->rx_piece;

	*group_done = false;

	if (!qp->rx_tail_pending && piece < 16 && len == 252) {
		*offset = piece * 0x100;
		qp->rx_piece++;
		return true;
	}
	if (!qp->rx_tail_pending && piece < 16 && len == 12) {
		*offset = piece * 0x100;
		qp->rx_tail_pending = true;
		return true;
	}
	if (qp->rx_tail_pending && piece < 16 && len == 240) {
		*offset = piece * 0x100 + 0x10;
		*group_done = true;
		qp->rx_tail_pending = false;
		qp->rx_piece = 0;
		return true;
	}
	return false;
}

static void ardma_rx_apple_frame(struct ardma_peer *peer, u32 qpn,
				 const void *payload, u32 len, u8 eof)
{
	struct ardma_qp *qp;
	struct ardma_pd *pd;
	struct ardma_recv_wr *r;
	struct ardma_recv_wr *pending_r = NULL;
	unsigned long flags;
	u32 dst_off, rel_off;
	u32 copy_len = len;
	u32 pending_byte_len = 0;
	bool group_done;
	enum ib_wc_status complete_status = IB_WC_SUCCESS;
	enum ib_wc_status pending_status = IB_WC_SUCCESS;
	int ret;

	qp = ardma_lookup_qp(qpn);
	if (!qp) {
		atomic64_inc(&peer->rx_no_qp);
		return;
	}

	pd = container_of(qp->base.pd, struct ardma_pd, base);

	spin_lock_irqsave(&qp->recv_lock, flags);
	if (!qp->rx_wr) {
		bool buffer_pending = false;

		if (READ_ONCE(rx_raw_mode) && qp->rx_pending_active >= 0) {
			buffer_pending = true;
		} else {
			r = ardma_pop_recv_locked(qp);
			if (!r) {
				if (READ_ONCE(rx_raw_mode))
					buffer_pending = true;
				else {
					spin_unlock_irqrestore(&qp->recv_lock,
							       flags);
					atomic64_inc(&peer->rx_drops);
					ardma_qp_put(qp);
					return;
				}
			}
		}

		if (buffer_pending) {
			struct ardma_pending_rx *p;

			p = ardma_pending_active_locked(qp);
			if (!p) {
				spin_unlock_irqrestore(&qp->recv_lock, flags);
				atomic64_inc(&peer->rx_drops);
				ardma_qp_put(qp);
				return;
			}
			copy_len = len;
			if (eof == 2 || eof == 3) {
				if (len < 4) {
					copy_len = 0;
					p->truncated = true;
					atomic64_inc(&peer->rx_bad_shape);
				} else {
					copy_len = len - 4;
				}
			}
			if (p->len + copy_len > ARDMA_PENDING_RX_BYTES) {
				copy_len = ARDMA_PENDING_RX_BYTES - p->len;
				p->truncated = true;
			}
			memcpy(p->buf + p->len, payload, copy_len);
			p->len += copy_len;
			if (eof == 3) {
				ardma_pending_finish_locked(qp);
				atomic64_inc(&peer->rx_messages);
				pending_r = ardma_flush_pending_locked(qp,
					&pending_byte_len, &pending_status);
			}
			spin_unlock_irqrestore(&qp->recv_lock, flags);
			if (pending_r)
				ardma_push_rx_wc(qp, pending_r,
						 pending_byte_len,
						 pending_status);
			ardma_qp_put(qp);
			return;
		}
		qp->rx_wr = r;
		qp->rx_group_base = 0;
		qp->rx_piece = 0;
		qp->rx_byte_len = 0;
		qp->rx_tail_pending = false;
		qp->rx_truncated = false;
	}

	if (eof == 1) {
		qp->rx_piece = 0;
		qp->rx_tail_pending = false;
	}

	if (READ_ONCE(rx_raw_mode)) {
		dst_off = qp->rx_byte_len;
		if (eof == 2 || eof == 3) {
			if (len < 4) {
				copy_len = 0;
				atomic64_inc(&peer->rx_bad_shape);
				qp->rx_truncated = true;
			} else {
				copy_len = len - 4;
			}
		}
	} else if (ardma_known_piece(qp, len, &rel_off, &group_done)) {
		dst_off = qp->rx_group_base + rel_off;
		if (group_done && eof == 2)
			qp->rx_group_base += SZ_4K;
	} else {
		dst_off = qp->rx_byte_len;
		qp->rx_tail_pending = false;
		atomic64_inc(&peer->rx_bad_shape);
	}
	r = qp->rx_wr;
	spin_unlock_irqrestore(&qp->recv_lock, flags);

	ret = ardma_recv_scatter(pd, r, dst_off, payload, copy_len);

	spin_lock_irqsave(&qp->recv_lock, flags);
	if (ret == -ERANGE)
		qp->rx_truncated = true;
	else if (ret)
		qp->rx_truncated = true;
	qp->rx_byte_len = max(qp->rx_byte_len, dst_off + copy_len);
	if (eof != 3) {
		spin_unlock_irqrestore(&qp->recv_lock, flags);
		ardma_qp_put(qp);
		return;
	}
	if (!ardma_recv_wr_total_len(r, &dst_off)) {
		if (qp->rx_byte_len != dst_off) {
			qp->rx_truncated = true;
			atomic64_inc(&peer->rx_bad_shape);
		}
	} else {
		qp->rx_truncated = true;
		atomic64_inc(&peer->rx_bad_shape);
	}
	if (qp->rx_truncated)
		complete_status = ret && ret != -ERANGE ?
			IB_WC_LOC_PROT_ERR : IB_WC_LOC_LEN_ERR;
	spin_unlock_irqrestore(&qp->recv_lock, flags);

	atomic64_inc(&peer->rx_messages);
	ardma_complete_rx_wr(qp, complete_status);
	ardma_qp_put(qp);
}

static u32 ardma_rx_frame_meta(const struct ring_frame *frame, u32 len)
{
	return (len & 0xfff) |
	       ((frame->eof & 0xf) << 12) |
	       ((frame->sof & 0xf) << 16) |
	       ((frame->flags & 0xfff) << ARDMA_DESC_FLAGS_SHIFT);
}

static void ardma_rx_marker_log(struct ardma_peer *peer,
				const struct ring_frame *frame,
				const void *payload, u32 len)
{
	struct ardma_rx_marker_log_entry *e;
	unsigned long flags;
	u32 pos;

	spin_lock_irqsave(&peer->rx_marker_lock, flags);
	pos = peer->rx_marker_pos;
	e = &peer->rx_marker_log[pos];
	memset(e, 0, sizeof(*e));
	e->seq = ++peer->rx_marker_seq;
	e->len = len;
	e->meta = ardma_rx_frame_meta(frame, len);
	e->flags = frame->flags;
	e->sof = frame->sof;
	e->eof = frame->eof;
	e->prefix_len = min_t(u32, len, ARDMA_RX_MARKER_PREFIX);
	memcpy(e->prefix, payload, e->prefix_len);
	peer->rx_marker_pos = (pos + 1) % ARDMA_RX_MARKER_LOG_ENTRIES;
	spin_unlock_irqrestore(&peer->rx_marker_lock, flags);
}

static void ardma_rx_callback(struct tb_ring *ring, struct ring_frame *frame,
			      bool canceled)
{
	struct ardma_rx_frame *rf = container_of(frame, typeof(*rf), frame);
	struct ardma_peer *peer = rf->peer;
	struct device *dma_dev = tb_ring_dma_device(ring);
	u32 len;
	int ret;

	if (canceled)
		return;

	dma_sync_single_for_cpu(dma_dev, rf->dma, ARDMA_FRAME_SIZE,
				DMA_FROM_DEVICE);
	len = frame->size ?: (READ_ONCE(rx_raw_mode) ? TB_FRAME_SIZE :
			      ARDMA_FRAME_SIZE);
	atomic64_inc(&peer->rx_frame_count);
	if (frame->eof < 4)
		atomic64_inc(&peer->rx_eof[frame->eof]);
	else
		atomic64_inc(&peer->rx_eof_other);
	ardma_rx_marker_log(peer, frame, rf->data, len);

	/* The incoming path is the destination HopID. Apple's visible QPN is
	 * HopID << 8 for the first QP shape we have observed. */
	ardma_rx_apple_frame(peer, (u32)receive_path << 8, rf->data, len,
			     frame->eof);

	dma_sync_single_for_device(dma_dev, rf->dma, ARDMA_FRAME_SIZE,
				   DMA_FROM_DEVICE);
	ret = tb_ring_rx(ring, frame);
	if (ret)
		pr_warn_ratelimited("RX repost failed: %d\n", ret);
}

/* ----- Apple-shaped TX ------------------------------------------- */

static void ardma_tx_ctx_put(struct ardma_send_ctx *ctx, bool failed)
{
	struct ardma_cq *send_cq =
		container_of(ctx->qp->base.send_cq, struct ardma_cq, base);

	if (failed)
		atomic_set(&ctx->failed, 1);

	if (atomic_dec_return(&ctx->pending) == 0) {
		if (ctx->signaled) {
			struct ib_wc wc = {};

			wc.wr_id = ctx->wr_id;
			wc.wr_cqe = ctx->wr_cqe;
			wc.status = atomic_read(&ctx->failed) ?
				IB_WC_GENERAL_ERR : IB_WC_SUCCESS;
			wc.opcode = IB_WC_SEND;
			wc.qp = &ctx->qp->base;
			ardma_cq_push_wc(send_cq, &wc);
		}
		ardma_qp_put(ctx->qp);
		kfree(ctx);
	}
}

static void ardma_tx_callback(struct tb_ring *ring, struct ring_frame *frame,
			      bool canceled)
{
	struct ardma_tx_frame *tf = container_of(frame, typeof(*tf), frame);
	struct device *dma_dev = tb_ring_dma_device(ring);
	struct ardma_peer *peer = tf->peer;

	dma_unmap_single(dma_dev, tf->dma, ARDMA_FRAME_SIZE, DMA_TO_DEVICE);
	if (canceled)
		atomic64_inc(&peer->tx_errors);
	else
		atomic64_inc(&peer->tx_completions);
	ardma_tx_ctx_put(tf->ctx, canceled);
	kfree(tf->data);
	kfree(tf);
	/* Drop the per-tx-frame peer ref taken in ardma_submit_tx_piece.
	 * Done after kfree(tf) so we don't deref tf->peer post-free. */
	ardma_peer_put(peer);
}

enum ardma_tx_piece_kind {
	ARDMA_TX_PIECE_FIRST,
	ARDMA_TX_PIECE_MID,
	ARDMA_TX_PIECE_SPLIT,
	ARDMA_TX_PIECE_TAIL,
};

static void ardma_tx_markers(unsigned int mode, enum ardma_tx_piece_kind kind,
			     u8 tail_eof, u8 *sof, u8 *eof)
{
	bool first = kind == ARDMA_TX_PIECE_FIRST;
	bool tail = kind == ARDMA_TX_PIECE_TAIL;

	if (mode > ARDMA_TX_MARKER_EVERY_SOF_APPLE_EOF)
		mode = ARDMA_TX_MARKER_APPLE_EOF;

	*sof = 0;
	*eof = 0;

	switch (mode) {
	case ARDMA_TX_MARKER_GEMINI_SOF:
		if (first)
			*sof = 1;
		break;
	case ARDMA_TX_MARKER_BOTH_START:
		if (first) {
			*sof = 1;
			*eof = 1;
		}
		break;
	case ARDMA_TX_MARKER_EVERY_DESC:
		*sof = 1;
		*eof = tail ? tail_eof : 1;
		break;
	case ARDMA_TX_MARKER_EVERY_SOF_APPLE_EOF:
		*sof = 1;
		if (first)
			*eof = 1;
		break;
	case ARDMA_TX_MARKER_APPLE_EOF:
	default:
		if (first)
			*eof = 1;
		break;
	}

	if (tail && mode != ARDMA_TX_MARKER_EVERY_DESC)
		*eof = tail_eof;
}

static int ardma_submit_tx_piece(struct ardma_peer *peer,
				 struct ardma_send_ctx *ctx,
				 struct ardma_pd *pd,
				 const struct ib_sge *sg_list, int num_sge,
				 u32 app_off, u32 payload_len,
				 u32 wire_len, u8 sof, u8 eof,
				 bool append_crc, u32 crc)
{
	struct device *dma_dev = tb_ring_dma_device(peer->tx_ring);
	struct ardma_tx_frame *tf;
	__le32 crc_le;
	int ret;

	if (payload_len > ARDMA_FRAME_SIZE ||
	    wire_len > ARDMA_FRAME_SIZE ||
	    (append_crc && payload_len + sizeof(crc_le) > ARDMA_FRAME_SIZE))
		return -EINVAL;

	tf = kzalloc(sizeof(*tf), GFP_KERNEL);
	if (!tf)
		return -ENOMEM;
	tf->data = kzalloc(ARDMA_FRAME_SIZE, GFP_KERNEL);
	if (!tf->data) {
		kfree(tf);
		return -ENOMEM;
	}
	ret = ardma_copy_sges_to_buf(pd, sg_list, num_sge, app_off,
				     tf->data, payload_len);
	if (ret) {
		kfree(tf->data);
		kfree(tf);
		return ret;
	}
	if (append_crc) {
		crc_le = cpu_to_le32(crc);
		memcpy((u8 *)tf->data + payload_len, &crc_le,
		       sizeof(crc_le));
	}

	tf->peer = peer;
	tf->ctx = ctx;
	/* Per-tx-frame peer ref so ardma_tx_callback (IRQ context) can
	 * safely deref tf->peer for stat increments without racing
	 * against ardma_remove freeing the peer. Dropped in the
	 * callback after the last use. */
	refcount_inc(&peer->refs);
	tf->dma = dma_map_single(dma_dev, tf->data, ARDMA_FRAME_SIZE,
				 DMA_TO_DEVICE);
	if (dma_mapping_error(dma_dev, tf->dma)) {
		ardma_peer_put(peer);
		kfree(tf->data);
		kfree(tf);
		return -EIO;
	}

	tf->frame.buffer_phy = tf->dma;
	tf->frame.callback = ardma_tx_callback;
	tf->frame.size = wire_len;
	tf->frame.sof = sof;
	tf->frame.eof = eof;
	INIT_LIST_HEAD(&tf->frame.list);

	atomic_inc(&ctx->pending);
	ret = tb_ring_tx(peer->tx_ring, &tf->frame);
	if (ret) {
		atomic_dec(&ctx->pending);
		dma_unmap_single(dma_dev, tf->dma, ARDMA_FRAME_SIZE,
				 DMA_TO_DEVICE);
		kfree(tf->data);
		kfree(tf);
		ardma_peer_put(peer);
		return ret;
	}
	atomic64_inc(&peer->tx_frames);
	return 0;
}

static int ardma_crc32c_sges(struct ardma_pd *pd,
			     const struct ib_sge *sg_list, int num_sge,
			     u32 src_off, u32 len, u32 *crc_out)
{
	void *buf;
	int ret;

	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = ardma_copy_sges_to_buf(pd, sg_list, num_sge, src_off, buf, len);
	if (!ret)
		*crc_out = crc32c(~0u, buf, len) ^ ~0u;
	kfree(buf);
	return ret;
}

static int ardma_wr_total_len(const struct ib_send_wr *wr, u32 *total)
{
	u32 n = 0;
	int i;

	for (i = 0; i < wr->num_sge; i++) {
		if (wr->sg_list[i].length > U32_MAX - n)
			return -EMSGSIZE;
		n += wr->sg_list[i].length;
	}
	*total = n;
	return 0;
}

static int ardma_send_apple(struct ardma_qp *qp, const struct ib_send_wr *wr)
{
	struct ardma_pd *pd = container_of(qp->base.pd, struct ardma_pd, base);
	struct ardma_peer *peer = qp->peer;
	struct ardma_send_ctx *ctx;
	u32 total, block, blocks;
	unsigned int marker_mode;
	int ret;

	if (!READ_ONCE(tx_enabled))
		return -EOPNOTSUPP;
	if (!peer || !peer->tx_ring || !peer->paths_enabled)
		return -ENOTCONN;
	if (!READ_ONCE(peer->tx_ring_running))
		return -ESHUTDOWN;
	if (wr->num_sge > ARDMA_MAX_SGE || (wr->num_sge && !wr->sg_list))
		return -EINVAL;
	if (wr->send_flags & IB_SEND_INLINE)
		return -EOPNOTSUPP;

	ret = ardma_wr_total_len(wr, &total);
	if (ret)
		return ret;
	if (!total || total % SZ_4K || total > ARDMA_MAX_MSG_SIZE)
		return -EMSGSIZE;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	ctx->qp = qp;
	ctx->wr_id = wr->wr_id;
	ctx->wr_cqe = wr->wr_cqe;
	ctx->signaled = qp->sq_sig_all || (wr->send_flags & IB_SEND_SIGNALED);
	atomic_set(&ctx->pending, 1);
	atomic_set(&ctx->failed, 0);
	ardma_qp_get(qp);
	marker_mode = READ_ONCE(tx_marker_mode);

	blocks = total / SZ_4K;
	for (block = 0; block < blocks; block++) {
		u32 base = block * SZ_4K;
		u32 piece;
		u32 crc = 0;
		unsigned int pace_us;
		u8 sof, eof;

		/* Diagnostic pacing: sleep between 4 KiB blocks if asked.
		 * Used to test the overrun hypothesis without changing the
		 * wire path. block==0 skips so the first block doesn't pay
		 * the latency cost. */
		pace_us = READ_ONCE(tx_pace_us);
		if (block && pace_us)
			usleep_range(pace_us, pace_us + 10);

		if (READ_ONCE(tx_raw_mode)) {
			ret = ardma_crc32c_sges(pd, wr->sg_list, wr->num_sge,
						base, SZ_4K, &crc);
			if (ret)
				goto fail;
		}

		for (piece = 0; piece < 15; piece++) {
			ardma_tx_markers(marker_mode,
					 piece ? ARDMA_TX_PIECE_MID :
						 ARDMA_TX_PIECE_FIRST,
					 0, &sof, &eof);
			ret = ardma_submit_tx_piece(peer, ctx, pd, wr->sg_list,
						    wr->num_sge,
						    base + piece * 0x100,
						    READ_ONCE(tx_raw_mode) ?
							256 : 252,
						    READ_ONCE(tx_raw_mode) ?
							256 : 252,
						    sof, eof,
						    false, 0);
			if (ret)
				goto fail;
		}
		ardma_tx_markers(marker_mode, ARDMA_TX_PIECE_SPLIT, 0,
				 &sof, &eof);
		ret = ardma_submit_tx_piece(peer, ctx, pd, wr->sg_list,
					    wr->num_sge, base + 0x0f00,
					    READ_ONCE(tx_raw_mode) ? 16 : 12,
					    READ_ONCE(tx_raw_mode) ? 16 : 12,
					    sof, eof,
					    false, 0);
		if (ret)
			goto fail;
		ardma_tx_markers(marker_mode, ARDMA_TX_PIECE_TAIL,
				 block + 1 == blocks ? 3 : 2, &sof, &eof);
		ret = ardma_submit_tx_piece(peer, ctx, pd, wr->sg_list,
					    wr->num_sge, base + 0x0f10,
					    240,
					    READ_ONCE(tx_raw_mode) ? 244 : 240,
					    sof, eof,
					    READ_ONCE(tx_raw_mode), crc);
		if (ret)
			goto fail;
	}

	ardma_tx_ctx_put(ctx, false);
	return 0;

fail:
	atomic64_inc(&peer->tx_errors);
	ardma_tx_ctx_put(ctx, true);
	return ret;
}

/* ----- verbs object ops ------------------------------------------ */

static int ardma_alloc_ucontext(struct ib_ucontext *ibuc,
				struct ib_udata *udata)
{
	return 0;
}

static void ardma_dealloc_ucontext(struct ib_ucontext *ibuc)
{
}

static int ardma_alloc_pd(struct ib_pd *ibpd, struct ib_udata *udata)
{
	struct ardma_pd *pd = container_of(ibpd, struct ardma_pd, base);

	INIT_LIST_HEAD(&pd->mrs);
	spin_lock_init(&pd->mr_lock);
	return 0;
}

static int ardma_dealloc_pd(struct ib_pd *ibpd, struct ib_udata *udata)
{
	return 0;
}

static struct ib_mr *ardma_reg_user_mr(struct ib_pd *ibpd, u64 start,
				       u64 length, u64 virt_addr,
				       int access_flags, struct ib_dmah *dmah,
				       struct ib_udata *udata)
{
	static atomic_t lkey_counter = ATOMIC_INIT(1);
	struct ardma_pd *pd = container_of(ibpd, struct ardma_pd, base);
	struct ardma_mr *mr;
	unsigned long flags;
	u64 page_off, va_aligned;
	long got;
	int npages, err;
	u32 lkey;

	page_off = start & ~PAGE_MASK;
	va_aligned = start & PAGE_MASK;
	npages = (page_off + length + PAGE_SIZE - 1) >> PAGE_SHIFT;
	if (npages <= 0 || npages > 1024 * 1024)
		return ERR_PTR(-EINVAL);

	mr = kzalloc(sizeof(*mr), GFP_KERNEL);
	if (!mr)
		return ERR_PTR(-ENOMEM);
	mr->user_va = start;
	mr->length = length;
	mr->npages = npages;
	refcount_set(&mr->refs, 1);
	init_waitqueue_head(&mr->ref_wait);

	mr->pages = kvcalloc(npages, sizeof(*mr->pages), GFP_KERNEL);
	if (!mr->pages) {
		err = -ENOMEM;
		goto err_mr;
	}

	got = pin_user_pages_fast(va_aligned, npages,
				  FOLL_WRITE | FOLL_LONGTERM, mr->pages);
	if (got < 0) {
		err = got;
		goto err_pages;
	}
	if (got < npages) {
		unpin_user_pages(mr->pages, got);
		err = -EFAULT;
		goto err_pages;
	}

	lkey = atomic_inc_return(&lkey_counter);
	mr->base.lkey = lkey;
	mr->base.rkey = lkey;
	mr->base.length = length;
	mr->base.iova = virt_addr;
	mr->base.pd = ibpd;
	mr->base.device = ibpd->device;

	spin_lock_irqsave(&pd->mr_lock, flags);
	list_add(&mr->pd_link, &pd->mrs);
	spin_unlock_irqrestore(&pd->mr_lock, flags);
	return &mr->base;

err_pages:
	kvfree(mr->pages);
err_mr:
	kfree(mr);
	return ERR_PTR(err);
}

static int ardma_dereg_mr(struct ib_mr *ibmr, struct ib_udata *udata)
{
	struct ardma_mr *mr = container_of(ibmr, struct ardma_mr, base);
	struct ardma_pd *pd = container_of(ibmr->pd, struct ardma_pd, base);
	unsigned long flags;

	spin_lock_irqsave(&pd->mr_lock, flags);
	mr->dying = true;
	list_del(&mr->pd_link);
	spin_unlock_irqrestore(&pd->mr_lock, flags);

	wait_event(mr->ref_wait, refcount_read(&mr->refs) == 1);
	if (mr->pages) {
		unpin_user_pages(mr->pages, mr->npages);
		kvfree(mr->pages);
	}
	kfree(mr);
	return 0;
}

static struct ib_mr *ardma_get_dma_mr(struct ib_pd *ibpd, int access_flags)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static int ardma_create_cq(struct ib_cq *ibcq,
			   const struct ib_cq_init_attr *attr,
			   struct uverbs_attr_bundle *attrs)
{
	struct ardma_cq *cq = container_of(ibcq, struct ardma_cq, base);
	int i;

	if (attr->cqe <= 0 || attr->cqe > ARDMA_MAX_CQE)
		return -EINVAL;

	spin_lock_init(&cq->lock);
	INIT_LIST_HEAD(&cq->wc_list);
	INIT_LIST_HEAD(&cq->free_list);
	cq->pool = kvcalloc(attr->cqe, sizeof(*cq->pool), GFP_KERNEL);
	if (!cq->pool)
		return -ENOMEM;
	for (i = 0; i < attr->cqe; i++)
		list_add_tail(&cq->pool[i].list, &cq->free_list);
	cq->cqe_capacity = attr->cqe;
	cq->free_count = attr->cqe;
	return 0;
}

static int ardma_destroy_cq(struct ib_cq *ibcq, struct ib_udata *udata)
{
	struct ardma_cq *cq = container_of(ibcq, struct ardma_cq, base);

	kvfree(cq->pool);
	return 0;
}

static int ardma_poll_cq(struct ib_cq *ibcq, int num_entries,
			 struct ib_wc *wc)
{
	struct ardma_cq *cq = container_of(ibcq, struct ardma_cq, base);
	struct ardma_wc_entry *e, *tmp;
	unsigned long flags;
	int n = 0;

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

static int ardma_req_notify_cq(struct ib_cq *ibcq,
			       enum ib_cq_notify_flags flags)
{
	struct ardma_cq *cq = container_of(ibcq, struct ardma_cq, base);
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

static int ardma_create_qp(struct ib_qp *ibqp, struct ib_qp_init_attr *attr,
			   struct ib_udata *udata)
{
	struct ardma_qp *qp = container_of(ibqp, struct ardma_qp, base);
	struct ardma_peer *peer;
	int qpn;

	if (attr->qp_type != IB_QPT_UC && attr->qp_type != IB_QPT_GSI)
		return -EOPNOTSUPP;
	if (attr->cap.max_send_sge > ARDMA_MAX_SGE ||
	    attr->cap.max_recv_sge > ARDMA_MAX_SGE)
		return -EINVAL;

	peer = ardma_peer_get_active();
	if (!peer && attr->qp_type != IB_QPT_GSI)
		return -ENOTCONN;

	qp->peer = peer;
	qp->qp_type = attr->qp_type;
	qp->init_attr = *attr;
	qp->init_attr.cap.max_send_wr = min_t(u32, attr->cap.max_send_wr, 4095);
	qp->init_attr.cap.max_recv_wr = min_t(u32, attr->cap.max_recv_wr, 4095);
	qp->init_attr.cap.max_send_sge = min_t(u32, attr->cap.max_send_sge,
					       ARDMA_MAX_SGE);
	qp->init_attr.cap.max_recv_sge = min_t(u32, attr->cap.max_recv_sge,
					       ARDMA_MAX_SGE);
	attr->cap = qp->init_attr.cap;
	qp->state = attr->qp_type == IB_QPT_GSI ? IB_QPS_RTS : IB_QPS_RESET;
	qp->sq_sig_all = attr->sq_sig_type == IB_SIGNAL_ALL_WR;
	refcount_set(&qp->refs, 1);
	init_waitqueue_head(&qp->ref_wait);
	spin_lock_init(&qp->recv_lock);
	INIT_LIST_HEAD(&qp->recv_q);
	INIT_LIST_HEAD(&qp->qps_link);
	qp->rx_pending_active = -1;
	if (attr->qp_type == IB_QPT_UC) {
		int i;

		for (i = 0; i < ARDMA_PENDING_RX_SLOTS; i++) {
			qp->rx_pending[i].buf = kzalloc(ARDMA_PENDING_RX_BYTES,
							GFP_KERNEL);
			if (!qp->rx_pending[i].buf) {
				while (i--)
					kfree(qp->rx_pending[i].buf);
				ardma_peer_put(peer);
				return -ENOMEM;
			}
		}
	}

	if (attr->qp_type == IB_QPT_GSI) {
		ibqp->qp_num = 1;
	} else {
		qpn = ida_alloc_range(&ardma_qpn_ida, ARDMA_QPN_MIN,
				      ARDMA_QPN_MAX, GFP_KERNEL);
		if (qpn < 0) {
			int i;

			for (i = 0; i < ARDMA_PENDING_RX_SLOTS; i++) {
				kfree(qp->rx_pending[i].buf);
				qp->rx_pending[i].buf = NULL;
			}
			ardma_peer_put(peer);
			return qpn;
		}
		ibqp->qp_num = qpn;
		qp->qpn_allocated = true;
	}

	pr_info("create_qp %s qpn=0x%x\n",
		attr->qp_type == IB_QPT_UC ? "UC" : "GSI", ibqp->qp_num);
	return 0;
}

static int ardma_modify_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr,
			   int attr_mask, struct ib_udata *udata)
{
	struct ardma_qp *qp = container_of(ibqp, struct ardma_qp, base);
	enum ib_qp_state old = qp->state;
	unsigned long flags;

	if (attr_mask & IB_QP_STATE)
		qp->state = attr->qp_state;
	if (attr_mask & IB_QP_DEST_QPN)
		qp->attr.dest_qp_num = attr->dest_qp_num;
	if (attr_mask & IB_QP_AV)
		qp->attr.ah_attr = attr->ah_attr;
	if (attr_mask & IB_QP_PORT)
		qp->attr.port_num = attr->port_num;
	if (attr_mask & IB_QP_PATH_MTU)
		qp->attr.path_mtu = attr->path_mtu;
	if (attr_mask & IB_QP_SQ_PSN)
		qp->attr.sq_psn = attr->sq_psn;
	if (attr_mask & IB_QP_RQ_PSN)
		qp->attr.rq_psn = attr->rq_psn;
	qp->attr_mask |= attr_mask;

	if (qp->qp_type == IB_QPT_UC && !qp->registered &&
	    old == IB_QPS_RESET && qp->state == IB_QPS_INIT) {
		spin_lock_irqsave(&ardma_qp_lock, flags);
		list_add_tail(&qp->qps_link, &ardma_qp_list);
		spin_unlock_irqrestore(&ardma_qp_lock, flags);
		qp->registered = true;
	}
	if (qp->registered &&
	    (qp->state == IB_QPS_RESET || qp->state == IB_QPS_ERR)) {
		spin_lock_irqsave(&ardma_qp_lock, flags);
		if (!list_empty(&qp->qps_link))
			list_del_init(&qp->qps_link);
		spin_unlock_irqrestore(&ardma_qp_lock, flags);
		qp->registered = false;
	}

	pr_info("modify_qp[0x%x]: %d -> %d dest=0x%x\n", ibqp->qp_num, old,
		qp->state, qp->attr.dest_qp_num);
	return 0;
}

static int ardma_query_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr,
			  int attr_mask, struct ib_qp_init_attr *init_attr)
{
	struct ardma_qp *qp = container_of(ibqp, struct ardma_qp, base);

	*attr = qp->attr;
	attr->qp_state = qp->state;
	*init_attr = qp->init_attr;
	return 0;
}

static int ardma_destroy_qp(struct ib_qp *ibqp, struct ib_udata *udata)
{
	struct ardma_qp *qp = container_of(ibqp, struct ardma_qp, base);
	struct ardma_recv_wr *r, *tmp;
	unsigned long flags;
	int i;

	if (qp->registered) {
		spin_lock_irqsave(&ardma_qp_lock, flags);
		if (!list_empty(&qp->qps_link))
			list_del_init(&qp->qps_link);
		spin_unlock_irqrestore(&ardma_qp_lock, flags);
		qp->registered = false;
	}

	if (refcount_read(&qp->refs) > 1 && qp->peer) {
		pr_warn("destroy_qp[0x%x]: canceling stuck TX ring refs=%u\n",
			ibqp->qp_num, refcount_read(&qp->refs));
		ardma_stop_tx_ring(qp->peer);
	}
	if (!wait_event_timeout(qp->ref_wait,
				refcount_read(&qp->refs) == 1,
				msecs_to_jiffies(5000))) {
		pr_warn("destroy_qp[0x%x]: timed out waiting for refs=%u; "
			"releasing peer ref to avoid hot-unplug hang (leaks QP)\n",
			ibqp->qp_num, refcount_read(&qp->refs));
		/* Critical: drop the peer ref even on timeout. Otherwise
		 * ardma_remove's unbounded wait_event(refs==1) hangs the
		 * tb_service remove path forever (D-state kworker on cable
		 * unplug). We accept the QP-level memory leak (recv_q,
		 * rx_wr, rx_pending, qpn) over a system-level wedge. */
		if (qp->qpn_allocated) {
			ida_free(&ardma_qpn_ida, ibqp->qp_num);
			qp->qpn_allocated = false;
		}
		ardma_peer_put(qp->peer);
		qp->peer = NULL;
		return -ETIMEDOUT;
	}

	spin_lock_irqsave(&qp->recv_lock, flags);
	list_for_each_entry_safe(r, tmp, &qp->recv_q, list) {
		list_del(&r->list);
		kfree(r);
	}
	kfree(qp->rx_wr);
	qp->rx_wr = NULL;
	spin_unlock_irqrestore(&qp->recv_lock, flags);
	for (i = 0; i < ARDMA_PENDING_RX_SLOTS; i++) {
		kfree(qp->rx_pending[i].buf);
		qp->rx_pending[i].buf = NULL;
	}

	if (qp->qpn_allocated)
		ida_free(&ardma_qpn_ida, ibqp->qp_num);
	ardma_peer_put(qp->peer);
	qp->peer = NULL;
	return 0;
}

static int ardma_post_recv(struct ib_qp *ibqp, const struct ib_recv_wr *wr,
			   const struct ib_recv_wr **bad_wr)
{
	struct ardma_qp *qp = container_of(ibqp, struct ardma_qp, base);
	unsigned long flags;

	if (qp->state != IB_QPS_INIT && qp->state != IB_QPS_RTR &&
	    qp->state != IB_QPS_RTS && qp->state != IB_QPS_SQD &&
	    qp->state != IB_QPS_SQE) {
		if (bad_wr)
			*bad_wr = wr;
		return -EINVAL;
	}

	for (; wr; wr = wr->next) {
		struct ardma_recv_wr *r;
		int i;

		if (wr->num_sge > ARDMA_MAX_SGE ||
		    (wr->num_sge && !wr->sg_list)) {
			if (bad_wr)
				*bad_wr = wr;
			return -EINVAL;
		}
		r = kmalloc(sizeof(*r), GFP_KERNEL);
		if (!r) {
			if (bad_wr)
				*bad_wr = wr;
			return -ENOMEM;
		}
		r->wr_id = wr->wr_id;
		r->wr_cqe = wr->wr_cqe;
		r->num_sge = wr->num_sge;
		for (i = 0; i < wr->num_sge; i++)
			r->sge[i] = wr->sg_list[i];

		spin_lock_irqsave(&qp->recv_lock, flags);
		list_add_tail(&r->list, &qp->recv_q);
		spin_unlock_irqrestore(&qp->recv_lock, flags);
	}
	for (;;) {
		struct ardma_recv_wr *r;
		enum ib_wc_status status;
		u32 byte_len;

		spin_lock_irqsave(&qp->recv_lock, flags);
		r = ardma_flush_pending_locked(qp, &byte_len, &status);
		spin_unlock_irqrestore(&qp->recv_lock, flags);
		if (!r)
			break;
		ardma_push_rx_wc(qp, r, byte_len, status);
	}
	return 0;
}

static int ardma_post_send(struct ib_qp *ibqp, const struct ib_send_wr *wr,
			   const struct ib_send_wr **bad_wr)
{
	struct ardma_qp *qp = container_of(ibqp, struct ardma_qp, base);

	if (qp->state != IB_QPS_RTS && qp->state != IB_QPS_SQD &&
	    qp->state != IB_QPS_SQE) {
		if (bad_wr)
			*bad_wr = wr;
		return -EINVAL;
	}

	for (; wr; wr = wr->next) {
		int ret;

		if (wr->opcode != IB_WR_SEND) {
			if (bad_wr)
				*bad_wr = wr;
			return -EOPNOTSUPP;
		}
		ret = ardma_send_apple(qp, wr);
		if (ret) {
			if (bad_wr)
				*bad_wr = wr;
			return ret;
		}
	}
	return 0;
}

static int ardma_create_ah(struct ib_ah *ibah,
			   struct rdma_ah_init_attr *init_attr,
			   struct ib_udata *udata)
{
	struct ardma_ah *ah = container_of(ibah, struct ardma_ah, base);

	if (!init_attr || !init_attr->ah_attr)
		return -EINVAL;
	rdma_copy_ah_attr(&ah->attr, init_attr->ah_attr);
	return 0;
}

static int ardma_query_ah(struct ib_ah *ibah, struct rdma_ah_attr *attr)
{
	struct ardma_ah *ah = container_of(ibah, struct ardma_ah, base);

	rdma_copy_ah_attr(attr, &ah->attr);
	return 0;
}

static int ardma_destroy_ah(struct ib_ah *ibah, u32 flags)
{
	struct ardma_ah *ah = container_of(ibah, struct ardma_ah, base);

	rdma_destroy_ah_attr(&ah->attr);
	return 0;
}

/* ----- query/GID ops --------------------------------------------- */

static int ardma_query_device(struct ib_device *ibdev,
			      struct ib_device_attr *attr,
			      struct ib_udata *udata)
{
	memset(attr, 0, sizeof(*attr));
	attr->vendor_id = 0x106b;
	attr->vendor_part_id = APPLE_RDMA_PRTCID;
	attr->hw_ver = 1;
	attr->sys_image_guid = ibdev->node_guid;
	attr->device_cap_flags = IB_DEVICE_CHANGE_PHY_PORT;
	attr->kernel_cap_flags = IBK_LOCAL_DMA_LKEY;
	attr->max_mr_size = ~0ull;
	attr->page_size_cap = SZ_4K | SZ_2M | SZ_1G;
	attr->max_qp = 11;
	attr->max_qp_wr = 4095;
	attr->max_send_sge = ARDMA_MAX_SGE;
	attr->max_recv_sge = ARDMA_MAX_SGE;
	attr->max_cq = 11;
	attr->max_cqe = ARDMA_MAX_CQE;
	attr->max_mr = 1024;
	attr->max_pd = 11;
	attr->max_ah = 1024;
	attr->atomic_cap = IB_ATOMIC_NONE;
	attr->max_pkeys = 1;
	return 0;
}

static int ardma_query_port(struct ib_device *ibdev, u32 port_num,
			    struct ib_port_attr *attr)
{
	struct ardma_ibdev *dev = container_of(ibdev, struct ardma_ibdev, base);
	bool active;

	if (port_num != 1)
		return -EINVAL;

	memset(attr, 0, sizeof(*attr));
	active = atomic_read(&dev->active_peers) > 0;
	attr->state = active ? IB_PORT_ACTIVE : IB_PORT_DOWN;
	attr->phys_state = active ?
		IB_PORT_PHYS_STATE_LINK_UP : IB_PORT_PHYS_STATE_DISABLED;
	attr->max_mtu = IB_MTU_4096;
	attr->active_mtu = IB_MTU_4096;
	attr->max_msg_sz = ARDMA_MAX_MSG_SIZE;
	attr->gid_tbl_len = 32;
	attr->pkey_tbl_len = 1;
	attr->max_vl_num = 1;
	attr->active_width = IB_WIDTH_4X;
	attr->active_speed = IB_SPEED_FDR10;
	return 0;
}

static int ardma_get_port_immutable(struct ib_device *ibdev, u32 port_num,
				    struct ib_port_immutable *immutable)
{
	struct ib_port_attr attr;
	int ret;

	ret = ardma_query_port(ibdev, port_num, &attr);
	if (ret)
		return ret;
	immutable->pkey_tbl_len = attr.pkey_tbl_len;
	immutable->gid_tbl_len = attr.gid_tbl_len;
	immutable->core_cap_flags = RDMA_CORE_PORT_IBA_ROCE_UDP_ENCAP;
	immutable->max_mad_size = IB_MGMT_MAD_SIZE;
	return 0;
}

static int ardma_query_pkey(struct ib_device *ibdev, u32 port, u16 index,
			    u16 *pkey)
{
	if (port != 1 || index)
		return -EINVAL;
	*pkey = 0xffff;
	return 0;
}

static enum rdma_link_layer ardma_get_link_layer(struct ib_device *ibdev,
						 u32 port_num)
{
	return IB_LINK_LAYER_ETHERNET;
}

static struct net_device *ardma_get_netdev(struct ib_device *ibdev,
					   u32 port_num)
{
	struct ardma_ibdev *dev = container_of(ibdev, struct ardma_ibdev, base);

	if (port_num != 1 || !dev->netdev)
		return NULL;
	dev_hold(dev->netdev);
	return dev->netdev;
}

static int ardma_add_gid(const struct ib_gid_attr *attr, void **context)
{
	return 0;
}

static int ardma_del_gid(const struct ib_gid_attr *attr, void **context)
{
	return 0;
}

static const struct ib_device_ops ardma_dev_ops = {
	.owner = THIS_MODULE,
	.driver_id = RDMA_DRIVER_UNKNOWN,
	.uverbs_abi_ver = ARDMA_UVERBS_ABI,
	.uverbs_no_driver_id_binding = 1,

	.query_device = ardma_query_device,
	.query_port = ardma_query_port,
	.query_pkey = ardma_query_pkey,
	.add_gid = ardma_add_gid,
	.del_gid = ardma_del_gid,
	.get_netdev = ardma_get_netdev,
	.get_port_immutable = ardma_get_port_immutable,
	.get_link_layer = ardma_get_link_layer,

	.alloc_ucontext = ardma_alloc_ucontext,
	.dealloc_ucontext = ardma_dealloc_ucontext,
	.alloc_pd = ardma_alloc_pd,
	.dealloc_pd = ardma_dealloc_pd,
	.create_qp = ardma_create_qp,
	.destroy_qp = ardma_destroy_qp,
	.modify_qp = ardma_modify_qp,
	.query_qp = ardma_query_qp,
	.create_ah = ardma_create_ah,
	.create_user_ah = ardma_create_ah,
	.query_ah = ardma_query_ah,
	.destroy_ah = ardma_destroy_ah,
	.create_cq = ardma_create_cq,
	.destroy_cq = ardma_destroy_cq,
	.post_send = ardma_post_send,
	.post_recv = ardma_post_recv,
	.poll_cq = ardma_poll_cq,
	.req_notify_cq = ardma_req_notify_cq,
	.reg_user_mr = ardma_reg_user_mr,
	.dereg_mr = ardma_dereg_mr,
	.get_dma_mr = ardma_get_dma_mr,

	INIT_RDMA_OBJ_SIZE(ib_ucontext, ardma_ucontext, base),
	INIT_RDMA_OBJ_SIZE(ib_ah, ardma_ah, base),
	INIT_RDMA_OBJ_SIZE(ib_pd, ardma_pd, base),
	INIT_RDMA_OBJ_SIZE(ib_cq, ardma_cq, base),
	INIT_RDMA_OBJ_SIZE(ib_qp, ardma_qp, base),
};

/* ----- ib_device registration ------------------------------------ */

static int ardma_register_ibdev(void)
{
	struct ardma_ibdev *dev;
	u8 mac[ETH_ALEN];
	int ret;

	dev = ib_alloc_device(ardma_ibdev, base);
	if (!dev)
		return -ENOMEM;

	atomic_set(&dev->active_peers, 0);
	dev->base.phys_port_cnt = 1;
	dev->base.num_comp_vectors = num_possible_cpus();
	dev->base.local_dma_lkey = 0;
	dev->base.node_type = RDMA_NODE_IB_CA;
	dev->base.uverbs_cmd_mask |=
		BIT_ULL(IB_USER_VERBS_CMD_POST_SEND) |
		BIT_ULL(IB_USER_VERBS_CMD_POST_RECV) |
		BIT_ULL(IB_USER_VERBS_CMD_POLL_CQ) |
		BIT_ULL(IB_USER_VERBS_CMD_REQ_NOTIFY_CQ);

	eth_random_addr(mac);
	addrconf_addr_eui48((u8 *)&dev->base.node_guid, mac);

	dev->netdev = dev_get_by_name(&init_net, cm_netdev);
	if (!dev->netdev) {
		pr_err("CM netdev '%s' not found\n", cm_netdev);
		ib_dealloc_device(&dev->base);
		return -ENODEV;
	}

	ib_set_device_ops(&dev->base, &ardma_dev_ops);
	ret = ib_device_set_netdev(&dev->base, dev->netdev, 1);
	if (ret)
		goto err_netdev;

	ret = ib_register_device(&dev->base, "usb4_rdma%d", NULL);
	if (ret)
		goto err_netdev;

	ardma_ibdev = dev;
	pr_info("registered ib_device %s using GID netdev %s\n",
		dev_name(&dev->base.dev), cm_netdev);
	return 0;

err_netdev:
	dev_put(dev->netdev);
	ib_dealloc_device(&dev->base);
	return ret;
}

static void ardma_unregister_ibdev(void)
{
	struct ardma_ibdev *dev = ardma_ibdev;
	struct net_device *netdev;

	if (!dev)
		return;
	ardma_ibdev = NULL;
	netdev = dev->netdev;
	dev->netdev = NULL;
	ib_unregister_device(&dev->base);
	ib_dealloc_device(&dev->base);
	if (netdev)
		dev_put(netdev);
}

/* ----- Thunderbolt service/rings --------------------------------- */

static int ardma_alloc_rx_frames(struct ardma_peer *peer)
{
	struct device *dma_dev = tb_ring_dma_device(peer->rx_ring);
	int i;

	peer->rx_frames = kcalloc(ARDMA_RX_FRAMES, sizeof(*peer->rx_frames),
				  GFP_KERNEL);
	if (!peer->rx_frames)
		return -ENOMEM;

	for (i = 0; i < ARDMA_RX_FRAMES; i++) {
		struct ardma_rx_frame *rf = &peer->rx_frames[i];

		rf->peer = peer;
		rf->data = kmalloc(ARDMA_FRAME_SIZE, GFP_KERNEL);
		if (!rf->data)
			goto err;
		rf->dma = dma_map_single(dma_dev, rf->data, ARDMA_FRAME_SIZE,
					 DMA_FROM_DEVICE);
		if (dma_mapping_error(dma_dev, rf->dma)) {
			kfree(rf->data);
			rf->data = NULL;
			goto err;
		}
		rf->frame.buffer_phy = rf->dma;
		rf->frame.size = 0;
		rf->frame.callback = ardma_rx_callback;
		INIT_LIST_HEAD(&rf->frame.list);
	}
	return 0;

err:
	while (--i >= 0) {
		struct ardma_rx_frame *rf = &peer->rx_frames[i];

		if (rf->data) {
			dma_unmap_single(dma_dev, rf->dma, ARDMA_FRAME_SIZE,
					 DMA_FROM_DEVICE);
			kfree(rf->data);
		}
	}
	kfree(peer->rx_frames);
	peer->rx_frames = NULL;
	return -ENOMEM;
}

static void ardma_free_rx_frames(struct ardma_peer *peer)
{
	struct device *dma_dev;
	int i;

	if (!peer->rx_frames || !peer->rx_ring)
		return;
	dma_dev = tb_ring_dma_device(peer->rx_ring);
	for (i = 0; i < ARDMA_RX_FRAMES; i++) {
		struct ardma_rx_frame *rf = &peer->rx_frames[i];

		if (rf->data) {
			dma_unmap_single(dma_dev, rf->dma, ARDMA_FRAME_SIZE,
					 DMA_FROM_DEVICE);
			kfree(rf->data);
		}
	}
	kfree(peer->rx_frames);
	peer->rx_frames = NULL;
}

static int ardma_setup_rings(struct ardma_peer *peer)
{
	struct tb_xdomain *xd = peer->xd;
	unsigned int tx_ring_flags = READ_ONCE(tx_e2e) ? RING_FLAG_E2E : 0;
	unsigned int rx_ring_flags = RING_FLAG_E2E;
	int e2e_tx_hop;
	int ret, i;

	if (!READ_ONCE(tx_raw_mode))
		tx_ring_flags |= RING_FLAG_FRAME;
	if (!READ_ONCE(rx_raw_mode))
		rx_ring_flags |= RING_FLAG_FRAME;

	peer->local_in_hop = -1;
	peer->local_out_hop = -1;

	ret = tb_xdomain_alloc_in_hopid(xd, receive_path);
	if (ret != receive_path) {
		if (ret >= 0) {
			tb_xdomain_release_in_hopid(xd, ret);
			ret = -EINVAL;
		}
		pr_warn("alloc_in_hopid(%d) failed: %d\n", receive_path, ret);
		return ret;
	}
	peer->local_in_hop = ret;

	peer->tx_ring = tb_ring_alloc_tx(xd->tb->nhi, -1, ARDMA_RING_DEPTH,
					 tx_ring_flags);
	if (!peer->tx_ring) {
		ret = -ENOMEM;
		goto err_in_hop;
	}
	e2e_tx_hop = peer->tx_ring->hop;

	ret = tb_xdomain_alloc_out_hopid(xd, -1);
	if (ret < 0)
		goto err_tx_ring;
	peer->local_out_hop = ret;

	peer->rx_ring = tb_ring_alloc_rx(xd->tb->nhi, -1, ARDMA_RING_DEPTH,
					 rx_ring_flags, e2e_tx_hop,
					 ARDMA_RX_SOF_MASK,
					 ARDMA_RX_EOF_MASK, NULL, NULL);
	if (!peer->rx_ring) {
		ret = -ENOMEM;
		goto err_out_hop;
	}

	ret = ardma_alloc_rx_frames(peer);
	if (ret)
		goto err_rx_ring;

	tb_ring_start(peer->tx_ring);
	peer->tx_ring_running = true;
	tb_ring_start(peer->rx_ring);

	for (i = 0; i < ARDMA_RX_FRAMES; i++) {
		ret = tb_ring_rx(peer->rx_ring, &peer->rx_frames[i].frame);
		if (ret) {
			pr_warn("post RX frame %d failed: %d\n", i, ret);
			goto err_started;
		}
	}

	ret = tb_xdomain_enable_paths(xd, peer->local_out_hop,
				      peer->tx_ring->hop,
				      peer->local_in_hop,
				      peer->rx_ring->hop);
	if (ret) {
		pr_warn("enable_paths failed: %d\n", ret);
		goto err_started;
	}
	peer->paths_enabled = true;

	pr_info("rings active: in_path=%d rx_hop=%d out_path=%d tx_hop=%d\n",
		peer->local_in_hop, peer->rx_ring->hop,
		peer->local_out_hop, peer->tx_ring->hop);
	return 0;

err_started:
	tb_ring_stop(peer->rx_ring);
	ardma_stop_tx_ring(peer);
	ardma_free_rx_frames(peer);
err_rx_ring:
	tb_ring_free(peer->rx_ring);
	peer->rx_ring = NULL;
err_out_hop:
	if (peer->local_out_hop >= 0)
		tb_xdomain_release_out_hopid(xd, peer->local_out_hop);
	peer->local_out_hop = -1;
err_tx_ring:
	tb_ring_free(peer->tx_ring);
	peer->tx_ring = NULL;
err_in_hop:
	if (peer->local_in_hop >= 0)
		tb_xdomain_release_in_hopid(xd, peer->local_in_hop);
	peer->local_in_hop = -1;
	return ret;
}

static void ardma_teardown_rings(struct ardma_peer *peer)
{
	if (peer->paths_enabled) {
		tb_xdomain_disable_paths(peer->xd, peer->local_out_hop,
					 peer->tx_ring ? peer->tx_ring->hop : -1,
					 peer->local_in_hop,
					 peer->rx_ring ? peer->rx_ring->hop : -1);
		peer->paths_enabled = false;
	}
	if (peer->rx_ring) {
		tb_ring_stop(peer->rx_ring);
		ardma_free_rx_frames(peer);
		tb_ring_free(peer->rx_ring);
		peer->rx_ring = NULL;
	}
	if (peer->tx_ring) {
		ardma_stop_tx_ring(peer);
		tb_ring_free(peer->tx_ring);
		peer->tx_ring = NULL;
	}
	if (peer->local_in_hop >= 0) {
		tb_xdomain_release_in_hopid(peer->xd, peer->local_in_hop);
		peer->local_in_hop = -1;
	}
	if (peer->local_out_hop >= 0) {
		tb_xdomain_release_out_hopid(peer->xd, peer->local_out_hop);
		peer->local_out_hop = -1;
	}
}

static int ardma_stats_show(struct seq_file *m, void *unused)
{
	struct ardma_peer *peer = m->private;

	seq_printf(m, "receive_path: %d\n", receive_path);
	seq_printf(m, "paths_enabled: %u\n", peer->paths_enabled);
	seq_printf(m, "local_in_hop: %d\n", peer->local_in_hop);
	seq_printf(m, "rx_hop: %d\n", peer->rx_ring ? peer->rx_ring->hop : -1);
	seq_printf(m, "local_out_hop: %d\n", peer->local_out_hop);
	seq_printf(m, "tx_hop: %d\n", peer->tx_ring ? peer->tx_ring->hop : -1);
	seq_printf(m, "tx_marker_mode: %u\n", READ_ONCE(tx_marker_mode));
	seq_printf(m, "rx_frames: %lld\n",
		   (long long)atomic64_read(&peer->rx_frame_count));
	seq_printf(m, "rx_eof0: %lld\n",
		   (long long)atomic64_read(&peer->rx_eof[0]));
	seq_printf(m, "rx_eof1: %lld\n",
		   (long long)atomic64_read(&peer->rx_eof[1]));
	seq_printf(m, "rx_eof2: %lld\n",
		   (long long)atomic64_read(&peer->rx_eof[2]));
	seq_printf(m, "rx_eof3: %lld\n",
		   (long long)atomic64_read(&peer->rx_eof[3]));
	seq_printf(m, "rx_eof_other: %lld\n",
		   (long long)atomic64_read(&peer->rx_eof_other));
	seq_printf(m, "rx_messages: %lld\n",
		   (long long)atomic64_read(&peer->rx_messages));
	seq_printf(m, "rx_drops: %lld\n",
		   (long long)atomic64_read(&peer->rx_drops));
	seq_printf(m, "rx_bad_shape: %lld\n",
		   (long long)atomic64_read(&peer->rx_bad_shape));
	seq_printf(m, "rx_no_qp: %lld\n",
		   (long long)atomic64_read(&peer->rx_no_qp));
	seq_printf(m, "tx_frames: %lld\n",
		   (long long)atomic64_read(&peer->tx_frames));
	seq_printf(m, "tx_completions: %lld\n",
		   (long long)atomic64_read(&peer->tx_completions));
	seq_printf(m, "tx_errors: %lld\n",
		   (long long)atomic64_read(&peer->tx_errors));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ardma_stats);

static u32 ardma_ring_reg_base(const struct tb_ring *ring)
{
	return ring->is_tx ? ARDMA_NHI_REG_TX_RING_BASE :
			     ARDMA_NHI_REG_RX_RING_BASE;
}

static u32 ardma_options_reg_base(const struct tb_ring *ring)
{
	return ring->is_tx ? ARDMA_NHI_REG_TX_OPTIONS_BASE :
			     ARDMA_NHI_REG_RX_OPTIONS_BASE;
}

static void __iomem *ardma_ring_mmio(struct tb_ring *ring)
{
	if (!ring || !ring->nhi || !ring->nhi->iobase)
		return NULL;
	return ring->nhi->iobase + ardma_ring_reg_base(ring) +
	       ring->hop * ARDMA_NHI_RING_SLOT_SIZE;
}

static void __iomem *ardma_options_mmio(struct tb_ring *ring)
{
	if (!ring || !ring->nhi || !ring->nhi->iobase)
		return NULL;
	return ring->nhi->iobase + ardma_options_reg_base(ring) +
	       ring->hop * ARDMA_NHI_OPTIONS_SLOT_SIZE;
}

static void ardma_count_ring_lists(struct tb_ring *ring,
				   unsigned int *queued,
				   unsigned int *inflight)
{
	struct ring_frame *frame;

	*queued = 0;
	*inflight = 0;
	list_for_each_entry(frame, &ring->queue, list)
		(*queued)++;
	list_for_each_entry(frame, &ring->in_flight, list)
		(*inflight)++;
}

static void ardma_dump_ring_state(struct seq_file *m, const char *name,
				  struct tb_ring *ring)
{
	struct ardma_ring_desc *descs;
	unsigned int queued, inflight;
	void __iomem *ring_mmio;
	void __iomem *opt_mmio;
	unsigned long flags;
	int head, tail, size;
	int i;

	if (!ring) {
		seq_printf(m, "%s: none\n\n", name);
		return;
	}

	descs = (struct ardma_ring_desc *)ring->descriptors;
	ring_mmio = ardma_ring_mmio(ring);
	opt_mmio = ardma_options_mmio(ring);

	spin_lock_irqsave(&ring->lock, flags);
	head = ring->head;
	tail = ring->tail;
	size = ring->size;
	ardma_count_ring_lists(ring, &queued, &inflight);

	seq_printf(m,
		   "%s: hop=%d is_tx=%u running=%u flags=0x%x e2e_tx_hop=%d size=%d head=%d tail=%d queued=%u inflight=%u\n",
		   name, ring->hop, ring->is_tx, ring->running, ring->flags,
		   ring->e2e_tx_hop, size, head, tail, queued, inflight);
	seq_printf(m, "%s: descriptors=%px descriptors_dma=%pad\n",
		   name, ring->descriptors, &ring->descriptors_dma);

	if (ring_mmio) {
		seq_printf(m,
			   "%s: ring_regs[0..15]=0x%08x 0x%08x 0x%08x 0x%08x\n",
			   name, ioread32(ring_mmio + 0),
			   ioread32(ring_mmio + 4), ioread32(ring_mmio + 8),
			   ioread32(ring_mmio + 12));
	} else {
		seq_printf(m, "%s: ring_regs unavailable\n", name);
	}

	if (opt_mmio) {
		seq_printf(m,
			   "%s: options[0..31]=0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x\n",
			   name, ioread32(opt_mmio + 0), ioread32(opt_mmio + 4),
			   ioread32(opt_mmio + 8), ioread32(opt_mmio + 12),
			   ioread32(opt_mmio + 16), ioread32(opt_mmio + 20),
			   ioread32(opt_mmio + 24), ioread32(opt_mmio + 28));
	} else {
		seq_printf(m, "%s: options unavailable\n", name);
	}

	if (!descs || size <= 0) {
		seq_puts(m, "\n");
		spin_unlock_irqrestore(&ring->lock, flags);
		return;
	}

	seq_printf(m, "%s: next descriptors from tail\n", name);
	for (i = 0; i < min(size, 8); i++) {
		int idx = (tail + i) % size;
		u32 meta = READ_ONCE(descs[idx].meta);
		u32 time = READ_ONCE(descs[idx].time);

		seq_printf(m,
			   "  desc[%03d]: phys=0x%016llx len=%u eof=%u sof=%u flags=0x%03x meta=0x%08x time=0x%08x%s%s\n",
			   idx, (unsigned long long)READ_ONCE(descs[idx].phys),
			   meta & 0xfff, (meta >> 12) & 0xf,
			   (meta >> 16) & 0xf, meta >> ARDMA_DESC_FLAGS_SHIFT,
			   meta, time, idx == tail ? " <tail>" : "",
			   idx == head ? " <head>" : "");
	}
	seq_puts(m, "\n");
	spin_unlock_irqrestore(&ring->lock, flags);
}

static int ardma_nhi_stall_dump_show(struct seq_file *m, void *unused)
{
	struct ardma_peer *peer = m->private;

	if (!peer)
		return -ENODEV;

	seq_printf(m, "receive_path: %d\n", receive_path);
	seq_printf(m, "paths_enabled: %u\n", peer->paths_enabled);
	seq_printf(m, "local_in_hop: %d\n", peer->local_in_hop);
	seq_printf(m, "local_out_hop: %d\n", peer->local_out_hop);
	seq_printf(m, "tx_marker_mode: %u\n", READ_ONCE(tx_marker_mode));
	seq_printf(m, "rx_frames: %lld\n",
		   (long long)atomic64_read(&peer->rx_frame_count));
	seq_printf(m, "rx_eof0: %lld\n",
		   (long long)atomic64_read(&peer->rx_eof[0]));
	seq_printf(m, "rx_eof1: %lld\n",
		   (long long)atomic64_read(&peer->rx_eof[1]));
	seq_printf(m, "rx_eof2: %lld\n",
		   (long long)atomic64_read(&peer->rx_eof[2]));
	seq_printf(m, "rx_eof3: %lld\n",
		   (long long)atomic64_read(&peer->rx_eof[3]));
	seq_printf(m, "rx_eof_other: %lld\n",
		   (long long)atomic64_read(&peer->rx_eof_other));
	seq_printf(m, "rx_messages: %lld\n",
		   (long long)atomic64_read(&peer->rx_messages));
	seq_printf(m, "tx_frames: %lld\n",
		   (long long)atomic64_read(&peer->tx_frames));
	seq_printf(m, "tx_completions: %lld\n",
		   (long long)atomic64_read(&peer->tx_completions));
	seq_printf(m, "tx_errors: %lld\n\n",
		   (long long)atomic64_read(&peer->tx_errors));

	ardma_dump_ring_state(m, "tx_ring", peer->tx_ring);
	ardma_dump_ring_state(m, "rx_ring", peer->rx_ring);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ardma_nhi_stall_dump);

static int ardma_rx_marker_log_show(struct seq_file *m, void *unused)
{
	struct ardma_rx_marker_log_entry *entries;
	struct ardma_peer *peer = m->private;
	unsigned long flags;
	u64 total;
	u32 count;
	u32 start;
	u32 pos;
	int i, j;

	if (!peer)
		return -ENODEV;

	entries = kcalloc(ARDMA_RX_MARKER_LOG_ENTRIES, sizeof(*entries),
			  GFP_KERNEL);
	if (!entries)
		return -ENOMEM;

	spin_lock_irqsave(&peer->rx_marker_lock, flags);
	total = peer->rx_marker_seq;
	pos = peer->rx_marker_pos;
	count = min_t(u64, total, ARDMA_RX_MARKER_LOG_ENTRIES);
	start = (pos + ARDMA_RX_MARKER_LOG_ENTRIES - count) %
		ARDMA_RX_MARKER_LOG_ENTRIES;
	for (i = 0; i < count; i++)
		entries[i] = peer->rx_marker_log[
			(start + i) % ARDMA_RX_MARKER_LOG_ENTRIES];
	spin_unlock_irqrestore(&peer->rx_marker_lock, flags);

	seq_printf(m, "rx marker entries logged: %llu (showing %u)\n\n",
		   total, count);
	for (i = 0; i < count; i++) {
		struct ardma_rx_marker_log_entry *e = &entries[i];

		seq_printf(m,
			   "seq=%llu len=%u sof=%u eof=%u flags=0x%03x meta=0x%08x prefix=",
			   e->seq, e->len, e->sof, e->eof, e->flags, e->meta);
		for (j = 0; j < e->prefix_len; j++)
			seq_printf(m, "%02x%s", e->prefix[j],
				   j + 1 == e->prefix_len ? "" : " ");
		seq_putc(m, '\n');
	}

	kfree(entries);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ardma_rx_marker_log);

static int ardma_ctrl_log_show(struct seq_file *m, void *unused)
{
	struct ardma_ctrl_log_entry *e;
	unsigned long flags;
	int i = 0;

	seq_printf(m, "control messages logged: %d (received total: %d)\n\n",
		   atomic_read(&ardma_ctrl_logged),
		   atomic_read(&ardma_ctrl_received));

	spin_lock_irqsave(&ardma_ctrl_lock, flags);
	list_for_each_entry(e, &ardma_ctrl_list, list) {
		seq_printf(m, "==== ctrl[%d] size=%u (showing first %u bytes) ====\n",
			   i++, e->size, e->dump_len);
		seq_hex_dump(m, "  ", DUMP_PREFIX_OFFSET, 16, 1,
			     e->dump, e->dump_len, true);
		seq_puts(m, "\n");
	}
	spin_unlock_irqrestore(&ardma_ctrl_lock, flags);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(ardma_ctrl_log);

static int ardma_probe(struct tb_service *svc, const struct tb_service_id *id)
{
	struct tb_xdomain *xd = tb_service_parent(svc);
	struct ardma_peer *peer;
	int ret;

	if (!xd)
		return -ENODEV;
	if (apple_vendor_only &&
	    (!xd->vendor_name || strcmp(xd->vendor_name, "Apple Inc."))) {
		dev_info(&svc->dev,
			 "skipping non-Apple AD/FA57 peer vendor='%s'\n",
			 xd->vendor_name ? xd->vendor_name : "(null)");
		return -ENODEV;
	}
	if (peer_device_name && *peer_device_name &&
	    (!xd->device_name || strcmp(xd->device_name, peer_device_name))) {
		dev_info(&svc->dev,
			 "skipping AD/FA57 peer device='%s' wanted='%s'\n",
			 xd->device_name ? xd->device_name : "(null)",
			 peer_device_name);
		return -ENODEV;
	}

	mutex_lock(&ardma_peer_lock);
	if (ardma_active_peer) {
		mutex_unlock(&ardma_peer_lock);
		dev_warn(&svc->dev, "only one Apple peer supported for now\n");
		return -EBUSY;
	}
	mutex_unlock(&ardma_peer_lock);

	peer = devm_kzalloc(&svc->dev, sizeof(*peer), GFP_KERNEL);
	if (!peer)
		return -ENOMEM;
	peer->svc = svc;
	peer->xd = xd;
	refcount_set(&peer->refs, 1);
	init_waitqueue_head(&peer->ref_wait);
	mutex_init(&peer->tx_lock);
	spin_lock_init(&peer->rx_marker_lock);
	peer->local_in_hop = -1;
	peer->local_out_hop = -1;

	dev_info(&svc->dev,
		 "Apple RDMA peer route=0x%llx link_speed=%u service_uuid=%pUb\n",
		 xd->route, xd->link_speed, &apple_rdma_service_uuid);

	ret = ardma_setup_rings(peer);
	if (ret)
		return ret;

	peer->debugfs_dir = debugfs_create_dir(dev_name(&svc->dev),
					       ardma_debugfs_root);
	if (!IS_ERR_OR_NULL(peer->debugfs_dir))
		debugfs_create_file("stats", 0444, peer->debugfs_dir, peer,
				    &ardma_stats_fops);
	if (!IS_ERR_OR_NULL(peer->debugfs_dir))
		debugfs_create_file("nhi_stall_dump", 0444, peer->debugfs_dir,
				    peer, &ardma_nhi_stall_dump_fops);
	if (!IS_ERR_OR_NULL(peer->debugfs_dir))
		debugfs_create_file("rx_marker_log", 0444, peer->debugfs_dir,
				    peer, &ardma_rx_marker_log_fops);

	tb_service_set_drvdata(svc, peer);
	mutex_lock(&ardma_peer_lock);
	ardma_active_peer = peer;
	mutex_unlock(&ardma_peer_lock);
	if (ardma_ibdev)
		atomic_inc(&ardma_ibdev->active_peers);
	return 0;
}

static void ardma_remove(struct tb_service *svc)
{
	struct ardma_peer *peer = tb_service_get_drvdata(svc);

	if (!peer)
		return;

	mutex_lock(&ardma_peer_lock);
	if (ardma_active_peer == peer)
		ardma_active_peer = NULL;
	mutex_unlock(&ardma_peer_lock);
	WRITE_ONCE(peer->closing, true);
	if (ardma_ibdev)
		atomic_set(&ardma_ibdev->active_peers, 0);
	wait_event(peer->ref_wait, refcount_read(&peer->refs) == 1);
	ardma_teardown_rings(peer);
	debugfs_remove_recursive(peer->debugfs_dir);
	tb_service_set_drvdata(svc, NULL);
}

static const struct tb_service_id ardma_ids[] = {
	{
		.match_flags = TBSVC_MATCH_PROTOCOL_KEY |
			       TBSVC_MATCH_PROTOCOL_ID |
			       TBSVC_MATCH_PROTOCOL_VERSION |
			       TBSVC_MATCH_PROTOCOL_REVISION,
		.protocol_key = {
			(char)0xff, (char)0xff, (char)0xff, (char)0xff,
			(char)0xff, (char)0xff, 'A', 'D', '\0',
		},
		.protocol_id = APPLE_RDMA_PRTCID,
		.protocol_version = APPLE_RDMA_PRTCVERS,
		.protocol_revision = APPLE_RDMA_PRTCREVS,
	},
	{ },
};
MODULE_DEVICE_TABLE(tbsvc, ardma_ids);

static struct tb_service_driver ardma_service_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = ARDMA_DRV_NAME,
	},
	.probe = ardma_probe,
	.remove = ardma_remove,
	.id_table = ardma_ids,
};

/* ----- property dir / control handler ---------------------------- */

static int ardma_select_service_uuid(void)
{
	apple_rdma_service_uuid = apple_rdma_default_service_uuid;
	if (service_uuid && *service_uuid)
		return uuid_parse(service_uuid, &apple_rdma_service_uuid);
	return 0;
}

static int ardma_register_property_dir(void)
{
	struct tb_property_dir *dir;
	int ret = 0;

	if (!advertise_service)
		return 0;

	dir = tb_property_create_dir(&apple_rdma_service_uuid);
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
	ardma_property_dir = dir;
	pr_info("advertising AD/FA57 service uuid %pUb\n",
		&apple_rdma_service_uuid);
	return 0;
}

static void ardma_unregister_property_dir(void)
{
	if (!ardma_property_dir)
		return;
	tb_unregister_property_dir(apple_rdma_key, ardma_property_dir);
	tb_property_free_dir(ardma_property_dir);
	ardma_property_dir = NULL;
}

static int ardma_ctrl_callback(const void *buf, size_t size, void *data)
{
	int logged;

	atomic_inc(&ardma_ctrl_received);

	logged = atomic_read(&ardma_ctrl_logged);
	if (logged < ARDMA_MAX_CTRL_MSGS) {
		struct ardma_ctrl_log_entry *e;

		e = kmalloc(sizeof(*e), GFP_ATOMIC);
		if (e) {
			unsigned long flags;

			e->size = size;
			e->dump_len = min_t(u32, size, ARDMA_MAX_CTRL_BYTES);
			memcpy(e->dump, buf, e->dump_len);

			spin_lock_irqsave(&ardma_ctrl_lock, flags);
			list_add_tail(&e->list, &ardma_ctrl_list);
			spin_unlock_irqrestore(&ardma_ctrl_lock, flags);

			atomic_inc(&ardma_ctrl_logged);
			pr_info("ctrl[%d]: size=%zu for AD/FA57 uuid\n",
				logged, size);
		}
	}

	return 1;
}

static void ardma_free_ctrl_log(void)
{
	struct ardma_ctrl_log_entry *e, *tmp;
	unsigned long flags;
	LIST_HEAD(free_list);

	spin_lock_irqsave(&ardma_ctrl_lock, flags);
	list_splice_init(&ardma_ctrl_list, &free_list);
	spin_unlock_irqrestore(&ardma_ctrl_lock, flags);

	list_for_each_entry_safe(e, tmp, &free_list, list) {
		list_del(&e->list);
		kfree(e);
	}
}

/* ----- module init/exit ------------------------------------------ */

static int __init ardma_init(void)
{
	int ret;

	ret = ardma_select_service_uuid();
	if (ret) {
		pr_err("invalid service_uuid '%s'\n", service_uuid);
		return ret;
	}

	ardma_debugfs_root = debugfs_create_dir(ARDMA_DRV_NAME, NULL);
	if (IS_ERR(ardma_debugfs_root))
		ardma_debugfs_root = NULL;
	if (ardma_debugfs_root)
		debugfs_create_file("ctrl_log", 0444, ardma_debugfs_root, NULL,
				    &ardma_ctrl_log_fops);

	ret = ardma_register_property_dir();
	if (ret)
		goto err_debugfs;

	INIT_LIST_HEAD(&ardma_protocol_handler.list);
	ardma_protocol_handler.uuid = &apple_rdma_service_uuid;
	ardma_protocol_handler.callback = ardma_ctrl_callback;
	ret = tb_register_protocol_handler(&ardma_protocol_handler);
	if (ret)
		goto err_property;

	ret = ardma_register_ibdev();
	if (ret)
		goto err_protocol;

	ret = tb_register_service_driver(&ardma_service_driver);
	if (ret)
		goto err_ibdev;

	pr_info("loaded, matching Apple AD/FA57 receive_path=%d\n",
		receive_path);
	return 0;

err_ibdev:
	ardma_unregister_ibdev();
err_protocol:
	tb_unregister_protocol_handler(&ardma_protocol_handler);
err_property:
	ardma_unregister_property_dir();
err_debugfs:
	debugfs_remove_recursive(ardma_debugfs_root);
	return ret;
}

static void __exit ardma_exit(void)
{
	tb_unregister_service_driver(&ardma_service_driver);
	ardma_unregister_ibdev();
	tb_unregister_protocol_handler(&ardma_protocol_handler);
	ardma_unregister_property_dir();
	ardma_free_ctrl_log();
	debugfs_remove_recursive(ardma_debugfs_root);
	ida_destroy(&ardma_qpn_ida);
	pr_info("unloaded\n");
}

module_init(ardma_init);
module_exit(ardma_exit);

MODULE_AUTHOR("usb4-rdma project");
MODULE_DESCRIPTION("Experimental Apple RDMA-over-Thunderbolt verbs peer");
MODULE_LICENSE("GPL v2");
