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
 * No atomics, no SRQ, no RDMA-WRITE/READ yet — RC SEND/RECV only.
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
#include <net/addrconf.h>

#include <rdma/ib_verbs.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/uverbs_ioctl.h>

#include "usb4_rdma.h"
#include "wire.h"

#define USB4_RDMA_NPORTS       1
#define USB4_RDMA_UVERBS_ABI   1
#define USB4_RDMA_PAGE_SIZE_CAP \
	(SZ_4K | SZ_2M | SZ_1G)
#define U4_MAX_SGE             4

/* ----- types ------------------------------------------------------- */

struct usb4_rdma_ucontext {
	struct ib_ucontext base;
};

struct usb4_rdma_mr {
	struct ib_mr base;
	struct list_head pd_link;
	u64 user_va;
	u64 length;
	int access_flags;
	int npages;
	struct page **pages;
};

struct usb4_rdma_pd {
	struct ib_pd base;
	struct list_head mrs;
	spinlock_t mr_lock;
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
	int wc_count;
	enum ib_cq_notify_flags notify;
};

struct usb4_rdma_recv_wr {
	struct list_head list;
	u64 wr_id;
	int num_sge;
	struct ib_sge sge[U4_MAX_SGE];
};

struct usb4_rdma_qp {
	struct ib_qp base;
	enum ib_qp_state state;
	struct ib_qp_attr attr;
	int attr_mask;
	u32 send_psn;
	u32 recv_psn;
	spinlock_t recv_lock;
	struct list_head recv_q;	/* of usb4_rdma_recv_wr */
};

struct usb4_rdma_ib_dev {
	struct ib_device base;
	atomic_t active_peers;
};

static struct usb4_rdma_ib_dev *u4r_dev;
static atomic_t u4r_lkey_counter = ATOMIC_INIT(1);

/* ----- helpers: PD ↔ MR lookup ------------------------------------ */

static struct usb4_rdma_mr *
u4r_pd_find_mr_by_lkey(struct usb4_rdma_pd *pd, u32 lkey)
{
	struct usb4_rdma_mr *mr;
	unsigned long flags;

	spin_lock_irqsave(&pd->mr_lock, flags);
	list_for_each_entry(mr, &pd->mrs, pd_link) {
		if (mr->base.lkey == lkey) {
			spin_unlock_irqrestore(&pd->mr_lock, flags);
			return mr;
		}
	}
	spin_unlock_irqrestore(&pd->mr_lock, flags);
	return NULL;
}

/* Copy `len` bytes between `kbuf` and the user-pinned MR pages,
 * starting at user virtual address `vaddr`. Returns 0 on success. */
