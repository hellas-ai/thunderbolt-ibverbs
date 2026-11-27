// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * usb4_rdma userspace verbs provider — Tier 1 skeleton.
 *
 * Tracks the kernel-side ibdev.c: most data-plane verbs return
 * the kernel's -ENOSYS straight to the application. Just enough
 * here for `ibv_devinfo` to find and open the device, and for
 * `ibv_alloc_pd`/`ibv_dealloc_pd` to round-trip through the
 * generic uverbs ioctl machinery.
 *
 * Matching: prefer the fixed module node GUIDs, with the kernel device names
 * as a fallback. We do not have an upstream RDMA_DRIVER_USB4_RDMA enum value
 * yet, and distro rdma-core udev rules may rename the devices away from
 * usb4_rdmaN/usb4_appleN.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "usb4_rdma.h"
#include "tbv_cq_shm.h"

#define USB4_RDMA_NODE_GUID  0x0200544256524253ULL
#define USB4_APPLE_NODE_GUID 0x0200544256524254ULL

static void usb4_rdma_free_context(struct ibv_context *ibv_ctx);

/* ----- query stubs ------------------------------------------------ */

static int usb4_rdma_query_device(struct ibv_context *ctx,
				  const struct ibv_query_device_ex_input *in,
				  struct ibv_device_attr_ex *attr,
				  size_t attr_size)
{
	struct ib_uverbs_ex_query_device_resp resp;
	size_t resp_size = sizeof(resp);

	return ibv_cmd_query_device_any(ctx, in, attr, attr_size,
					&resp, &resp_size);
}

static int usb4_rdma_query_port(struct ibv_context *ctx, uint8_t port,
				struct ibv_port_attr *attr)
{
	struct ibv_query_port cmd;

	return ibv_cmd_query_port(ctx, port, attr, &cmd, sizeof(cmd));
}

/* ----- pd ---------------------------------------------------------- */

static struct ibv_pd *usb4_rdma_alloc_pd(struct ibv_context *ctx)
{
	struct ib_uverbs_alloc_pd_resp resp;
	struct ibv_alloc_pd cmd;
	struct usb4_rdma_pd *pd;
	int rv;

	pd = calloc(1, sizeof(*pd));
	if (!pd)
		return NULL;

	rv = ibv_cmd_alloc_pd(ctx, &pd->base, &cmd, sizeof(cmd),
			      &resp, sizeof(resp));
	if (rv) {
		free(pd);
		errno = rv;
		return NULL;
	}
	return &pd->base;
}

static int usb4_rdma_dealloc_pd(struct ibv_pd *base_pd)
{
	struct usb4_rdma_pd *pd =
		container_of(base_pd, struct usb4_rdma_pd, base);
	int rv;

	rv = ibv_cmd_dealloc_pd(base_pd);
	if (rv)
		return rv;
	free(pd);
	return 0;
}

/* ----- cq ---------------------------------------------------------- */

/*
 * Userspace-mapped CQ (Phase 1 of the userspace-mapped-queues work).
 *
 * When the kernel offers it (a vendor resp tail with cq_mmap_offset != 0),
 * the provider maps the CQ's shared page and polls it lock-free: an acquire
 * load of the tail, read the entries, a release store of the head. That is
 * the whole poll -- zero ioctls, where the generic path issues one per call
 * (measured: ~96 ioctls per pingpong iteration, 2026-09-12).
 *
 * When the kernel offers nothing (old kernel) or the map fails, the cq keeps
 * hdr == NULL and every call falls through to the generic ibv_cmd_* path --
 * byte-identical with the pre-mmap provider.
 */

struct usb4_rdma_cq {
	struct ibv_cq base_cq;
	struct tbv_cq_shm *hdr;
	/* The ring carries struct ib_uverbs_wc, the uapi wire format -- NOT the
	 * kernel's struct ib_wc (which holds pointers) and not userspace's
	 * struct ibv_wc. Sharing either side's internal struct across the
	 * boundary is what made the first version return exit 17 with zero
	 * iterations: the provider read a kernel pointer as qp_num. */
	struct ib_uverbs_wc *ring;
	size_t map_len;
	/*
	 * Consumer-private cursor. The shared state is a pair of monotonic
	 * totals (see proto/tbv_cq_shm.h); this side keeps only where it has
	 * read up to, so no shared field has two writers. `head` is the ring
	 * index and `consumed` the total taken, and they advance together.
	 */
	uint32_t head;
	uint32_t consumed;
	uint32_t ovf_seen; /* hdr->ovf_seq values already reported */
	/*
	 * TBV_SHM_DEBUG diagnostics. On this driver the shared-page path is
	 * invisible when it goes wrong -- a failure arrives at the application
	 * as a bare negative return from ibv_poll_cq, with nothing to say
	 * whether the ring was empty, the kernel never published, or the
	 * kernel published and then declared overflow. These counters and
	 * dumps exist so the failure can be DESCRIBED rather than guessed at.
	 */
	unsigned dbg_polls;
	unsigned dbg_eio;
};

