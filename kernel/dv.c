// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

/*
 * UVERBS_MODULE_NAME must be defined before including
 * rdma/uverbs_named_ioctl.h; the macro is used in the DECLARE_UVERBS_*
 * helpers below to namespace generated symbols.
 */
#define UVERBS_MODULE_NAME tbv

#include <linux/build_bug.h>
#include <linux/cacheflush.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <rdma/ib_umem.h>
#include <rdma/ib_verbs.h>
#include <rdma/uverbs_ioctl.h>
#include <rdma/uverbs_named_ioctl.h>
#include <rdma/uverbs_std_types.h>
#include <rdma/uverbs_types.h>

#include "../userspace/usb4_rdma/usb4_rdma_dv.h"
#include "tbv.h"

/*
 * USB4 RDMA Direct Verbs (DV) ABI surface.
 *
 * The DV ABI is the software-RNIC contract used by the upcoming GDA path: the
 * GPU produces work queue entries into host-visible coherent memory and the
 * kernel poll worker consumes them. See userspace/usb4_rdma/usb4_rdma_dv.h
 * for the full design, including memory-ordering rules and the generation
 * protocol.
 *
 * Methods wired so far: QUERY_CAPS, CREATE_QUEUE, DESTROY_QUEUE. KICK and
 * the actual WQE consumer follow in subsequent commits; until they land,
 * caps stays 0 to advertise that no transport opcodes are processed.
 */

/* ----- per-QP lifecycle helpers (called from ibdev.c) ----------------- */

void tbv_dv_qp_state_init(struct tbv_dv_qp_state *dv)
{
	mutex_init(&dv->mutex);
	dv->active = false;
	dv->generation = 0;
	dv->sq_entries = 0;
	dv->cq_entries = 0;
	dv->sq_addr = 0;
	dv->cq_addr = 0;
	dv->doorbell_addr = 0;
	dv->sq_umem = NULL;
	dv->cq_umem = NULL;
	dv->doorbell_umem = NULL;
}

/*
 * Generation policy: u8 monotonic counter, skipping 0 so a freshly
 * zero-initialized doorbell page cannot be mistaken for a live generation.
 * 8 bits matches USB4_RDMA_DV_TAIL_GENERATION_BITS == 8 — see the packed
 * tail format in the ABI header.
 */
static u8 tbv_dv_next_generation(u8 generation)
{
	u8 next = (generation + 1) & 0xff;

	return next ? next : 1;
}

bool tbv_dv_qp_state_active(struct tbv_dv_qp_state *dv)
{
	return READ_ONCE(dv->active);
}

/* Release pinned umem regions and reset state to idle. */
static void tbv_dv_state_release_locked(struct tbv_dv_qp_state *dv)
{
	if (dv->sq_umem)
		ib_umem_release(dv->sq_umem);
	if (dv->cq_umem)
		ib_umem_release(dv->cq_umem);
	if (dv->doorbell_umem)
		ib_umem_release(dv->doorbell_umem);
	dv->sq_umem = NULL;
	dv->cq_umem = NULL;
	dv->doorbell_umem = NULL;
	dv->sq_entries = 0;
	dv->cq_entries = 0;
	dv->sq_addr = 0;
	dv->cq_addr = 0;
	dv->doorbell_addr = 0;
	WRITE_ONCE(dv->active, false);
}

void tbv_dv_qp_state_teardown(struct tbv_dv_qp_state *dv)
{
	mutex_lock(&dv->mutex);
	if (dv->active) {
		dv->generation = tbv_dv_next_generation(dv->generation);
		tbv_dv_state_release_locked(dv);
	}
	mutex_unlock(&dv->mutex);
	mutex_destroy(&dv->mutex);
}

/* ----- request validation and queue pinning --------------------------- */

static bool tbv_dv_reserved_zero(const u32 *reserved, size_t count)
{
	size_t i;

	for (i = 0; i < count; i++)
		if (reserved[i])
			return false;
	return true;
}

