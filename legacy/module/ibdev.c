// SPDX-License-Identifier: GPL-2.0
/*
 * ibdev.c — soft-RDMA `ib_device` skeleton.
 *
 * Goal of this commit: register an ib_device named `usb4_rdma0` so
 * `ibv_devinfo` finds it and `ibv_open_device()` succeeds. All
 * data-plane verbs return -ENOSYS for now; the actual RC SEND/RECV
 * path comes in subsequent commits.
 *
 * Design choices baked in here:
 *   - One ib_device per machine (singleton). Multiple peers / cables
 *     are exposed as multiple PORTS on this device. Strix Halo with
 *     two host routers => two ports.
 *   - Link-layer Ethernet (RoCE-style addressing); we'll route via
 *     xdomain hops underneath but the verbs surface looks
 *     RoCEv2-compatible to apps.
 *   - Port state comes from the underlying tb_service binding: ACTIVE
 *     when a peer is connected, DOWN otherwise. usb4_rdma_ibdev_set_port()
 *     is the bridge from main.c's probe/remove callbacks.
 *
 * The module declares RDMA_DRIVER_UNKNOWN until we get a proper
 * enum value upstream; this is fine for out-of-tree development but
 * means `rdma link` won't show a typed driver.
 */

#define pr_fmt(fmt) "usb4_rdma/ibdev: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/random.h>
#include <linux/etherdevice.h>
#include <net/addrconf.h>

#include <rdma/ib_verbs.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/uverbs_ioctl.h>

#include "usb4_rdma.h"

/* For now we expose a single port. When we have proper per-controller
 * binding (one ib_device per host router, or one port per controller
 * on a single ib_device) this becomes USB4_RDMA_MAX_PORTS = 2. */
#define USB4_RDMA_NPORTS       1
#define USB4_RDMA_UVERBS_ABI   1
#define USB4_RDMA_PAGE_SIZE_CAP \
	(SZ_4K | SZ_2M | SZ_1G)

struct usb4_rdma_ucontext {
	struct ib_ucontext base;
};

struct usb4_rdma_pd {
	struct ib_pd base;
};

struct usb4_rdma_cq {
	struct ib_cq base;
	int cqe;
};

struct usb4_rdma_qp {
	struct ib_qp base;
	enum ib_qp_state state;
	struct ib_qp_attr attr;
	int attr_mask;
};

struct usb4_rdma_ib_dev {
	struct ib_device base;
	atomic_t active_peers;	/* # of bound xdomain services */
};

static struct usb4_rdma_ib_dev *u4r_dev;

/* ----- ucontext (ibv_open_device) --------------------------------- */

static int u4r_alloc_ucontext(struct ib_ucontext *ibuc, struct ib_udata *udata)
{
	/* No driver-specific context yet. */
	return 0;
}

static void u4r_dealloc_ucontext(struct ib_ucontext *ibuc)
{
}

/* ----- query callbacks -------------------------------------------- */

static int u4r_query_device(struct ib_device *ibdev,
			    struct ib_device_attr *attr,
			    struct ib_udata *udata)
{
	memset(attr, 0, sizeof(*attr));

	attr->vendor_id           = 0x1022; /* AMD; will distinguish later */
	attr->vendor_part_id      = 0x158d;
	attr->hw_ver              = 1;
	attr->fw_ver              = 0;
	attr->sys_image_guid      = ibdev->node_guid;

	/* Capabilities — minimal but valid. We claim RC and basic RDMA
	 * write so apps probing for RoCE features see something sensible.
	 * Numbers are placeholders until we wire the data path. */
	attr->device_cap_flags    = IB_DEVICE_CHANGE_PHY_PORT;
	attr->max_mr_size         = ~0ull;
	attr->page_size_cap       = USB4_RDMA_PAGE_SIZE_CAP;
	attr->max_qp              = 256;
	attr->max_qp_wr           = 1024;
	attr->max_send_sge        = 8;
	attr->max_recv_sge        = 8;
	attr->max_sge_rd          = 8;
	attr->max_cq              = 256;
	attr->max_cqe             = 4096;
	attr->max_mr              = 1024;
	attr->max_pd              = 256;
	attr->max_qp_rd_atom      = 0; /* RDMA READ not implemented yet */
	attr->max_res_rd_atom     = 0;
	attr->max_qp_init_rd_atom = 0;
	attr->atomic_cap          = IB_ATOMIC_NONE;
	attr->max_ee              = 0;
	attr->max_rdd             = 0;
	attr->max_mw              = 0;
	attr->max_raw_ipv6_qp     = 0;
	attr->max_raw_ethy_qp     = 0;
	attr->max_mcast_grp       = 0;
	attr->max_mcast_qp_attach = 0;
	attr->max_total_mcast_qp_attach = 0;
	attr->max_ah              = 16;
	attr->max_srq             = 0;
	attr->max_srq_wr          = 0;
	attr->max_srq_sge         = 0;
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
	attr->lid            = 0;
	attr->sm_lid         = 0;
	attr->lmc            = 0;
	attr->max_vl_num     = 1;
	attr->sm_sl          = 0;
	attr->subnet_timeout = 0;
	attr->init_type_reply= 0;
	attr->active_width   = IB_WIDTH_4X;
	attr->active_speed   = IB_SPEED_FDR10; /* ~10 Gbps placeholder */

