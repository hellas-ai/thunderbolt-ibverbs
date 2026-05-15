// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/idr.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <rdma/ib_mad.h>
#include <rdma/ib_umem.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/ib_verbs.h>

#include "tbv.h"

#define TBV_IBDEV_ABI_VERSION 1
#define TBV_IBDEV_PORTS 1
#define TBV_IBDEV_MAX_QP 256
#define TBV_IBDEV_MAX_QP_WR 1024
#define TBV_IBDEV_MAX_CQ 256
#define TBV_IBDEV_MAX_CQE 4096
#define TBV_IBDEV_MAX_SGE 4
#define TBV_IBDEV_QPN_MIN 0x900
#define TBV_IBDEV_QPN_MAX 0x00ffffff
#define TBV_IBDEV_PAGE_SIZE_CAP (SZ_4K | SZ_2M | SZ_1G)

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
	u32 cqe;
};

struct tbv_qp {
	struct ib_qp base;
	struct tbv_state *owner;
	struct ib_qp_init_attr init_attr;
	struct ib_qp_attr attr;
	enum ib_qp_state state;
	enum ib_qp_type type;
	bool qpn_allocated;
};

struct tbv_mr {
	struct ib_mr base;
	struct tbv_state *owner;
	struct ib_umem *umem;
	u64 start;
	u64 length;
	u64 virt_addr;
	int access;
};

struct tbv_ibdev {
	struct ib_device base;
	struct tbv_state *state;
};

static DEFINE_IDA(tbv_qpn_ida);
static atomic_t tbv_mr_key = ATOMIC_INIT(1);

static struct tbv_state *tbv_ibdev_state(struct ib_device *ibdev)
{
	struct tbv_ibdev *dev = container_of(ibdev, struct tbv_ibdev, base);

	return dev->state;
}

static bool tbv_ibdev_port_active(struct tbv_state *state)
{
	struct tbv_peer *peer;
	bool active = false;

	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		list_for_each_entry(rail, &peer->rails, node) {
			if (rail->path.state == TBV_PATH_TUNNEL_ENABLED) {
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
	attr->max_qp = TBV_IBDEV_MAX_QP;
	attr->max_qp_wr = TBV_IBDEV_MAX_QP_WR;
	attr->max_send_sge = TBV_IBDEV_MAX_SGE;
	attr->max_recv_sge = TBV_IBDEV_MAX_SGE;
	attr->max_sge_rd = TBV_IBDEV_MAX_SGE;
	attr->max_cq = TBV_IBDEV_MAX_CQ;
	attr->max_cqe = TBV_IBDEV_MAX_CQE;
	attr->max_mr = 1024;
	attr->max_pd = 256;
	attr->max_qp_rd_atom = 16;
	attr->max_res_rd_atom = 256;
	attr->max_qp_init_rd_atom = 16;
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
	attr->max_msg_sz = SZ_1G;
	attr->gid_tbl_len = 1;
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
	immutable->core_cap_flags = RDMA_CORE_PORT_IBA_IB;
	immutable->max_mad_size = IB_MGMT_MAD_SIZE;
	return 0;
}

static enum rdma_link_layer tbv_get_link_layer(struct ib_device *ibdev,
					       u32 port_num)
{
	return IB_LINK_LAYER_INFINIBAND;
}

static int tbv_query_gid(struct ib_device *ibdev, u32 port_num, int index,
			 union ib_gid *gid)
{
	if (port_num != 1 || index != 0)
		return -EINVAL;

	memset(gid, 0, sizeof(*gid));
	gid->global.subnet_prefix = cpu_to_be64(0xfe80000000000000ULL);
	gid->global.interface_id = ibdev->node_guid;
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
	return 0;
}

static int tbv_create_qp(struct ib_qp *qp, struct ib_qp_init_attr *init_attr,
			 struct ib_udata *udata)
{
	struct tbv_qp *tqp = container_of(qp, struct tbv_qp, base);
	int qpn;

	if (!init_attr || init_attr->srq)
		return -EOPNOTSUPP;
	if (init_attr->qp_type != IB_QPT_RC && init_attr->qp_type != IB_QPT_UC)
		return -EOPNOTSUPP;
	if (init_attr->cap.max_send_wr > TBV_IBDEV_MAX_QP_WR ||
	    init_attr->cap.max_recv_wr > TBV_IBDEV_MAX_QP_WR ||
	    init_attr->cap.max_send_sge > TBV_IBDEV_MAX_SGE ||
	    init_attr->cap.max_recv_sge > TBV_IBDEV_MAX_SGE)
		return -EINVAL;

	qpn = ida_alloc_range(&tbv_qpn_ida, TBV_IBDEV_QPN_MIN,
			      TBV_IBDEV_QPN_MAX, GFP_KERNEL);
	if (qpn < 0)
		return qpn;

	tqp->init_attr = *init_attr;
	tqp->owner = tbv_ibdev_state(qp->device);
	tqp->state = IB_QPS_RESET;
	tqp->type = init_attr->qp_type;
	tqp->qpn_allocated = true;
	qp->qp_num = qpn;
	init_attr->cap.max_inline_data = 0;
	atomic_inc(&tqp->owner->verbs_qps);
	return 0;
}

static int tbv_destroy_qp(struct ib_qp *qp, struct ib_udata *udata)
{
	struct tbv_qp *tqp = container_of(qp, struct tbv_qp, base);

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

	if (attr_mask & IB_QP_STATE)
		tqp->state = attr->qp_state;
	tqp->attr = *attr;
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

static int tbv_post_send(struct ib_qp *qp, const struct ib_send_wr *wr,
			 const struct ib_send_wr **bad_wr)
{
	if (bad_wr)
		*bad_wr = wr;
	return -EOPNOTSUPP;
}

static int tbv_post_recv(struct ib_qp *qp, const struct ib_recv_wr *wr,
			 const struct ib_recv_wr **bad_wr)
{
	if (bad_wr)
		*bad_wr = wr;
	return -EOPNOTSUPP;
}

static int tbv_poll_cq(struct ib_cq *cq, int num_entries, struct ib_wc *wc)
{
	return 0;
}

static int tbv_req_notify_cq(struct ib_cq *cq, enum ib_cq_notify_flags flags)
{
	return 0;
}

static struct ib_mr *tbv_reg_user_mr(struct ib_pd *pd, u64 start, u64 length,
				     u64 virt_addr, int access,
				     struct ib_dmah *dmah,
				     struct ib_udata *udata)
{
	struct tbv_mr *mr;
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
	mr->start = start;
	mr->length = length;
	mr->virt_addr = virt_addr;
	mr->access = access;
	ret = xa_insert(&mr->owner->verbs_mrs_xa, key, mr, GFP_KERNEL);
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

	if (mr->owner)
		xa_erase(&mr->owner->verbs_mrs_xa, ibmr->lkey);
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

	ret = ib_register_device(&dev->base, "usb4_rdma%d", NULL);
	if (ret) {
		ib_dealloc_device(&dev->base);
		return ret;
	}

	state->ibdev = dev;
	state->verbs_registered = true;
	pr_info("registered ib_device %s\n", dev_name(&dev->base.dev));
	return 0;
}

void tbv_ibdev_stop(struct tbv_state *state)
{
	struct tbv_ibdev *dev = state->ibdev;

	if (!dev)
		return;

	state->verbs_registered = false;
	state->ibdev = NULL;
	ib_unregister_device(&dev->base);
	ib_dealloc_device(&dev->base);
	ida_destroy(&tbv_qpn_ida);
	pr_info("unregistered ib_device\n");
}