/* Mirror of the kernel's struct tbv_uresp_create_cq, which is written at the
 * response base -- i.e. immediately after the generic ib_uverbs_create_cq_resp
 * this provider declares first. Every field must stay in step with
 * proto/tbv_cq_shm.h or the two sides disagree silently. */
struct usb4_rdma_create_cq_resp {
	struct ib_uverbs_create_cq_resp ibv_resp;
	__aligned_u64 cq_mmap_offset;
	uint32_t cqe;
	uint32_t shm_abi;
	uint32_t reserved;
	uint32_t pad;
	__aligned_u64 map_len;
};

static bool usb4_rdma_shm_debug(void)
{
	static int on = -1;

	if (on < 0)
		on = getenv("TBV_SHM_DEBUG") != NULL;
	return on;
}

/* Dump the shared header as the CONSUMER sees it. */
static void usb4_rdma_dump_shm(const char *what, const struct usb4_rdma_cq *cq)
{
	const struct tbv_cq_shm *h = cq->hdr;

	if (!usb4_rdma_shm_debug())
		return;
	fprintf(stderr,
		"[shm] %s: mapped=%d magic=%#x abi=%u cqe=%u entry_size=%u "
		"ring_offset=%u produced=%u consumed=%u ovf_seq=%u map_len=%u "
		"[consumer head=%u consumed=%u ovf_seen=%u] "
		"dbg_polls=%u dbg_eio=%u\n",
		what, h != NULL,
		h ? h->magic : 0, h ? h->abi : 0, h ? h->cqe : 0,
		h ? h->entry_size : 0, h ? h->ring_offset : 0,
		h ? h->produced : 0, h ? h->consumed : 0,
		h ? h->ovf_seq : 0, h ? h->map_len : 0,
		cq->head, cq->consumed, cq->ovf_seen,
		cq->dbg_polls, cq->dbg_eio);
	fflush(stderr);
}

static struct ibv_cq *usb4_rdma_create_cq(struct ibv_context *ctx, int num_cqe,
					  struct ibv_comp_channel *channel,
					  int comp_vector)
{
	struct usb4_rdma_create_cq_resp resp = {};
	struct ibv_create_cq cmd = {};
	struct usb4_rdma_cq *cq;
	int rv;

	cq = calloc(1, sizeof(*cq));
	if (!cq)
		return NULL;

	rv = ibv_cmd_create_cq(ctx, num_cqe, channel, comp_vector, &cq->base_cq,
			       &cmd, sizeof(cmd), &resp.ibv_resp, sizeof(resp));
	if (rv) {
		free(cq);
		errno = rv;
		return NULL;
	}

	if (usb4_rdma_shm_debug())
		fprintf(stderr,
			"[shm] create_cq: requested=%d offered_offset=%#llx "
			"offered_cqe=%u offered_abi=%u offered_len=%llu\n",
			num_cqe, (unsigned long long)resp.cq_mmap_offset,
			resp.cqe, resp.shm_abi,
			(unsigned long long)resp.map_len);
	/*
	 * The offer test keys on shm_abi, NOT on the offset being nonzero. 0 is
	 * a legitimate offset -- measured: the first CQ this leg created was
	 * offered exactly 0 -- so the old `if (resp.cq_mmap_offset && ...)`
	 * silently discarded a real offer, and one of the two CQs perftest
	 * creates never used the shared ring at all.
	 *
	 * ONE mmap for the whole buffer: the kernel publishes map_len in the
	 * response. The first version mapped a single page to discover the
	 * length and then remapped, and the kernel rejected that first call
	 * outright, so the feature never engaged and every poll was an ioctl.
	 */
	if (resp.shm_abi == TBV_CQ_SHM_ABI && resp.map_len) {
		size_t len = resp.map_len;
		void *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED,
			       ctx->cmd_fd, resp.cq_mmap_offset);

		if (m == MAP_FAILED) {
			if (usb4_rdma_shm_debug())
				fprintf(stderr,
					"[shm] create_cq: mmap(len=%zu, off=%#llx) failed: %s\n",
					len,
					(unsigned long long)resp.cq_mmap_offset,
					strerror(errno));
		} else {
			struct tbv_cq_shm *hdr = m;

			if (hdr->magic == TBV_CQ_SHM_MAGIC &&
			    hdr->abi == TBV_CQ_SHM_ABI &&
			    hdr->cqe > 0 &&
			    hdr->entry_size == sizeof(struct ib_uverbs_wc) &&
			    hdr->map_len == len &&
			    hdr->ring_offset +
				    (size_t)hdr->cqe * sizeof(struct ib_uverbs_wc) <= len) {
				cq->hdr = hdr;
				cq->ring = (struct ib_uverbs_wc *)
					((char *)m + hdr->ring_offset);
				cq->map_len = len;
				cq->consumed = hdr->consumed;
			} else {
				if (usb4_rdma_shm_debug())
					fprintf(stderr,
						"[shm] create_cq: header REJECTED "
						"magic=%#x abi=%u cqe=%u entry_size=%u "
						"ring_offset=%u map_len=%u "
						"want_len=%zu want_entry=%zu\n",
						hdr->magic, hdr->abi, hdr->cqe,
						hdr->entry_size, hdr->ring_offset,
						hdr->map_len, len,
						sizeof(struct ib_uverbs_wc));
				munmap(m, len);
			}
		}
	}
	usb4_rdma_dump_shm("create_cq", cq);
	return &cq->base_cq;
}