	return 0;
}

static int u4r_get_port_immutable(struct ib_device *ibdev, u32 port_num,
				  struct ib_port_immutable *immutable)
{
	struct ib_port_attr attr;
	int err;

	err = u4r_query_port(ibdev, port_num, &attr);
	if (err)
		return err;

	immutable->pkey_tbl_len = attr.pkey_tbl_len;
	immutable->gid_tbl_len  = attr.gid_tbl_len;
	/* core_cap_flags = 0 — no protocol claimed. Avoids MAD machinery
	 * (which RoCE requires) and the netdev requirement (which IWARP
	 * imposes via iw_query_port). The IB core ends up using our
	 * query_port directly via __ib_query_port. We'll claim a real
	 * protocol once we wire CM + GID address resolution. */
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

	/* Stub GID — derive from node_guid for uniqueness; will be
	 * replaced when we wire address resolution to xdomain peer UUIDs. */
	memset(gid, 0, sizeof(*gid));
	memcpy(gid->raw + 8, &ibdev->node_guid, 8);
	return 0;
}

static enum rdma_link_layer u4r_get_link_layer(struct ib_device *ibdev,
					       u32 port_num)
{
	return IB_LINK_LAYER_ETHERNET;
}

/* ----- stub data-plane verbs (return -ENOSYS for now) ------------- */

#define U4R_STUB(name) \
	pr_warn_ratelimited("%s called — not implemented yet\n", #name)

static int u4r_alloc_pd(struct ib_pd *ibpd, struct ib_udata *udata)
{
	/* No driver state yet — the ib_pd object IB core allocated for
	 * us is enough. Real implementation will track per-PD state
	 * (registered MRs, associated QPs) here. */
	pr_info("alloc_pd ok\n");
	return 0;
}

static int u4r_dealloc_pd(struct ib_pd *ibpd, struct ib_udata *udata)
{
	pr_info("dealloc_pd\n");
	return 0;
}

static int u4r_create_qp(struct ib_qp *ibqp, struct ib_qp_init_attr *attr,
			 struct ib_udata *udata)
{
	struct usb4_rdma_qp *qp = container_of(ibqp, struct usb4_rdma_qp, base);

	if (attr->qp_type != IB_QPT_RC) {
		pr_warn("create_qp: only RC supported, got type %d\n",
			attr->qp_type);
		return -EOPNOTSUPP;
	}
	qp->state = IB_QPS_RESET;
	pr_info("create_qp ok (RC, max_send_wr=%u max_recv_wr=%u)\n",
		attr->cap.max_send_wr, attr->cap.max_recv_wr);
	return 0;
}

static int u4r_destroy_qp(struct ib_qp *ibqp, struct ib_udata *udata)
{
	pr_info("destroy_qp\n");
	return 0;
}

static int u4r_modify_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr,
			 int attr_mask, struct ib_udata *udata)
{
	struct usb4_rdma_qp *qp = container_of(ibqp, struct usb4_rdma_qp, base);

	/* IB core validates the (current_state, attr_mask, new_state)
	 * tuple against the RC state-machine table before calling us,
	 * so we only need to reflect the new state and remember the
	 * attrs for when the data path comes online. */
	if (attr_mask & IB_QP_STATE) {
		pr_info("modify_qp: state %d -> %d (mask 0x%x)\n",
			qp->state, attr->qp_state, attr_mask);
		qp->state = attr->qp_state;
	}
	qp->attr = *attr;
	qp->attr_mask = attr_mask;
	return 0;
}

