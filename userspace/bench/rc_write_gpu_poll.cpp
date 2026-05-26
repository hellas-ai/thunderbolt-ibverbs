// SPDX-License-Identifier: MIT
/*
 * RC RDMA_WRITE visibility probe for GPU-mapped host memory.
 *
 * The target allocates a hipHostMallocMapped buffer, registers it as an MR,
 * launches a tiny GPU kernel that polls the mapped host word, then asks the
 * writer to update the word with IBV_WR_RDMA_WRITE.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <hip/hip_runtime.h>
#include <infiniband/verbs.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAGIC 0x54424750u
#define VALUE 0x8877665544332211ull

struct opts {
	const char *role;
	const char *dev = "usb4_rdma0";
	const char *connect_host;
	int tcp_port = 29820;
	int ib_port = 1;
	int gid_index = 1;
	unsigned int hip_host_flags = hipHostMallocMapped;
	uint64_t loops = 1000000000ull;
};

struct wire_info {
	uint32_t magic;
	uint32_t qpn;
	uint32_t psn;
	uint32_t lid;
	uint32_t rkey;
	uint64_t addr;
	uint8_t gid[16];
};

__global__ static void wait_word(volatile unsigned long long *word,
				 unsigned long long expected,
				 unsigned int *seen,
				 unsigned long long loops)
{
	for (unsigned long long i = 0; i < loops; i++) {
		if (*word == expected) {
			*seen = 1;
			return;
		}
	}
	*seen = 0;
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int send_all(int fd, const void *buf, size_t len)
{
	const char *p = (const char *)buf;

	while (len) {
		ssize_t n = send(fd, p, len, 0);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
	char *p = (char *)buf;

	while (len) {
		ssize_t n = recv(fd, p, len, 0);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!n)
			return -1;
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static int tcp_listen(int port)
{
	struct sockaddr_in addr = {};
	int fd;
	int one = 1;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((uint16_t)port);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) ||
	    listen(fd, 1)) {
		close(fd);
		return -1;
	}
	return fd;
}

static int tcp_connect(const char *host, int port)
{
	struct addrinfo hints = {};
	struct addrinfo *res = NULL;
	char port_s[16];
	int fd = -1;

	snprintf(port_s, sizeof(port_s), "%d", port);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port_s, &hints, &res))
		return -1;
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (!connect(fd, ai->ai_addr, ai->ai_addrlen))
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	return fd;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s --role target|writer [--connect HOST] [--dev DEV]\n"
		"          [--tcp-port N] [--gid-index N] [--loops N]\n"
		"          [--coherent|--noncoherent|--uncached]\n",
		argv0);
}

static int parse_opts(int argc, char **argv, struct opts *o)
{
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--role") && i + 1 < argc)
			o->role = argv[++i];
		else if (!strcmp(argv[i], "--dev") && i + 1 < argc)
			o->dev = argv[++i];
		else if (!strcmp(argv[i], "--connect") && i + 1 < argc)
			o->connect_host = argv[++i];
		else if (!strcmp(argv[i], "--tcp-port") && i + 1 < argc)
			o->tcp_port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--gid-index") && i + 1 < argc)
			o->gid_index = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--loops") && i + 1 < argc)
			o->loops = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--coherent"))
			o->hip_host_flags = hipHostMallocMapped |
					    hipHostMallocCoherent;
		else if (!strcmp(argv[i], "--noncoherent"))
			o->hip_host_flags = hipHostMallocMapped |
					    hipHostMallocNonCoherent;
		else if (!strcmp(argv[i], "--uncached"))
			o->hip_host_flags = hipHostMallocMapped |
					    hipHostMallocUncached;
		else
			return -1;
	}
	if (!o->role || (strcmp(o->role, "target") &&
			 strcmp(o->role, "writer")))
		return -1;
	if (!strcmp(o->role, "writer") && !o->connect_host)
		return -1;
	return 0;
}

static struct ibv_context *open_context(const char *name)
{
	struct ibv_device **list;
	struct ibv_context *ctx = NULL;
	int n = 0;

	list = ibv_get_device_list(&n);
	if (!list)
		return NULL;
	for (int i = 0; i < n; i++) {
		if (strcmp(ibv_get_device_name(list[i]), name))
			continue;
		ctx = ibv_open_device(list[i]);
		break;
	}
	ibv_free_device_list(list);
	return ctx;
}

static int modify_qp(struct ibv_qp *qp, const struct opts *o,
		     const struct wire_info *local,
		     const struct wire_info *remote)
{
	struct ibv_qp_attr attr = {};
	union ibv_gid dgid;

	attr.qp_state = IBV_QPS_INIT;
	attr.pkey_index = 0;
	attr.port_num = o->ib_port;
	attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
			       IBV_ACCESS_REMOTE_WRITE |
			       IBV_ACCESS_REMOTE_READ;
	if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
			  IBV_QP_PORT | IBV_QP_ACCESS_FLAGS))
		return -1;

	memcpy(&dgid, remote->gid, sizeof(remote->gid));
	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTR;
	attr.path_mtu = IBV_MTU_4096;
	attr.dest_qp_num = remote->qpn;
	attr.rq_psn = remote->psn;
	attr.max_dest_rd_atomic = 1;
	attr.min_rnr_timer = 12;
	attr.ah_attr.is_global = 1;
	attr.ah_attr.grh.dgid = dgid;
	attr.ah_attr.grh.sgid_index = o->gid_index;
	attr.ah_attr.grh.hop_limit = 1;
	attr.ah_attr.dlid = (uint16_t)remote->lid;
	attr.ah_attr.port_num = o->ib_port;
	if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_AV |
			  IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
			  IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
			  IBV_QP_MIN_RNR_TIMER))
		return -1;

	memset(&attr, 0, sizeof(attr));
	attr.qp_state = IBV_QPS_RTS;
	attr.timeout = 14;
	attr.retry_cnt = 7;
	attr.rnr_retry = 7;
	attr.sq_psn = local->psn;
	attr.max_rd_atomic = 1;
	return ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_TIMEOUT |
			     IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
			     IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
}

static int poll_send(struct ibv_cq *cq, uint64_t wr_id)
{
	uint64_t deadline = now_ns() + 5ull * 1000 * 1000 * 1000;

	while (now_ns() < deadline) {
		struct ibv_wc wc;
		int n = ibv_poll_cq(cq, 1, &wc);

		if (n < 0)
			return -1;
		if (!n)
			continue;
		if (wc.wr_id != wr_id || wc.status != IBV_WC_SUCCESS)
			return -1;
		return 0;
	}
	return -1;
}

int main(int argc, char **argv)
{
	struct opts o;
	struct ibv_context *ctx = NULL;
	struct ibv_port_attr port_attr = {};
	struct ibv_qp_init_attr qp_init = {};
	struct wire_info local = {};
	struct wire_info remote = {};
	struct ibv_pd *pd = NULL;
	struct ibv_cq *cq = NULL;
	struct ibv_qp *qp = NULL;
	struct ibv_mr *mr = NULL;
	union ibv_gid gid;
	void *host = NULL;
	void *dev = NULL;
	unsigned int *seen_dev = NULL;
	unsigned int seen = 0;
	int listen_fd = -1;
	int fd = -1;
	int result = 1;

	if (parse_opts(argc, argv, &o)) {
		usage(argv[0]);
		return 2;
	}

	if (!strcmp(o.role, "target")) {
		listen_fd = tcp_listen(o.tcp_port);
		if (listen_fd < 0) {
			perror("listen");
			goto out;
		}
		fd = accept(listen_fd, NULL, NULL);
	} else {
		fd = tcp_connect(o.connect_host, o.tcp_port);
	}
	if (fd < 0) {
		perror("tcp");
		goto out;
	}

	if (hipSetDevice(0) != hipSuccess)
		goto out;
	if (!strcmp(o.role, "target")) {
		if (hipHostMalloc(&host, 4096, o.hip_host_flags) != hipSuccess)
			goto out;
		if (hipHostGetDevicePointer(&dev, host, 0) != hipSuccess)
			goto out;
		memset(host, 0, 4096);
		if (hipMalloc(&seen_dev, sizeof(*seen_dev)) != hipSuccess)
			goto out;
		if (hipMemset(seen_dev, 0, sizeof(*seen_dev)) != hipSuccess)
			goto out;
	} else {
		if (posix_memalign(&host, 4096, 4096))
			goto out;
		memset(host, 0, 4096);
		*(uint64_t *)host = VALUE;
	}

	ctx = open_context(o.dev);
	if (!ctx)
		goto out;
	if (ibv_query_port(ctx, o.ib_port, &port_attr) ||
	    ibv_query_gid(ctx, o.ib_port, o.gid_index, &gid))
		goto out;
	pd = ibv_alloc_pd(ctx);
	cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
	if (!pd || !cq)
		goto out;
	mr = ibv_reg_mr_iova2(pd, host, 4096, (uintptr_t)host,
			      IBV_ACCESS_LOCAL_WRITE |
			      IBV_ACCESS_REMOTE_WRITE |
			      IBV_ACCESS_REMOTE_READ);
	if (!mr) {
		perror("ibv_reg_mr_iova2");
		goto out;
	}

	qp_init.send_cq = cq;
	qp_init.recv_cq = cq;
	qp_init.qp_type = IBV_QPT_RC;
	qp_init.cap.max_send_wr = 16;
	qp_init.cap.max_recv_wr = 16;
	qp_init.cap.max_send_sge = 1;
	qp_init.cap.max_recv_sge = 1;
	qp = ibv_create_qp(pd, &qp_init);
	if (!qp)
		goto out;

	local.magic = MAGIC;
	local.qpn = qp->qp_num;
	local.psn = (uint32_t)(now_ns() & 0xffffffu);
	local.lid = port_attr.lid;
	local.rkey = mr->rkey;
	local.addr = (uintptr_t)host;
	memcpy(local.gid, &gid, sizeof(local.gid));
	if (send_all(fd, &local, sizeof(local)) ||
	    recv_all(fd, &remote, sizeof(remote)) ||
	    remote.magic != MAGIC)
		goto out;
	if (modify_qp(qp, &o, &local, &remote))
		goto out;

	if (!strcmp(o.role, "target")) {
		char ready = 1;

		hipLaunchKernelGGL(wait_word, dim3(1), dim3(1), 0, 0,
				   (volatile unsigned long long *)dev,
				   (unsigned long long)VALUE, seen_dev,
				   (unsigned long long)o.loops);
		if (send_all(fd, &ready, sizeof(ready)))
			goto out;
		if (hipDeviceSynchronize() != hipSuccess)
			goto out;
		if (hipMemcpy(&seen, seen_dev, sizeof(seen),
			      hipMemcpyDeviceToHost) != hipSuccess)
			goto out;
		result = seen && *(volatile uint64_t *)host == VALUE ? 0 : 1;
		if (send_all(fd, &result, sizeof(result)))
			goto out;
		printf("target result=%d cpu_value=0x%016" PRIx64
		       " gpu_seen=%u host=%p dev=%p rkey=0x%x flags=0x%x\n",
		       result, *(uint64_t *)host, seen, host, dev, mr->rkey,
		       o.hip_host_flags);
	} else {
		struct ibv_sge sge = {};
		struct ibv_send_wr wr = {};
		struct ibv_send_wr *bad = NULL;
		char ready;

		if (recv_all(fd, &ready, sizeof(ready)))
			goto out;
		sge.addr = (uintptr_t)host;
		sge.length = sizeof(uint64_t);
		sge.lkey = mr->lkey;
		wr.wr_id = 0x77;
		wr.sg_list = &sge;
		wr.num_sge = 1;
		wr.opcode = IBV_WR_RDMA_WRITE;
		wr.send_flags = IBV_SEND_SIGNALED;
		wr.wr.rdma.remote_addr = remote.addr;
		wr.wr.rdma.rkey = remote.rkey;
		if (ibv_post_send(qp, &wr, &bad) || poll_send(cq, wr.wr_id))
			goto out;
		if (recv_all(fd, &result, sizeof(result)))
			goto out;
		printf("writer result=%d wrote=0x%016" PRIx64 "\n",
		       result, *(uint64_t *)host);
	}

out:
	if (qp)
		ibv_destroy_qp(qp);
	if (mr)
		ibv_dereg_mr(mr);
	if (cq)
		ibv_destroy_cq(cq);
	if (pd)
		ibv_dealloc_pd(pd);
	if (ctx)
		ibv_close_device(ctx);
	if (seen_dev)
		hipFree(seen_dev);
	if (host && !strcmp(o.role, "target"))
		hipHostFree(host);
	else
		free(host);
	if (fd >= 0)
		close(fd);
	if (listen_fd >= 0)
		close(listen_fd);
	return result;
}
