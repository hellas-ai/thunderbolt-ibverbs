#ifndef PERFTEST_APPLE_COMPAT_INFINIBAND_VERBS_H
#define PERFTEST_APPLE_COMPAT_INFINIBAND_VERBS_H

#include_next <infiniband/verbs.h>

#ifndef __always_inline
#define __always_inline __attribute__((__always_inline__)) inline
#endif

#ifndef IBV_ODP_SUPPORT_SEND
#define IBV_ODP_SUPPORT_SEND (1 << 2)
#endif
#ifndef IBV_ODP_SUPPORT_RECV
#define IBV_ODP_SUPPORT_RECV (1 << 3)
#endif
#ifndef IBV_ODP_SUPPORT_WRITE
#define IBV_ODP_SUPPORT_WRITE (1 << 4)
#endif
#ifndef IBV_ODP_SUPPORT_READ
#define IBV_ODP_SUPPORT_READ (1 << 5)
#endif
#ifndef IBV_ODP_SUPPORT_ATOMIC
#define IBV_ODP_SUPPORT_ATOMIC (1 << 6)
#endif
#ifndef IBV_ODP_SUPPORT_SRQ_RECV
#define IBV_ODP_SUPPORT_SRQ_RECV (1 << 7)
#endif

struct ibv_srq_attr {
	uint32_t max_wr;
	uint32_t max_sge;
	uint32_t srq_limit;
};

struct ibv_srq_init_attr {
	void *srq_context;
	struct ibv_srq_attr attr;
};

enum ibv_srq_type {
	IBV_SRQT_BASIC,
	IBV_SRQT_XRC,
};

struct ibv_srq_init_attr_ex {
	void *srq_context;
	struct ibv_srq_attr attr;
	uint32_t comp_mask;
	enum ibv_srq_type srq_type;
	struct ibv_pd *pd;
	struct ibv_cq *cq;
	struct ibv_xrcd *xrcd;
};

#ifndef IBV_SRQ_INIT_ATTR_TYPE
#define IBV_SRQ_INIT_ATTR_TYPE (1 << 0)
#endif
#ifndef IBV_SRQ_INIT_ATTR_PD
#define IBV_SRQ_INIT_ATTR_PD (1 << 1)
#endif
#ifndef IBV_SRQ_INIT_ATTR_CQ
#define IBV_SRQ_INIT_ATTR_CQ (1 << 2)
#endif
#ifndef IBV_SRQ_INIT_ATTR_XRCD
#define IBV_SRQ_INIT_ATTR_XRCD (1 << 3)
#endif

struct ibv_flow;
struct ibv_flow_attr;

#ifndef HAVE_EX_ODP
#define check_odp_support(ctx, user_param) 0
#endif

int ibv_get_srq_num(struct ibv_srq *srq, uint32_t *srq_num);
int ibv_attach_mcast(struct ibv_qp *qp, const union ibv_gid *gid, uint16_t lid);
int ibv_detach_mcast(struct ibv_qp *qp, const union ibv_gid *gid, uint16_t lid);
struct ibv_srq *ibv_create_srq(struct ibv_pd *pd, struct ibv_srq_init_attr *srq_init_attr);
struct ibv_srq *ibv_create_srq_ex(struct ibv_context *context,
				  struct ibv_srq_init_attr_ex *srq_init_attr_ex);
int ibv_destroy_srq(struct ibv_srq *srq);
int ibv_post_srq_recv(struct ibv_srq *srq, struct ibv_recv_wr *recv_wr,
		      struct ibv_recv_wr **bad_recv_wr);
struct ibv_mr *ibv_alloc_null_mr(struct ibv_pd *pd);
struct ibv_flow *ibv_create_flow(struct ibv_qp *qp,
				 struct ibv_flow_attr *flow_attr);
int ibv_destroy_flow(struct ibv_flow *flow);

#endif
