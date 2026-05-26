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
#include <unistd.h>

#include "usb4_rdma.h"

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

static struct ibv_cq *usb4_rdma_create_cq(struct ibv_context *ctx, int num_cqe,
					  struct ibv_comp_channel *channel,
					  int comp_vector)
{
	struct ib_uverbs_create_cq_resp resp;
	struct ibv_create_cq cmd;
	struct ibv_cq *cq;
	int rv;

	cq = calloc(1, sizeof(*cq));
	if (!cq)
		return NULL;

	rv = ibv_cmd_create_cq(ctx, num_cqe, channel, comp_vector, cq,
			       &cmd, sizeof(cmd), &resp, sizeof(resp));
	if (rv) {
		free(cq);
		errno = rv;
		return NULL;
	}
	return cq;
}

static int usb4_rdma_destroy_cq(struct ibv_cq *cq)
{
	int rv = ibv_cmd_destroy_cq(cq);

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

static int usb4_rdma_poll_cq(struct ibv_cq *cq, int num_entries,
			     struct ibv_wc *wc)
{
	return ibv_cmd_poll_cq(cq, num_entries, wc);
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

	rv = ibv_cmd_reg_dmabuf_mr(pd, offset, length, iova, fd, access, vmr,
				   NULL);
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

	rv = ibv_cmd_get_context(&ctx->base, &cmd, sizeof(cmd),
				 NULL, &resp, sizeof(resp));
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