static int tbv_dv_validate_queue_create(
	const struct usb4_rdma_dv_queue_create *req)
{
	u64 sq_bytes;
	u64 cq_bytes;
	u64 end;

	if (req->abi_version != USB4_RDMA_DV_ABI_VERSION)
		return -EINVAL;
	if (req->flags)
		return -EINVAL;
	if (!tbv_dv_reserved_zero(req->reserved, ARRAY_SIZE(req->reserved)))
		return -EINVAL;
	if (req->sq_entries < USB4_RDMA_DV_MIN_QUEUE_ENTRIES ||
	    req->sq_entries > USB4_RDMA_DV_MAX_SQ_ENTRIES ||
	    req->cq_entries < USB4_RDMA_DV_MIN_QUEUE_ENTRIES ||
	    req->cq_entries > USB4_RDMA_DV_MAX_CQ_ENTRIES)
		return -EINVAL;
	if (req->sq_stride != USB4_RDMA_DV_WQE_SIZE ||
	    req->cq_stride != USB4_RDMA_DV_CQE_SIZE)
		return -EINVAL;
	if (!req->sq_addr || !req->cq_addr || !req->doorbell_addr)
		return -EINVAL;
	if (!IS_ALIGNED(req->sq_addr, USB4_RDMA_DV_WQE_SIZE) ||
	    !IS_ALIGNED(req->cq_addr, USB4_RDMA_DV_CQE_SIZE) ||
	    !IS_ALIGNED(req->doorbell_addr, USB4_RDMA_DV_DOORBELL_PAGE_SIZE))
		return -EINVAL;
	if (check_mul_overflow((u64)req->sq_entries, (u64)req->sq_stride,
			       &sq_bytes) ||
	    check_add_overflow(req->sq_addr, sq_bytes, &end))
		return -EINVAL;
	if (check_mul_overflow((u64)req->cq_entries, (u64)req->cq_stride,
			       &cq_bytes) ||
	    check_add_overflow(req->cq_addr, cq_bytes, &end))
		return -EINVAL;
	if (check_add_overflow(req->doorbell_addr,
			       (u64)USB4_RDMA_DV_DOORBELL_PAGE_SIZE, &end))
		return -EINVAL;
	return 0;
}

struct tbv_dv_pin_result {
	struct ib_umem *sq_umem;
	struct ib_umem *cq_umem;
	struct ib_umem *doorbell_umem;
};

static void tbv_dv_pin_release(struct tbv_dv_pin_result *res)
{
	if (res->sq_umem)
		ib_umem_release(res->sq_umem);
	if (res->cq_umem)
		ib_umem_release(res->cq_umem);
	if (res->doorbell_umem)
		ib_umem_release(res->doorbell_umem);
	memset(res, 0, sizeof(*res));
}

static int tbv_dv_pin_queue(struct ib_device *dev,
			    const struct usb4_rdma_dv_queue_create *req,
			    struct tbv_dv_pin_result *res)
{
	u64 sq_bytes = (u64)req->sq_entries * req->sq_stride;
	u64 cq_bytes = (u64)req->cq_entries * req->cq_stride;
	int access = IB_ACCESS_LOCAL_WRITE;

	memset(res, 0, sizeof(*res));

	res->sq_umem = ib_umem_get(dev, req->sq_addr, sq_bytes, access);
	if (IS_ERR(res->sq_umem)) {
		int ret = PTR_ERR(res->sq_umem);

		res->sq_umem = NULL;
		return ret;
	}

	res->cq_umem = ib_umem_get(dev, req->cq_addr, cq_bytes, access);
	if (IS_ERR(res->cq_umem)) {
		int ret = PTR_ERR(res->cq_umem);

		res->cq_umem = NULL;
		tbv_dv_pin_release(res);
		return ret;
	}

	res->doorbell_umem = ib_umem_get(dev, req->doorbell_addr,
					 USB4_RDMA_DV_DOORBELL_PAGE_SIZE,
					 access);
	if (IS_ERR(res->doorbell_umem)) {
		int ret = PTR_ERR(res->doorbell_umem);

		res->doorbell_umem = NULL;
		tbv_dv_pin_release(res);
		return ret;
	}

	return 0;
}

/*
 * Write `len` bytes from `src` into pinned userspace pages at
 * umem-relative `offset`. The kernel does not export a public
 * `ib_umem_copy_to` (only `_from`), so we walk the umem's scatterlist
 * and copy page by page. The doorbell page is the only thing this
 * helper writes today; the access window is always small and within
 * the pinned range.
 */