static int u4r_mr_xfer(struct usb4_rdma_mr *mr, u64 vaddr, void *kbuf,
		       size_t len, bool from_mr_to_kbuf)
{
	u64 offset, page_idx, page_off;
	size_t copied = 0;

	if (vaddr < mr->user_va || vaddr + len > mr->user_va + mr->length)
		return -ERANGE;
	offset = vaddr - mr->user_va;

	while (copied < len) {
		size_t chunk;
		void *page_kva;

		page_idx = (offset + copied) >> PAGE_SHIFT;
		page_off = (offset + copied) & ~PAGE_MASK;
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

/* ----- helpers: CQ enqueue ---------------------------------------- */

static int u4r_cq_push_wc(struct usb4_rdma_cq *cq, const struct ib_wc *wc)
{
	struct usb4_rdma_wc_entry *e;
	unsigned long flags;

	e = kmalloc(sizeof(*e), GFP_ATOMIC);
	if (!e)
		return -ENOMEM;
	e->wc = *wc;

	spin_lock_irqsave(&cq->lock, flags);
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
	list_del(&mr->pd_link);
	spin_unlock_irqrestore(&pd->mr_lock, flags);

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

/* ----- CQ --------------------------------------------------------- */

static int u4r_create_cq(struct ib_cq *ibcq, const struct ib_cq_init_attr *attr,
			 struct uverbs_attr_bundle *attrs)
{
	struct usb4_rdma_cq *cq = container_of(ibcq, struct usb4_rdma_cq, base);

	cq->cqe_capacity = attr->cqe;
	spin_lock_init(&cq->lock);
	INIT_LIST_HEAD(&cq->wc_list);
	cq->wc_count = 0;
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
	list_for_each_entry_safe(e, tmp, &cq->wc_list, list) {
		list_del(&e->list);
		kfree(e);
	}
	cq->wc_count = 0;
	spin_unlock_irqrestore(&cq->lock, flags);
	return 0;
}

static int u4r_poll_cq(struct ib_cq *ibcq, int num_entries, struct ib_wc *wc)
{
	struct usb4_rdma_cq *cq = container_of(ibcq, struct usb4_rdma_cq, base);
	struct usb4_rdma_wc_entry *e, *tmp;
	int n = 0;
	unsigned long flags;

	spin_lock_irqsave(&cq->lock, flags);
	list_for_each_entry_safe(e, tmp, &cq->wc_list, list) {
		if (n >= num_entries)
			break;
		wc[n++] = e->wc;
		list_del(&e->list);
		cq->wc_count--;
		kfree(e);
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

	if (attr->qp_type != IB_QPT_RC)
		return -EOPNOTSUPP;

	qp->state = IB_QPS_RESET;
	qp->send_psn = 0;
	qp->recv_psn = 0;
	spin_lock_init(&qp->recv_lock);
	INIT_LIST_HEAD(&qp->recv_q);

	/* Register with the data layer so RX dispatch finds us by qp_num.
	 * IB core hasn't filled qp->qp_num yet at this point — but
	 * qp->qp_num is the address-of-uobject hash, which is determined.
	 * Defer registration until the first modify_qp INIT. */
	pr_info("create_qp ok (RC, max_send_wr=%u max_recv_wr=%u)\n",
		attr->cap.max_send_wr, attr->cap.max_recv_wr);
	return 0;
}

static int u4r_modify_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr,
			 int attr_mask, struct ib_udata *udata)
{
	struct usb4_rdma_qp *qp = container_of(ibqp, struct usb4_rdma_qp, base);
	enum ib_qp_state old = qp->state;

	if (attr_mask & IB_QP_STATE) {
		qp->state = attr->qp_state;
		pr_info("modify_qp[%u]: %d -> %d\n",
			ibqp->qp_num, old, attr->qp_state);
	}
	if (attr_mask & IB_QP_DEST_QPN) {
		qp->attr.dest_qp_num = attr->dest_qp_num;
		pr_info("modify_qp[%u]: dest_qp = %u\n",
			ibqp->qp_num, attr->dest_qp_num);
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

	if (old == IB_QPS_RESET && qp->state == IB_QPS_INIT)
		usb4_rdma_data_register_qp(ibqp->qp_num, qp);
	if (qp->state == IB_QPS_RESET || qp->state == IB_QPS_ERR)
		usb4_rdma_data_unregister_qp(ibqp->qp_num);
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
	init_attr->qp_type = IB_QPT_RC;
	return 0;
}

static int u4r_destroy_qp(struct ib_qp *ibqp, struct ib_udata *udata)
{
	struct usb4_rdma_qp *qp = container_of(ibqp, struct usb4_rdma_qp, base);
	struct usb4_rdma_recv_wr *r, *tmp;
	unsigned long flags;

	usb4_rdma_data_unregister_qp(ibqp->qp_num);

	spin_lock_irqsave(&qp->recv_lock, flags);
	list_for_each_entry_safe(r, tmp, &qp->recv_q, list) {
		list_del(&r->list);
		kfree(r);
	}
	spin_unlock_irqrestore(&qp->recv_lock, flags);
	return 0;
}

/* ----- post_send -------------------------------------------------- */

/* Build a single wire frame from one WR's SGE list, send it. For now
 * we require the entire WR to fit in one frame (length <= U4_MAX_PAYLOAD).
 * Multi-frame fragmentation comes later. */
static int u4r_send_one(struct usb4_rdma_qp *qp, const struct ib_send_wr *wr)
{
	struct usb4_rdma_pd *pd =
		container_of(qp->base.pd, struct usb4_rdma_pd, base);
	u8 *buf;
	u32 total_len = 0, off = 0;
	int i, ret;

	for (i = 0; i < wr->num_sge; i++)
		total_len += wr->sg_list[i].length;
	if (total_len > U4_MAX_PAYLOAD)
		return -EMSGSIZE;

	buf = kmalloc(U4_MAX_PAYLOAD, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	for (i = 0; i < wr->num_sge; i++) {
		const struct ib_sge *sge = &wr->sg_list[i];
		struct usb4_rdma_mr *mr =
			u4r_pd_find_mr_by_lkey(pd, sge->lkey);
		if (!mr) { ret = -EINVAL; goto out; }
		ret = u4r_mr_xfer(mr, sge->addr, buf + off, sge->length, true);
		if (ret) goto out;
		off += sge->length;
	}

	ret = usb4_rdma_data_send(qp->base.qp_num,
				  qp->attr.dest_qp_num,
				  qp->send_psn++,
				  U4_F_LAST | U4_F_SOLICITED,
				  buf, total_len);
out:
	kfree(buf);
	return ret;
}

static int u4r_post_send(struct ib_qp *ibqp, const struct ib_send_wr *wr,
			 const struct ib_send_wr **bad_wr)
{
	struct usb4_rdma_qp *qp = container_of(ibqp, struct usb4_rdma_qp, base);
	struct usb4_rdma_cq *send_cq =
		container_of(ibqp->send_cq, struct usb4_rdma_cq, base);

	if (qp->state != IB_QPS_RTS && qp->state != IB_QPS_SQD &&
	    qp->state != IB_QPS_SQE) {
		*bad_wr = wr;
		return -EINVAL;
	}

	for (; wr; wr = wr->next) {
		struct ib_wc wc = {};
		int ret;

		if (wr->opcode != IB_WR_SEND) {
			pr_warn("post_send: opcode %d not implemented\n",
				wr->opcode);
			*bad_wr = wr;
			return -EOPNOTSUPP;
		}
		ret = u4r_send_one(qp, wr);
		if (ret) {
			*bad_wr = wr;
			return ret;
		}

		if (wr->send_flags & IB_SEND_SIGNALED) {
			wc.wr_id        = wr->wr_id;
			wc.status       = IB_WC_SUCCESS;
			wc.opcode       = IB_WC_SEND;
			wc.qp           = ibqp;
			wc.byte_len     = 0;
			u4r_cq_push_wc(send_cq, &wc);
		}
	}
	return 0;
}

/* ----- post_recv -------------------------------------------------- */

static int u4r_post_recv(struct ib_qp *ibqp, const struct ib_recv_wr *wr,
			 const struct ib_recv_wr **bad_wr)
{
	struct usb4_rdma_qp *qp = container_of(ibqp, struct usb4_rdma_qp, base);
	unsigned long flags;

	pr_info("post_recv enter qp=%u\n", ibqp->qp_num);
	for (; wr; wr = wr->next) {
		struct usb4_rdma_recv_wr *r;
		int i;

		if (wr->num_sge > U4_MAX_SGE) {
			*bad_wr = wr;
			return -EINVAL;
		}
		r = kmalloc(sizeof(*r), GFP_KERNEL);
		if (!r) {
			*bad_wr = wr;
			return -ENOMEM;
		}
		r->wr_id   = wr->wr_id;
		r->num_sge = wr->num_sge;
		for (i = 0; i < wr->num_sge; i++)
			r->sge[i] = wr->sg_list[i];
		spin_lock_irqsave(&qp->recv_lock, flags);
		list_add_tail(&r->list, &qp->recv_q);
		spin_unlock_irqrestore(&qp->recv_lock, flags);
	}
	return 0;
}

/* ----- RX dispatch (called from data.c softirq context) ----------- */

static void u4r_rx_handler(void *qp_opaque, const struct u4_wire_hdr *hdr,
			   const void *payload, u32 length)
{
	struct usb4_rdma_qp *qp = qp_opaque;
	struct usb4_rdma_pd *pd =
		container_of(qp->base.pd, struct usb4_rdma_pd, base);
	struct usb4_rdma_cq *recv_cq =
		container_of(qp->base.recv_cq, struct usb4_rdma_cq, base);
	struct usb4_rdma_recv_wr *r;
	unsigned long flags;
	struct ib_wc wc = {};
	u32 written = 0;
	int i;

	spin_lock_irqsave(&qp->recv_lock, flags);
	r = list_first_entry_or_null(&qp->recv_q, struct usb4_rdma_recv_wr,
				     list);
	if (r)
		list_del(&r->list);
	spin_unlock_irqrestore(&qp->recv_lock, flags);
	if (!r) {
		pr_warn_ratelimited("rx[qp=%u]: no pending recv WR, dropping %u bytes\n",
				    qp->base.qp_num, length);
		return;
	}

	for (i = 0; i < r->num_sge && written < length; i++) {
		struct ib_sge *sge = &r->sge[i];
		struct usb4_rdma_mr *mr =
			u4r_pd_find_mr_by_lkey(pd, sge->lkey);
		u32 chunk;

		if (!mr) {
			wc.status = IB_WC_LOC_PROT_ERR;
			goto done;
		}
		chunk = min(sge->length, length - written);
		if (u4r_mr_xfer(mr, sge->addr,
				(void *)payload + written, chunk, false)) {
			wc.status = IB_WC_LOC_PROT_ERR;
			goto done;
		}
		written += chunk;
	}

	wc.status   = IB_WC_SUCCESS;
done:
	wc.wr_id    = r->wr_id;
	wc.opcode   = IB_WC_RECV;
	wc.qp       = &qp->base;
	wc.byte_len = written;
	wc.src_qp   = le32_to_cpu(hdr->src_qp);

	u4r_cq_push_wc(recv_cq, &wc);
	kfree(r);
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
	attr->max_mr_size         = ~0ull;
	attr->page_size_cap       = USB4_RDMA_PAGE_SIZE_CAP;
	attr->max_qp              = 256;
	attr->max_qp_wr           = 1024;
	attr->max_send_sge        = U4_MAX_SGE;
	attr->max_recv_sge        = U4_MAX_SGE;
	attr->max_sge_rd          = 0;
	attr->max_cq              = 256;
	attr->max_cqe             = 4096;
	attr->max_mr              = 1024;
	attr->max_pd              = 256;
	attr->max_qp_rd_atom      = 0;
	attr->max_res_rd_atom     = 0;
	attr->max_qp_init_rd_atom = 0;
	attr->atomic_cap          = IB_ATOMIC_NONE;
	attr->max_ah              = 16;
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
	attr->gid_tbl_len    = 1;
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
	immutable->core_cap_flags = 0;
	immutable->max_mad_size = 0;
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

static int u4r_query_gid(struct ib_device *ibdev, u32 port, int idx,
			 union ib_gid *gid)
{
	if (port != 1 || idx > 0)
		return -EINVAL;
	memset(gid, 0, sizeof(*gid));
	memcpy(gid->raw + 8, &ibdev->node_guid, 8);
	return 0;
}

static enum rdma_link_layer u4r_get_link_layer(struct ib_device *ibdev,
					       u32 port_num)
{
	return IB_LINK_LAYER_ETHERNET;
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
	.query_gid         = u4r_query_gid,
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
	INIT_RDMA_OBJ_SIZE(ib_pd,        usb4_rdma_pd,       base),
	INIT_RDMA_OBJ_SIZE(ib_cq,        usb4_rdma_cq,       base),
	INIT_RDMA_OBJ_SIZE(ib_qp,        usb4_rdma_qp,       base),
};

void usb4_rdma_ibdev_peer_event(bool joined)
{
	int n;

	if (!u4r_dev)
		return;
	if (joined)
		n = atomic_inc_return(&u4r_dev->active_peers);
	else
		n = atomic_dec_return(&u4r_dev->active_peers);
	pr_info("peer %s, %d active — port 1 %s\n",
		joined ? "joined" : "left", n,
		n > 0 ? "ACTIVE" : "DOWN");
}

int usb4_rdma_ibdev_init(void)
{
	struct usb4_rdma_ib_dev *u4r;
	u8 mac[ETH_ALEN];
	int err;

	u4r = ib_alloc_device(usb4_rdma_ib_dev, base);
	if (!u4r)
		return -ENOMEM;
	atomic_set(&u4r->active_peers, 0);
	u4r->base.phys_port_cnt    = USB4_RDMA_NPORTS;
	u4r->base.num_comp_vectors = num_possible_cpus();
	u4r->base.local_dma_lkey   = 0;
	u4r->base.node_type        = RDMA_NODE_RNIC;
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

	eth_random_addr(mac);
	addrconf_addr_eui48((u8 *)&u4r->base.node_guid, mac);

	ib_set_device_ops(&u4r->base, &u4r_dev_ops);

	err = ib_register_device(&u4r->base, "usb4_rdma%d", NULL);
	if (err) {
		pr_err("ib_register_device failed: %d\n", err);
		ib_dealloc_device(&u4r->base);
		return err;
	}

	u4r_dev = u4r;
	usb4_rdma_data_set_rx_handler(u4r_rx_handler);
	pr_info("registered ib_device %s (1 port)\n",
		dev_name(&u4r->base.dev));
	return 0;
}

void usb4_rdma_ibdev_exit(void)
{
	if (!u4r_dev)
		return;
	usb4_rdma_data_set_rx_handler(NULL);
	ib_unregister_device(&u4r_dev->base);
	ib_dealloc_device(&u4r_dev->base);
	u4r_dev = NULL;
}