static int u4r_query_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr,
			int attr_mask, struct ib_qp_init_attr *init_attr)
{
	struct usb4_rdma_qp *qp = container_of(ibqp, struct usb4_rdma_qp, base);

	*attr = qp->attr;
	attr->qp_state = qp->state;
	memset(init_attr, 0, sizeof(*init_attr));
	init_attr->qp_type = IB_QPT_RC;
	return 0;
}

static int u4r_create_cq(struct ib_cq *ibcq, const struct ib_cq_init_attr *attr,
			 struct uverbs_attr_bundle *attrs)
{
	struct usb4_rdma_cq *cq = container_of(ibcq, struct usb4_rdma_cq, base);

	cq->cqe = attr->cqe;
	pr_info("create_cq ok (cqe=%u, comp_vector=%u)\n",
		attr->cqe, attr->comp_vector);
	return 0;
}

static int u4r_destroy_cq(struct ib_cq *ibcq, struct ib_udata *udata)
{
	pr_info("destroy_cq\n");
	return 0;
}

static int u4r_post_send(struct ib_qp *ibqp, const struct ib_send_wr *wr,
			 const struct ib_send_wr **bad_wr)
{
	U4R_STUB(post_send);
	return -ENOSYS;
}

static int u4r_post_recv(struct ib_qp *ibqp, const struct ib_recv_wr *wr,
			 const struct ib_recv_wr **bad_wr)
{
	U4R_STUB(post_recv);
	return -ENOSYS;
}

static int u4r_poll_cq(struct ib_cq *ibcq, int num_entries, struct ib_wc *wc)
{
	return 0;
}

static int u4r_req_notify_cq(struct ib_cq *ibcq, enum ib_cq_notify_flags flags)
{
	return 0;
}

static struct ib_mr *u4r_reg_user_mr(struct ib_pd *ibpd, u64 start, u64 length,
				     u64 virt_addr, int access_flags,
				     struct ib_dmah *dmah,
				     struct ib_udata *udata)
{
	U4R_STUB(reg_user_mr);
	return ERR_PTR(-ENOSYS);
}

static int u4r_dereg_mr(struct ib_mr *ibmr, struct ib_udata *udata)
{
	return 0;
}

static struct ib_mr *u4r_get_dma_mr(struct ib_pd *ibpd, int access_flags)
{
	return ERR_PTR(-ENOSYS);
}

/* ----- ops table -------------------------------------------------- */

static const struct ib_device_ops u4r_dev_ops = {
	.owner             = THIS_MODULE,
	.driver_id         = RDMA_DRIVER_UNKNOWN,
	.uverbs_abi_ver    = USB4_RDMA_UVERBS_ABI,

	/* device & port queries — implemented */
	.query_device      = u4r_query_device,
	.query_port        = u4r_query_port,
	.query_pkey        = u4r_query_pkey,
	.query_gid         = u4r_query_gid,
	.get_port_immutable= u4r_get_port_immutable,
	.get_link_layer    = u4r_get_link_layer,

	/* ucontext — empty but present so ibv_open_device works */
	.alloc_ucontext    = u4r_alloc_ucontext,
	.dealloc_ucontext  = u4r_dealloc_ucontext,

	/* data-plane stubs (-ENOSYS until the data path is wired) */
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

/* ----- peer-tracking hook called from loadtest probe/remove ------- */

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

/* ----- module-level register / unregister ------------------------- */

int usb4_rdma_ibdev_init(void)
{
	struct usb4_rdma_ib_dev *u4r;
	u8 mac[ETH_ALEN];
	int err;

	u4r = ib_alloc_device(usb4_rdma_ib_dev, base);
	if (!u4r)
		return -ENOMEM;

	atomic_set(&u4r->active_peers, 0);

	u4r->base.phys_port_cnt   = USB4_RDMA_NPORTS;
	u4r->base.num_comp_vectors = num_possible_cpus();
	u4r->base.local_dma_lkey  = 0;
	u4r->base.node_type       = RDMA_NODE_RNIC; /* Ethernet-style RDMA */

	/* Generate a unique node_guid from a random MAC. */
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
	pr_info("registered ib_device %s (%u port, %d active peers)\n",
		dev_name(&u4r->base.dev), USB4_RDMA_NPORTS,
		atomic_read(&u4r->active_peers));
	return 0;
}

void usb4_rdma_ibdev_exit(void)
{
	if (!u4r_dev)
		return;
	pr_info("unregistering ib_device %s\n", dev_name(&u4r_dev->base.dev));
	ib_unregister_device(&u4r_dev->base);
	ib_dealloc_device(&u4r_dev->base);
	u4r_dev = NULL;
}