static int tbv_dv_umem_write(struct ib_umem *umem, size_t offset,
			     const void *src, size_t len)
{
	struct sg_table *sgt = &umem->sgt_append.sgt;
	struct scatterlist *sg;
	size_t copied = 0;
	size_t skip;
	unsigned int i;

	if (!len)
		return 0;
	if (offset + len < offset)
		return -EINVAL;
	if (offset + len > umem->length)
		return -EFAULT;

	skip = ib_umem_offset(umem) + offset;
	for_each_sg(sgt->sgl, sg, sgt->nents, i) {
		size_t seg_len = sg->length;
		size_t seg_off;

		if (skip >= seg_len) {
			skip -= seg_len;
			continue;
		}

		seg_off = sg->offset + skip;
		seg_len -= skip;
		skip = 0;

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
			memcpy((u8 *)kaddr + page_off,
			       (const u8 *)src + copied, chunk);
			flush_dcache_page(page);
			kunmap_local(kaddr);

			copied += chunk;
			seg_off += chunk;
			seg_len -= chunk;
		}

		if (copied == len)
			break;
	}

	return copied == len ? 0 : -EFAULT;
}

/*
 * Write the consumer cacheline of the doorbell page so the GPU side sees
 * a known initial (or terminal) state. Used after CREATE_QUEUE to publish
 * generation=X / qp_state=LIVE and during DESTROY_QUEUE to publish the
 * QP_DEAD sentinel with a bumped generation.
 *
 * The doorbell page is pinned at offset 0 of its umem, so we use the
 * umem-relative offset of the consumer line directly; no base-address
 * translation is needed.
 */
static int tbv_dv_write_consumer_line(struct ib_umem *umem,
				      u32 sq_head, u32 cq_tail,
				      u32 qp_state, u32 generation)
{
	struct usb4_rdma_dv_doorbell_consumer_line line = {
		.sq_head = sq_head,
		.cq_tail = cq_tail,
		.qp_state = qp_state,
		.generation = generation,
	};

	return tbv_dv_umem_write(
		umem, offsetof(struct usb4_rdma_dv_doorbell, consumer),
		&line, sizeof(line));
}

/* ----- uverbs methods ------------------------------------------------- */

static int UVERBS_HANDLER(USB4_RDMA_DV_METHOD_QUERY_CAPS)(
	struct uverbs_attr_bundle *attrs)
{
	struct usb4_rdma_dv_query_caps_resp resp = {
		.abi_version = USB4_RDMA_DV_ABI_VERSION,
		/*
		 * No transport opcodes are wired through the DV consumer yet,
		 * so we advertise an empty capability bitmap. Each subsequent
		 * commit that enables a real WQE opcode will OR the matching
		 * USB4_RDMA_DV_CAP_* bit in here so userspace can detect what
		 * the kernel will actually consume.
		 */
		.caps = 0,
		.max_sq_entries = USB4_RDMA_DV_MAX_SQ_ENTRIES,
		.max_cq_entries = USB4_RDMA_DV_MAX_CQ_ENTRIES,
		.default_sq_entries = USB4_RDMA_DV_DEFAULT_SQ_ENTRIES,
		.default_cq_entries = USB4_RDMA_DV_DEFAULT_CQ_ENTRIES,
		.wqe_size = USB4_RDMA_DV_WQE_SIZE,
		.cqe_size = USB4_RDMA_DV_CQE_SIZE,
		.doorbell_record_size = USB4_RDMA_DV_DOORBELL_RECORD_SIZE,
		.doorbell_page_size = USB4_RDMA_DV_DOORBELL_PAGE_SIZE,
		.tail_index_bits = USB4_RDMA_DV_TAIL_INDEX_BITS,
		.tail_generation_bits = USB4_RDMA_DV_TAIL_GENERATION_BITS,
	};
	struct ib_ucontext *ib_uctx;