static int usb4_rdma_destroy_cq(struct ibv_cq *base_cq)
{
	struct usb4_rdma_cq *cq =
		container_of(base_cq, struct usb4_rdma_cq, base_cq);
	int rv;

	if (cq->hdr)
		munmap(cq->hdr, cq->map_len);
	rv = ibv_cmd_destroy_cq(base_cq);
	if (rv)
		return rv;
	free(cq);
	return 0;
}

/* ----- qp ---------------------------------------------------------- */

static struct ibv_qp *usb4_rdma_create_qp(struct ibv_pd *pd,
					  struct ibv_qp_init_attr *attr)
{
	struct ib_uverbs_create_qp_resp resp;
	struct ibv_create_qp cmd;
	struct ibv_qp *qp;
	int rv;

	qp = calloc(1, sizeof(*qp));
	if (!qp)
		return NULL;

	rv = ibv_cmd_create_qp(pd, qp, attr, &cmd, sizeof(cmd),
			       &resp, sizeof(resp));
	if (rv) {
		free(qp);
		errno = rv;
		return NULL;
	}
	return qp;
}

static int usb4_rdma_modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr,
			       int attr_mask)
{
	struct ibv_modify_qp cmd = {};

	return ibv_cmd_modify_qp(qp, attr, attr_mask, &cmd, sizeof(cmd));
}

static int usb4_rdma_query_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr,
			      int attr_mask, struct ibv_qp_init_attr *init_attr)
{
	struct ibv_query_qp cmd;

	return ibv_cmd_query_qp(qp, attr, attr_mask, init_attr,
				&cmd, sizeof(cmd));
}

static int usb4_rdma_destroy_qp(struct ibv_qp *qp)
{
	int rv = ibv_cmd_destroy_qp(qp);

	if (rv)
		return rv;
	free(qp);
	return 0;
}

static int usb4_rdma_post_send(struct ibv_qp *qp, struct ibv_send_wr *wr,
			       struct ibv_send_wr **bad_wr)
{
	return ibv_cmd_post_send(qp, wr, bad_wr);
}

static int usb4_rdma_post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr,
			       struct ibv_recv_wr **bad_wr)
{
	return ibv_cmd_post_recv(qp, wr, bad_wr);
}

static int usb4_rdma_poll_cq(struct ibv_cq *base_cq, int num_entries,
			     struct ibv_wc *wc)
{
	struct usb4_rdma_cq *cq =
		container_of(base_cq, struct usb4_rdma_cq, base_cq);
	struct tbv_cq_shm *hdr = cq->hdr;
	uint32_t produced, ovf;
	int polled = 0;

	if (!hdr)
		return ibv_cmd_poll_cq(base_cq, num_entries, wc);

