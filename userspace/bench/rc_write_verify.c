// SPDX-License-Identifier: MIT
/*
 * rc_write_verify - minimal RC RDMA_WRITE payload visibility probe.
 *
 * TCP is used only to exchange QP/MR metadata and the final check result.
 * The data path is a single IBV_WR_RDMA_WRITE into the receiver's MR.
 */

#define _POSIX_C_SOURCE 200112L

#include <arpa/inet.h>
#include <errno.h>
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

#define MAGIC 0x54425752u

struct opts {
	const char *role;
	const char *dev;
	const char *connect_host;
	int tcp_port;
	int ib_port;
	int gid_index;
	size_t size;
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

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int send_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;

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
	char *p = buf;

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

static int parse_size(const char *s, size_t *out)
{
	char *end = NULL;
	unsigned long long v;

	errno = 0;
	v = strtoull(s, &end, 0);
	if (errno || !end || *end || !v)
		return -1;
	*out = (size_t)v;
	return 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s --role send|recv --dev DEV [--connect HOST]\n"
		"          [--tcp-port N] [--gid-index N] [--ib-port N]\n"
		"          [--size BYTES]\n",
		argv0);
}

static int parse_opts(int argc, char **argv, struct opts *o)
{
	memset(o, 0, sizeof(*o));
	o->dev = "usb4_rdma0";
	o->tcp_port = 29800;
	o->ib_port = 1;
	o->gid_index = 0;
	o->size = 4096;

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
		else if (!strcmp(argv[i], "--ib-port") && i + 1 < argc)
			o->ib_port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
			if (parse_size(argv[++i], &o->size))
				return -1;
		} else {
			return -1;
		}
	}

	if (!o->role || (strcmp(o->role, "send") && strcmp(o->role, "recv")))
		return -1;
	if (!strcmp(o->role, "send") && !o->connect_host)
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

static int modify_qp(struct ibv_qp *qp, int port, int gid_index,
		     const struct wire_info *local,
		     const struct wire_info *remote)
{
	struct ibv_qp_attr attr = {};
	union ibv_gid dgid;

	attr.qp_state = IBV_QPS_INIT;
	attr.pkey_index = 0;
	attr.port_num = port;
	attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE |
			       IBV_ACCESS_REMOTE_WRITE;
	if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
			  IBV_QP_PORT | IBV_QP_ACCESS_FLAGS))
		return -1;

	memset(&dgid, 0, sizeof(dgid));
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
	attr.ah_attr.grh.sgid_index = gid_index;
	attr.ah_attr.grh.hop_limit = 1;
	attr.ah_attr.dlid = (uint16_t)remote->lid;
	attr.ah_attr.port_num = port;
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
	if (ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_TIMEOUT |
			  IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
			  IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC))
		return -1;
	return 0;
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
		if (wc.wr_id != wr_id || wc.status != IBV_WC_SUCCESS) {
			fprintf(stderr, "bad wc wr_id=%" PRIu64 " status=%d opcode=%d\n",
				wc.wr_id, wc.status, wc.opcode);
			return -1;
		}
		return 0;
	}
	fprintf(stderr, "send completion timeout\n");
	return -1;
}

static void fill_pattern(uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++)
		buf[i] = (uint8_t)((i * 131u + 0x5au) & 0xffu);
}

static int check_pattern(const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		uint8_t want = (uint8_t)((i * 131u + 0x5au) & 0xffu);

		if (buf[i] != want) {
			fprintf(stderr, "mismatch at %zu got=0x%02x want=0x%02x\n",
				i, buf[i], want);
			return -1;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct ibv_context *ctx = NULL;
	struct ibv_port_attr port_attr = {};
	struct ibv_qp_init_attr qp_init = {};
	struct wire_info local = {};
	struct wire_info remote = {};
	struct opts o;
	struct ibv_pd *pd = NULL;
	struct ibv_cq *cq = NULL;
	struct ibv_qp *qp = NULL;
	struct ibv_mr *mr = NULL;
	union ibv_gid gid;
	uint8_t *buf = NULL;
	int listen_fd = -1;
	int fd = -1;
	int result = 1;
	char token;

	if (parse_opts(argc, argv, &o)) {
		usage(argv[0]);
		return 2;
	}

	if (!strcmp(o.role, "recv")) {
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

	ctx = open_context(o.dev);
	if (!ctx) {
		fprintf(stderr, "open device %s failed\n", o.dev);
		goto out;
	}
	if (ibv_query_port(ctx, o.ib_port, &port_attr) ||
	    ibv_query_gid(ctx, o.ib_port, o.gid_index, &gid)) {
		perror("query port/gid");
		goto out;
	}
	pd = ibv_alloc_pd(ctx);
	cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
	if (!pd || !cq)
		goto out;
	if (posix_memalign((void **)&buf, 4096, o.size))
		goto out;
	memset(buf, 0xa5, o.size);
	if (!strcmp(o.role, "send"))
		fill_pattern(buf, o.size);

	mr = ibv_reg_mr(pd, buf, o.size, IBV_ACCESS_LOCAL_WRITE |
			IBV_ACCESS_REMOTE_WRITE);
	if (!mr) {
		perror("ibv_reg_mr");
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
	if (!qp) {
		perror("ibv_create_qp");
		goto out;
	}

	local.magic = MAGIC;
	local.qpn = qp->qp_num;
	local.psn = (uint32_t)(now_ns() & 0xffffffu);
	local.lid = port_attr.lid;
	local.rkey = mr->rkey;
	local.addr = (uintptr_t)buf;
	memcpy(local.gid, &gid, sizeof(local.gid));

	if (send_all(fd, &local, sizeof(local)) ||
	    recv_all(fd, &remote, sizeof(remote)) ||
	    remote.magic != MAGIC) {
		fprintf(stderr, "metadata exchange failed\n");
		goto out;
	}
	if (modify_qp(qp, o.ib_port, o.gid_index, &local, &remote)) {
		perror("ibv_modify_qp");
		goto out;
	}

	if (!strcmp(o.role, "send")) {
		struct ibv_sge sge = {};
		struct ibv_send_wr wr = {};
		struct ibv_send_wr *bad = NULL;

		sge.addr = (uintptr_t)buf;
		sge.length = (uint32_t)o.size;
		sge.lkey = mr->lkey;
		wr.wr_id = 0x77;
		wr.sg_list = &sge;
		wr.num_sge = 1;
		wr.opcode = IBV_WR_RDMA_WRITE;
		wr.send_flags = IBV_SEND_SIGNALED;
		wr.wr.rdma.remote_addr = remote.addr;
		wr.wr.rdma.rkey = remote.rkey;
		if (ibv_post_send(qp, &wr, &bad) || poll_send(cq, wr.wr_id)) {
			perror("rdma write");
			goto out;
		}
		token = 'D';
		if (send_all(fd, &token, 1) || recv_all(fd, &result, sizeof(result)))
			goto out;
		printf("send_complete result=%d size=%zu\n", result, o.size);
	} else {
		if (recv_all(fd, &token, 1) || token != 'D')
			goto out;
		result = check_pattern(buf, o.size) ? 1 : 0;
		if (send_all(fd, &result, sizeof(result)))
			goto out;
		printf("recv_check result=%d size=%zu\n", result, o.size);
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
	free(buf);
	if (fd >= 0)
		close(fd);
	if (listen_fd >= 0)
		close(listen_fd);
	return result;
}