	/*
	 * Compile-time guards: the ABI sizes that userspace receives in the
	 * QUERY_CAPS response are the same sizes the future producer/consumer
	 * code will assume for the on-the-wire-via-memory structs. If a
	 * struct field is added without updating the corresponding _SIZE
	 * constant (and bumping the ABI version), userspace and kernel will
	 * silently disagree about the layout. Catch that at build time.
	 */
	BUILD_BUG_ON(sizeof(struct usb4_rdma_dv_wqe) !=
		     USB4_RDMA_DV_WQE_SIZE);
	BUILD_BUG_ON(sizeof(struct usb4_rdma_dv_cqe) !=
		     USB4_RDMA_DV_CQE_SIZE);
	BUILD_BUG_ON(sizeof(struct usb4_rdma_dv_doorbell_producer_line) !=
		     USB4_RDMA_DV_DOORBELL_LINE_SIZE);
	BUILD_BUG_ON(sizeof(struct usb4_rdma_dv_doorbell_consumer_line) !=
		     USB4_RDMA_DV_DOORBELL_LINE_SIZE);
	BUILD_BUG_ON(sizeof(struct usb4_rdma_dv_doorbell) !=
		     USB4_RDMA_DV_DOORBELL_RECORD_SIZE);

	ib_uctx = ib_uverbs_get_ucontext(attrs);
	if (IS_ERR(ib_uctx))
		return PTR_ERR(ib_uctx);
	if (!tbv_ibdev_state(ib_uctx->device))
		return -ENODEV;

	return uverbs_copy_to(attrs, USB4_RDMA_DV_ATTR_QUERY_CAPS_RESP,
			      &resp, sizeof(resp));
}

static int UVERBS_HANDLER(USB4_RDMA_DV_METHOD_CREATE_QUEUE)(
	struct uverbs_attr_bundle *attrs)
{
	struct usb4_rdma_dv_queue_create req = {};
	struct usb4_rdma_dv_queue_resp resp = {};
	struct tbv_dv_pin_result res = {};
	struct tbv_dv_qp_state *dv;
	struct ib_qp *ibqp;
	struct tbv_qp *tqp;
	u8 generation;
	int ret;

	ibqp = uverbs_attr_get_obj(attrs, USB4_RDMA_DV_ATTR_CREATE_QUEUE_QP);
	if (IS_ERR(ibqp))
		return PTR_ERR(ibqp);

	ret = uverbs_copy_from(&req, attrs,
			       USB4_RDMA_DV_ATTR_CREATE_QUEUE_REQ);
	if (ret)
		return ret;

	ret = tbv_dv_validate_queue_create(&req);
	if (ret)
		return ret;

	ret = tbv_dv_pin_queue(ibqp->device, &req, &res);
	if (ret)
		return ret;

	tqp = tbv_qp_from_ibqp(ibqp);
	dv = tbv_qp_dv_state(tqp);

	mutex_lock(&dv->mutex);
	if (dv->active) {
		mutex_unlock(&dv->mutex);
		tbv_dv_pin_release(&res);
		return -EBUSY;
	}

	generation = tbv_dv_next_generation(dv->generation);
	dv->generation = generation;
	dv->sq_entries = req.sq_entries;
	dv->cq_entries = req.cq_entries;
	dv->sq_addr = req.sq_addr;
	dv->cq_addr = req.cq_addr;
	dv->doorbell_addr = req.doorbell_addr;
	dv->sq_umem = res.sq_umem;
	dv->cq_umem = res.cq_umem;
	dv->doorbell_umem = res.doorbell_umem;
	memset(&res, 0, sizeof(res));
	WRITE_ONCE(dv->active, true);

	/*
	 * Publish the LIVE state to the doorbell consumer line with packed
	 * (index=0, generation) tail words. The producer side mirrors this
	 * generation into its own line before issuing the first WQE.
	 */
	ret = tbv_dv_write_consumer_line(
		dv->doorbell_umem,
		usb4_rdma_dv_tail_pack(0, generation),
		usb4_rdma_dv_tail_pack(0, generation),
		USB4_RDMA_DV_QP_LIVE, generation);
	if (ret)
		goto err_release_locked;

	resp.qp_num = ibqp->qp_num;
	resp.generation = generation;

	ret = uverbs_copy_to(attrs, USB4_RDMA_DV_ATTR_CREATE_QUEUE_RESP,
			     &resp, sizeof(resp));
	if (ret)
		goto err_release_locked;

	mutex_unlock(&dv->mutex);
	return 0;

err_release_locked:
	dv->generation = tbv_dv_next_generation(dv->generation);
	tbv_dv_state_release_locked(dv);
	mutex_unlock(&dv->mutex);
	return ret;
}