	cq->dbg_polls++;
	/*
	 * Overflow first, and once per EVENT. The kernel advances ovf_seq when
	 * it drops a completion; we remember the values we have already
	 * reported and surface each new one a single time. Testing before
	 * consuming keeps a clean error from arriving alongside a partial
	 * batch, and the latch lives HERE rather than in shared memory so it
	 * cannot turn one dropped completion into a permanently dead CQ.
	 */
	ovf = __atomic_load_n(&hdr->ovf_seq, __ATOMIC_ACQUIRE);
	if (ovf != cq->ovf_seen) {
		cq->ovf_seen = ovf;
		cq->dbg_eio++;
		if (usb4_rdma_shm_debug())
			usb4_rdma_dump_shm("poll -EIO", cq);
		return -EIO;
	}

	/*
	 * Lock-free single-consumer read: the producer publishes the entry and
	 * then the total (release); we read the total (acquire) and then the
	 * entries, and publish our own total (release). Occupancy is the
	 * difference of the two totals -- exact at every capacity, including
	 * the cqe == 1 perftest asks for, where an index comparison cannot
	 * distinguish full from empty. We keep our own head and consumed, so
	 * nothing we read has a second writer.
	 */
	produced = __atomic_load_n(&hdr->produced, __ATOMIC_ACQUIRE);
	while (polled < num_entries && produced - cq->consumed > 0) {
		struct ib_uverbs_wc *u = &cq->ring[cq->head];

		/* ib_uverbs_wc -> ibv_wc, the same field-for-field translation
		 * ibv_cmd_poll_cq performs on the generic path. */
		memset(&wc[polled], 0, sizeof(wc[polled]));
		wc[polled].wr_id      = u->wr_id;
		wc[polled].status     = u->status;
		wc[polled].opcode     = u->opcode;
		wc[polled].vendor_err = u->vendor_err;
		wc[polled].byte_len   = u->byte_len;
		wc[polled].imm_data   = u->ex.imm_data;
		wc[polled].qp_num     = u->qp_num;
		wc[polled].src_qp     = u->src_qp;
		wc[polled].wc_flags   = u->wc_flags;
		wc[polled].pkey_index = u->pkey_index;
		wc[polled].slid       = u->slid;
		wc[polled].sl         = u->sl;
		wc[polled].dlid_path_bits = u->dlid_path_bits;
		polled++;
		cq->head = (cq->head + 1) % hdr->cqe;
		cq->consumed++;
	}
	if (polled)
		__atomic_store_n(&hdr->consumed, cq->consumed, __ATOMIC_RELEASE);

	if (usb4_rdma_shm_debug() && cq->dbg_polls <= 8)
		fprintf(stderr,
			"[shm] poll #%u: n=%d polled=%d produced_read=%u consumed_out=%u\n",
			cq->dbg_polls, num_entries, polled, produced,
			cq->consumed);
	return polled;
}

static int usb4_rdma_req_notify_cq(struct ibv_cq *cq, int solicited_only)
{
	return ibv_cmd_req_notify_cq(cq, solicited_only);
}

/* ----- mr ---------------------------------------------------------- */

static struct ibv_mr *usb4_rdma_reg_mr(struct ibv_pd *pd, void *addr,
				       size_t length, uint64_t hca_va,
				       int access)
{
	struct ib_uverbs_reg_mr_resp resp;
	struct ibv_reg_mr cmd;
	struct verbs_mr *vmr;
	int rv;

	vmr = calloc(1, sizeof(*vmr));
	if (!vmr)
		return NULL;

	rv = ibv_cmd_reg_mr(pd, addr, length, hca_va, access, vmr,
			    &cmd, sizeof(cmd), &resp, sizeof(resp));
	if (rv) {
		free(vmr);
		errno = rv;
		return NULL;
	}
	return &vmr->ibv_mr;
}

static struct ibv_mr *usb4_rdma_reg_dmabuf_mr(struct ibv_pd *pd,
					      uint64_t offset,
					      size_t length,
					      uint64_t iova, int fd,
					      int access)
{
	struct verbs_mr *vmr;
	int rv;

	vmr = calloc(1, sizeof(*vmr));
	if (!vmr)
		return NULL;

#ifdef USB4_RDMA_OLD_REG_DMABUF_MR
	/* rdma-core < v55 (Ubuntu 22.04/24.04 etc.) — no trailing driver_data. */
	rv = ibv_cmd_reg_dmabuf_mr(pd, offset, length, iova, fd, access, vmr);
#else
	rv = ibv_cmd_reg_dmabuf_mr(pd, offset, length, iova, fd, access, vmr,
				   NULL);
#endif
	if (rv) {
		free(vmr);
		errno = rv;
		return NULL;
	}
	return &vmr->ibv_mr;
}

