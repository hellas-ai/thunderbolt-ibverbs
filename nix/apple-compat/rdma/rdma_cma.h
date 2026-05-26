#ifndef PERFTEST_APPLE_COMPAT_RDMA_CMA_H
#define PERFTEST_APPLE_COMPAT_RDMA_CMA_H

#include <netinet/in.h>
#include <stdint.h>
#include <sys/socket.h>

#include <infiniband/verbs.h>

typedef uint16_t __be16;

struct ibv_sa_path_rec;

enum rdma_cm_event_type {
	RDMA_CM_EVENT_ADDR_RESOLVED,
	RDMA_CM_EVENT_ADDR_ERROR,
	RDMA_CM_EVENT_ROUTE_RESOLVED,
	RDMA_CM_EVENT_ROUTE_ERROR,
	RDMA_CM_EVENT_CONNECT_REQUEST,
	RDMA_CM_EVENT_CONNECT_RESPONSE,
	RDMA_CM_EVENT_CONNECT_ERROR,
	RDMA_CM_EVENT_UNREACHABLE,
	RDMA_CM_EVENT_REJECTED,
	RDMA_CM_EVENT_ESTABLISHED,
	RDMA_CM_EVENT_DISCONNECTED,
	RDMA_CM_EVENT_DEVICE_REMOVAL,
	RDMA_CM_EVENT_MULTICAST_JOIN,
	RDMA_CM_EVENT_MULTICAST_ERROR,
	RDMA_CM_EVENT_ADDR_CHANGE,
	RDMA_CM_EVENT_TIMEWAIT_EXIT,
	RDMA_CM_EVENT_ADDRINFO_RESOLVED,
	RDMA_CM_EVENT_ADDRINFO_ERROR,
	RDMA_CM_EVENT_USER,
	RDMA_CM_EVENT_INTERNAL,
};

enum rdma_port_space {
	RDMA_PS_IPOIB = 0x0002,
	RDMA_PS_TCP = 0x0106,
	RDMA_PS_UDP = 0x0111,
	RDMA_PS_IB = 0x013F,
};

struct rdma_ib_addr {
	union ibv_gid sgid;
	union ibv_gid dgid;
	__be16 pkey;
};

struct rdma_addr {
	union {
		struct sockaddr src_addr;
		struct sockaddr_in src_sin;
		struct sockaddr_in6 src_sin6;
		struct sockaddr_storage src_storage;
	};
	union {
		struct sockaddr dst_addr;
		struct sockaddr_in dst_sin;
		struct sockaddr_in6 dst_sin6;
		struct sockaddr_storage dst_storage;
	};
	union {
		struct rdma_ib_addr ibaddr;
	} addr;
};

struct rdma_route {
	struct rdma_addr addr;
	struct ibv_sa_path_rec *path_rec;
	int num_paths;
};

struct rdma_event_channel {
	int fd;
};

struct rdma_cm_id {
	struct ibv_context *verbs;
	struct rdma_event_channel *channel;
	void *context;
	struct ibv_qp *qp;
	struct rdma_route route;
	enum rdma_port_space ps;
	uint8_t port_num;
	struct rdma_cm_event *event;
	struct ibv_comp_channel *send_cq_channel;
	struct ibv_cq *send_cq;
	struct ibv_comp_channel *recv_cq_channel;
	struct ibv_cq *recv_cq;
	struct ibv_srq *srq;
	struct ibv_pd *pd;
	enum ibv_qp_type qp_type;
};

struct rdma_conn_param {
	const void *private_data;
	uint8_t private_data_len;
	uint8_t responder_resources;
	uint8_t initiator_depth;
	uint8_t flow_control;
	uint8_t retry_count;
	uint8_t rnr_retry_count;
	uint8_t srq;
	uint32_t qp_num;
};

struct rdma_ud_param {
	const void *private_data;
	uint8_t private_data_len;
	struct ibv_ah_attr ah_attr;
	uint32_t qp_num;
	uint32_t qkey;
};

struct rdma_cm_event {
	struct rdma_cm_id *id;
	struct rdma_cm_id *listen_id;
	enum rdma_cm_event_type event;
	int status;
	union {
		struct rdma_conn_param conn;
		struct rdma_ud_param ud;
		uint64_t arg;
	} param;
};

#define RAI_PASSIVE 0x00000001
#define RAI_NUMERICHOST 0x00000002
#define RAI_NOROUTE 0x00000004
#define RAI_FAMILY 0x00000008

struct rdma_addrinfo {
	int ai_flags;
	int ai_family;
	int ai_qp_type;
	int ai_port_space;
	socklen_t ai_src_len;
	socklen_t ai_dst_len;
	struct sockaddr *ai_src_addr;
	struct sockaddr *ai_dst_addr;
	char *ai_src_canonname;
	char *ai_dst_canonname;
	size_t ai_route_len;
	void *ai_route;
	size_t ai_connect_len;
	void *ai_connect;
	struct rdma_addrinfo *ai_next;
};

enum {
	RDMA_OPTION_ID = 0,
	RDMA_OPTION_IB = 1,
};

enum {
	RDMA_OPTION_ID_TOS = 0,
	RDMA_OPTION_ID_REUSEADDR = 1,
	RDMA_OPTION_ID_AFONLY = 2,
	RDMA_OPTION_ID_ACK_TIMEOUT = 3,
};

struct rdma_event_channel *rdma_create_event_channel(void);
void rdma_destroy_event_channel(struct rdma_event_channel *channel);
int rdma_create_id(struct rdma_event_channel *channel,
		   struct rdma_cm_id **id, void *context,
		   enum rdma_port_space ps);
int rdma_destroy_id(struct rdma_cm_id *id);
int rdma_bind_addr(struct rdma_cm_id *id, struct sockaddr *addr);
int rdma_resolve_addr(struct rdma_cm_id *id, struct sockaddr *src_addr,
		      struct sockaddr *dst_addr, int timeout_ms);
int rdma_resolve_route(struct rdma_cm_id *id, int timeout_ms);
int rdma_create_qp(struct rdma_cm_id *id, struct ibv_pd *pd,
		   struct ibv_qp_init_attr *qp_init_attr);
void rdma_destroy_qp(struct rdma_cm_id *id);
int rdma_connect(struct rdma_cm_id *id, struct rdma_conn_param *conn_param);
int rdma_listen(struct rdma_cm_id *id, int backlog);
int rdma_accept(struct rdma_cm_id *id, struct rdma_conn_param *conn_param);
int rdma_reject(struct rdma_cm_id *id, const void *private_data,
		uint8_t private_data_len);
int rdma_disconnect(struct rdma_cm_id *id);
int rdma_get_cm_event(struct rdma_event_channel *channel,
		      struct rdma_cm_event **event);
int rdma_ack_cm_event(struct rdma_cm_event *event);
const char *rdma_event_str(enum rdma_cm_event_type event);
int rdma_set_option(struct rdma_cm_id *id, int level, int optname,
		    void *optval, size_t optlen);
int rdma_getaddrinfo(const char *node, const char *service,
		     const struct rdma_addrinfo *hints,
		     struct rdma_addrinfo **res);
void rdma_freeaddrinfo(struct rdma_addrinfo *res);

static inline struct sockaddr *rdma_get_local_addr(struct rdma_cm_id *id)
{
	return &id->route.addr.src_addr;
}

static inline struct sockaddr *rdma_get_peer_addr(struct rdma_cm_id *id)
{
	return &id->route.addr.dst_addr;
}

#endif