static int UVERBS_HANDLER(USB4_RDMA_DV_METHOD_DESTROY_QUEUE)(
	struct uverbs_attr_bundle *attrs)
{
	struct tbv_dv_qp_state *dv;
	struct ib_umem *doorbell_umem = NULL;
	struct ib_qp *ibqp;
	struct tbv_qp *tqp;
	u8 generation = 0;
	int ret = 0;

	ibqp = uverbs_attr_get_obj(attrs, USB4_RDMA_DV_ATTR_DESTROY_QUEUE_QP);
	if (IS_ERR(ibqp))
		return PTR_ERR(ibqp);

	tqp = tbv_qp_from_ibqp(ibqp);
	dv = tbv_qp_dv_state(tqp);

	mutex_lock(&dv->mutex);
	if (!dv->active) {
		mutex_unlock(&dv->mutex);
		return -ENOENT;
	}

	/*
	 * Bump generation before releasing so any stale producer write
	 * landing on the now-defunct doorbell page is detectable. The
	 * QP_DEAD sentinel + bumped generation are the contract documented
	 * in the ABI header.
	 */
	generation = tbv_dv_next_generation(dv->generation);
	dv->generation = generation;

	/* Keep one reference to the doorbell umem so we can write the
	 * sentinel after releasing the rest. */
	doorbell_umem = dv->doorbell_umem;
	dv->doorbell_umem = NULL;

	tbv_dv_state_release_locked(dv);
	mutex_unlock(&dv->mutex);

	if (doorbell_umem) {
		ret = tbv_dv_write_consumer_line(
			doorbell_umem,
			usb4_rdma_dv_tail_pack(0, generation),
			usb4_rdma_dv_tail_pack(0, generation),
			USB4_RDMA_DV_QP_DEAD, generation);
		ib_umem_release(doorbell_umem);
		/*
		 * A late write failure here is not surfaced to userspace
		 * because the queue has already been logically torn down;
		 * the only effect is that the GPU may not observe the DEAD
		 * sentinel through this doorbell page. The bumped generation
		 * in the next CREATE_QUEUE response is sufficient to keep
		 * generation-checking consumers safe.
		 */
		(void)ret;
		ret = 0;
	}

	return ret;
}

DECLARE_UVERBS_NAMED_METHOD(
	USB4_RDMA_DV_METHOD_QUERY_CAPS,
	UVERBS_ATTR_PTR_OUT(
		USB4_RDMA_DV_ATTR_QUERY_CAPS_RESP,
		UVERBS_ATTR_STRUCT(struct usb4_rdma_dv_query_caps_resp,
				   reserved),
		UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(
	USB4_RDMA_DV_METHOD_CREATE_QUEUE,
	UVERBS_ATTR_IDR(USB4_RDMA_DV_ATTR_CREATE_QUEUE_QP,
			UVERBS_OBJECT_QP,
			UVERBS_ACCESS_READ,
			UA_MANDATORY),
	UVERBS_ATTR_PTR_IN(
		USB4_RDMA_DV_ATTR_CREATE_QUEUE_REQ,
		UVERBS_ATTR_STRUCT(struct usb4_rdma_dv_queue_create, reserved),
		UA_MANDATORY),
	UVERBS_ATTR_PTR_OUT(
		USB4_RDMA_DV_ATTR_CREATE_QUEUE_RESP,
		UVERBS_ATTR_STRUCT(struct usb4_rdma_dv_queue_resp, reserved0),
		UA_MANDATORY));

DECLARE_UVERBS_NAMED_METHOD(
	USB4_RDMA_DV_METHOD_DESTROY_QUEUE,
	UVERBS_ATTR_IDR(USB4_RDMA_DV_ATTR_DESTROY_QUEUE_QP,
			UVERBS_OBJECT_QP,
			UVERBS_ACCESS_READ,
			UA_MANDATORY));

DECLARE_UVERBS_GLOBAL_METHODS(
	USB4_RDMA_DV_OBJECT_DEVICE,
	&UVERBS_METHOD(USB4_RDMA_DV_METHOD_QUERY_CAPS),
	&UVERBS_METHOD(USB4_RDMA_DV_METHOD_CREATE_QUEUE),
	&UVERBS_METHOD(USB4_RDMA_DV_METHOD_DESTROY_QUEUE));

const struct uapi_definition tbv_uapi_defs[] = {
	UAPI_DEF_CHAIN_OBJ_TREE_NAMED(USB4_RDMA_DV_OBJECT_DEVICE),
	{},
};