static int usb4_rdma_dereg_mr(struct verbs_mr *vmr)
{
	int rv = ibv_cmd_dereg_mr(vmr);

	if (rv)
		return rv;
	free(vmr);
	return 0;
}

/* ----- context init / free ---------------------------------------- */

static const struct verbs_context_ops usb4_rdma_context_ops = {
	.query_device_ex = usb4_rdma_query_device,
	.query_port      = usb4_rdma_query_port,
	.alloc_pd        = usb4_rdma_alloc_pd,
	.dealloc_pd      = usb4_rdma_dealloc_pd,
	.reg_mr          = usb4_rdma_reg_mr,
	.reg_dmabuf_mr   = usb4_rdma_reg_dmabuf_mr,
	.dereg_mr        = usb4_rdma_dereg_mr,
	.create_cq       = usb4_rdma_create_cq,
	.destroy_cq      = usb4_rdma_destroy_cq,
	.poll_cq         = usb4_rdma_poll_cq,
	.req_notify_cq   = usb4_rdma_req_notify_cq,
	.create_qp       = usb4_rdma_create_qp,
	.destroy_qp      = usb4_rdma_destroy_qp,
	.modify_qp       = usb4_rdma_modify_qp,
	.query_qp        = usb4_rdma_query_qp,
	.post_send       = usb4_rdma_post_send,
	.post_recv       = usb4_rdma_post_recv,
	.free_context    = usb4_rdma_free_context,
};

static struct verbs_context *usb4_rdma_alloc_context(struct ibv_device *base_dev,
						     int cmd_fd, void *priv)
{
	struct usb4_rdma_context *ctx;
	struct ibv_get_context cmd;
	struct ib_uverbs_get_context_resp resp;
	int rv;

	ctx = verbs_init_and_alloc_context(base_dev, cmd_fd, ctx, base,
					   RDMA_DRIVER_UNKNOWN);
	if (!ctx)
		return NULL;

#ifdef USB4_RDMA_OLD_GET_CONTEXT
	/* rdma-core < v55 — no async event channel param. */
	rv = ibv_cmd_get_context(&ctx->base, &cmd, sizeof(cmd),
				 &resp, sizeof(resp));
#else
	rv = ibv_cmd_get_context(&ctx->base, &cmd, sizeof(cmd),
				 NULL, &resp, sizeof(resp));
#endif
	if (rv) {
		verbs_uninit_context(&ctx->base);
		free(ctx);
		errno = rv;
		return NULL;
	}

	verbs_set_ops(&ctx->base, &usb4_rdma_context_ops);
	return &ctx->base;
}

static void usb4_rdma_free_context(struct ibv_context *ibv_ctx)
{
	struct usb4_rdma_context *ctx =
		container_of(ibv_ctx, struct usb4_rdma_context, base.context);

	verbs_uninit_context(&ctx->base);
	free(ctx);
}

/* ----- device alloc / free ---------------------------------------- */

static struct verbs_device *usb4_rdma_device_alloc(struct verbs_sysfs_dev *sysfs)
{
	struct usb4_rdma_device *dev;

	dev = calloc(1, sizeof(*dev));
	if (!dev)
		return NULL;
	return &dev->base_dev;
}

static void usb4_rdma_device_free(struct verbs_device *base_dev)
{
	struct usb4_rdma_device *dev =
		container_of(base_dev, struct usb4_rdma_device, base_dev);
	free(dev);
}

/* ----- match table & driver-ops registration ---------------------- */

static const struct verbs_match_ent usb4_rdma_match_table[] = {
	VERBS_NAME_MATCH("usb4_rdma", NULL),
	VERBS_NAME_MATCH("usb4_apple", NULL),
	{},
};

static bool usb4_rdma_match_device(struct verbs_sysfs_dev *sysfs)
{
	return sysfs->match ||
	       sysfs->node_guid == USB4_RDMA_NODE_GUID ||
	       sysfs->node_guid == USB4_APPLE_NODE_GUID;
}

static const struct verbs_device_ops usb4_rdma_dev_ops = {
	.name                   = "usb4_rdma",
	.match_min_abi_version  = USB4_RDMA_ABI_VERSION,
	.match_max_abi_version  = USB4_RDMA_ABI_VERSION,
	.match_table            = usb4_rdma_match_table,
	.match_device           = usb4_rdma_match_device,
	.alloc_device           = usb4_rdma_device_alloc,
	.uninit_device          = usb4_rdma_device_free,
	.alloc_context          = usb4_rdma_alloc_context,
};

PROVIDER_DRIVER(usb4_rdma, usb4_rdma_dev_ops);
