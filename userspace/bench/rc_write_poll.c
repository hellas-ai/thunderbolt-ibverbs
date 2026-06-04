// SPDX-License-Identifier: MIT
/*
 * rc_write_poll - RC RDMA_WRITE visibility probe.
 *
 * This mirrors RCCL's host FIFO pattern more closely than rc_write_verify:
 * the target spins on host memory while the writer updates it with an
 * RDMA_WRITE. TCP is used only for metadata exchange and final result.
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

#define MAGIC 0x5442504fu
#define VALUE 0x1122334455667788ull
#define IMM_VALUE 0xfeedbeefu

struct __attribute__((aligned(64))) test_fifo {
	uint64_t addr;
	uint64_t size;
	uint32_t rkeys[4];
	uint32_t nreqs;
	uint32_t tag;
	uint64_t idx;
	char padding[16];
};

struct opts {
	const char *role;
	const char *dev;
	const char *connect_host;
	int tcp_port;
	int ib_port;
	int gid_index;
	int timeout_ms;
	int signaled;
	int signal_first;
	int imm;
	int no_recv;
	int expect_no_write;
	int recv_sge;
	int fifo;
	int count;
	int iova2;
	size_t stride;
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

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s --role target|writer --dev DEV [--connect HOST]\n"
		"          [--tcp-port N] [--gid-index N] [--ib-port N]\n"
		"          [--timeout-ms N] [--unsignaled] [--imm] [--no-recv]\n"
		"          [--expect-no-write] [--recv-sge]\n"
		"          [--fifo] [--count N] [--stride BYTES] [--signal-first]\n"
		"          [--iova2]\n",
		argv0);
}

static int parse_opts(int argc, char **argv, struct opts *o)
{
	memset(o, 0, sizeof(*o));
	o->dev = "usb4_rdma0";
	o->tcp_port = 29810;
	o->ib_port = 1;
	o->gid_index = 0;
	o->timeout_ms = 5000;
	o->signaled = 1;
	o->count = 1;
	o->stride = sizeof(struct test_fifo);

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
		else if (!strcmp(argv[i], "--timeout-ms") && i + 1 < argc)
			o->timeout_ms = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--unsignaled"))
			o->signaled = 0;
		else if (!strcmp(argv[i], "--signal-first"))
			o->signal_first = 1;
		else if (!strcmp(argv[i], "--imm"))
			o->imm = 1;
		else if (!strcmp(argv[i], "--no-recv"))
			o->no_recv = 1;
		else if (!strcmp(argv[i], "--expect-no-write"))
			o->expect_no_write = 1;
		else if (!strcmp(argv[i], "--recv-sge"))
			o->recv_sge = 1;
		else if (!strcmp(argv[i], "--fifo"))
			o->fifo = 1;
		else if (!strcmp(argv[i], "--iova2"))
			o->iova2 = 1;
		else if (!strcmp(argv[i], "--count") && i + 1 < argc)
			o->count = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--stride") && i + 1 < argc)
			o->stride = (size_t)strtoull(argv[++i], NULL, 0);
		else
			return -1;
	}

	if (!o->role || (strcmp(o->role, "target") &&
			 strcmp(o->role, "writer")))
		return -1;
	if (!strcmp(o->role, "writer") && !o->connect_host)
		return -1;
	if (o->timeout_ms <= 0)
		return -1;
	if (o->count <= 0 || o->count > 32 || !o->stride)
		return -1;
	if (o->fifo && ((size_t)(o->count - 1) * o->stride +
			sizeof(struct test_fifo) > 4096))
		return -1;
	if (o->no_recv && !o->imm)
		return -1;
	if (o->expect_no_write && (!o->imm || !o->no_recv))
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

static int poll_recv_imm(struct ibv_cq *cq, uint64_t wr_id, uint32_t expected,
			 struct ibv_wc *out)
{
	struct ibv_wc wc;
	int n = ibv_poll_cq(cq, 1, &wc);

	if (n < 0)
		return -1;
	if (!n)
		return 0;
	if (wc.wr_id != wr_id || wc.status != IBV_WC_SUCCESS ||
	    wc.opcode != IBV_WC_RECV_RDMA_WITH_IMM ||
	    ntohl(wc.imm_data) != expected) {
		fprintf(stderr,
			"bad recv wc wr_id=%" PRIu64 " status=%d opcode=%d imm=0x%08x byte_len=%u\n",
			wc.wr_id, wc.status, wc.opcode, ntohl(wc.imm_data),
			wc.byte_len);
		return -1;
	}
	if (out)
		*out = wc;
	return 1;
}

static int poll_send(struct ibv_cq *cq, uint64_t wr_id, int expect_error)
{
	uint64_t deadline = now_ns() + 5ull * 1000 * 1000 * 1000;

	while (now_ns() < deadline) {
		struct ibv_wc wc;
		int n = ibv_poll_cq(cq, 1, &wc);

		if (n < 0)
			return -1;
		if (!n)
			continue;
		if (wc.wr_id != wr_id ||
		    (!expect_error && wc.status != IBV_WC_SUCCESS) ||
		    (expect_error && wc.status == IBV_WC_SUCCESS)) {
			fprintf(stderr,
				"bad wc wr_id=%" PRIu64 " status=%d opcode=%d expect_error=%d\n",
				wc.wr_id, wc.status, wc.opcode, expect_error);
			return -1;
		}
		return 0;
	}
	fprintf(stderr, "send completion timeout\n");
	return -1;
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
	uint64_t *buf = NULL;
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
	if (posix_memalign((void **)&buf, 4096, 4096))
		goto out;
	if (!strcmp(o.role, "writer") && !o.fifo)
		*buf = VALUE;
	else
		memset(buf, 0, 4096);

	if (o.iova2)
		mr = ibv_reg_mr_iova2(pd, buf, 4096, (uintptr_t)buf,
				      IBV_ACCESS_LOCAL_WRITE |
				      IBV_ACCESS_REMOTE_WRITE |
				      IBV_ACCESS_REMOTE_READ);
	else
		mr = ibv_reg_mr(pd, buf, 4096, IBV_ACCESS_LOCAL_WRITE |
				IBV_ACCESS_REMOTE_WRITE |
				IBV_ACCESS_REMOTE_READ);
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

	if (o.imm) {
		char ready;

		if (!strcmp(o.role, "target")) {
			struct ibv_sge rsge = {};
			struct ibv_recv_wr rr = {};
			struct ibv_recv_wr *bad_recv = NULL;

			rsge.addr = (uintptr_t)buf + 128;
			rsge.length = 1;
			rsge.lkey = mr->lkey;
			rr.wr_id = 0x88;
			if (o.recv_sge) {
				rr.sg_list = &rsge;
				rr.num_sge = 1;
			}
			if (!o.no_recv) {
				if (ibv_post_recv(qp, &rr, &bad_recv)) {
					perror("ibv_post_recv");
					goto out;
				}
			}
			ready = 1;
			if (send_all(fd, &ready, sizeof(ready)))
				goto out;
		} else {
			if (recv_all(fd, &ready, sizeof(ready)))
				goto out;
		}
	}

	if (!strcmp(o.role, "writer")) {
		struct ibv_sge sge = {};
		struct ibv_send_wr wr = {};
		struct ibv_send_wr *bad = NULL;
		int first_signaled = o.signal_first && o.signaled;

		for (int i = 0; o.fifo && i < o.count; i++) {
			struct test_fifo *fifo =
				(struct test_fifo *)((char *)buf +
						     i * sizeof(*fifo));

			memset(fifo, 0, sizeof(*fifo));
			fifo->addr = remote.addr + 1024;
			fifo->size = 4096;
			fifo->rkeys[0] = remote.rkey;
			fifo->rkeys[1] = remote.rkey + 1;
			fifo->rkeys[2] = remote.rkey + 2;
			fifo->rkeys[3] = remote.rkey + 3;
			fifo->nreqs = 1;
			fifo->tag = 0x12345678 + (uint32_t)i;
			fifo->idx = (uint64_t)i + 1;
		}

		sge.length = o.fifo ? sizeof(struct test_fifo) : sizeof(*buf);
		sge.lkey = mr->lkey;
		wr.sg_list = &sge;
		wr.num_sge = 1;
		wr.opcode = o.imm ? IBV_WR_RDMA_WRITE_WITH_IMM :
				    IBV_WR_RDMA_WRITE;
		if (o.imm)
			wr.imm_data = htonl(IMM_VALUE);
		wr.wr.rdma.rkey = remote.rkey;

		for (int i = 0; i < o.count; i++) {
			sge.addr = (uintptr_t)buf;
			wr.wr.rdma.remote_addr = remote.addr;
			if (o.fifo) {
				sge.addr += (uint64_t)i * sizeof(struct test_fifo);
				wr.wr.rdma.remote_addr += (uint64_t)i * o.stride;
			}
			wr.wr_id = 0x99 + (uint64_t)i;
			wr.send_flags = o.signaled &&
					(!o.signal_first || i == 0) ?
					IBV_SEND_SIGNALED : 0;
			if (ibv_post_send(qp, &wr, &bad)) {
				perror("ibv_post_send");
				goto out;
			}
		}
		if (first_signaled) {
			if (poll_send(cq, 0x99, o.expect_no_write)) {
				perror("rdma write");
				goto out;
			}
		} else if (o.signaled) {
			for (int i = 0; i < o.count; i++) {
				if (poll_send(cq, 0x99 + (uint64_t)i,
					      o.expect_no_write)) {
					perror("rdma write");
					goto out;
				}
			}
		}
		if (recv_all(fd, &result, sizeof(result)))
			goto out;
		printf("writer result=%d signaled=%d signal_first=%d imm=%d no_recv=%d expect_no_write=%d fifo=%d count=%d stride=%zu\n",
		       result, o.signaled, o.signal_first, o.imm, o.no_recv,
		       o.expect_no_write, o.fifo, o.count, o.stride);
	} else {
		uint64_t deadline = now_ns() + (uint64_t)o.timeout_ms * 1000000ull;
		struct ibv_wc wc = {};
		int saw_value = 0;
		int saw_imm = !o.imm;
		struct test_fifo *fifo = (struct test_fifo *)buf;

		while (now_ns() < deadline) {
			if (!saw_value) {
				if (o.fifo) {
					int matched = 0;

					for (int i = 0; i < o.count; i++) {
						volatile struct test_fifo *vf =
							(volatile struct test_fifo *)
							((char *)buf + (uint64_t)i *
							 o.stride);

						if (vf->idx == (uint64_t)i + 1 &&
						    vf->size == 4096 &&
						    vf->nreqs == 1 &&
						    vf->tag == 0x12345678 + (uint32_t)i &&
						    vf->rkeys[0] == mr->rkey &&
						    vf->addr == local.addr + 1024)
							matched++;
					}
					saw_value = matched == o.count;
				} else if (*(volatile uint64_t *)buf == VALUE) {
					saw_value = 1;
				}
			}
			if (!saw_imm) {
				int n = poll_recv_imm(cq, 0x88, IMM_VALUE, &wc);

				if (n < 0)
					break;
				if (n > 0)
					saw_imm = 1;
			}
			if (!o.expect_no_write && saw_value && saw_imm) {
				result = 0;
				break;
			}
		}
		if (o.expect_no_write && !saw_value && !saw_imm)
			result = 0;
		if (send_all(fd, &result, sizeof(result)))
			goto out;
		printf("target result=%d value=0x%016" PRIx64
		       " saw_value=%d saw_imm=%d imm=0x%08x opcode=%d byte_len=%u"
		       " fifo_idx=%" PRIu64 " fifo_size=%" PRIu64
		       " fifo_nreqs=%u fifo_tag=0x%08x fifo_rkey0=0x%08x"
		       " no_recv=%d expect_no_write=%d count=%d stride=%zu\n",
		       result, *buf, saw_value, saw_imm, ntohl(wc.imm_data),
		       wc.opcode, wc.byte_len, fifo->idx, fifo->size,
		       fifo->nreqs, fifo->tag, fifo->rkeys[0], o.no_recv,
		       o.expect_no_write, o.count, o.stride);
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
