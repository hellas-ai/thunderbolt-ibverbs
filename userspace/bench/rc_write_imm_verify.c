// SPDX-License-Identifier: MIT
/*
 * rc_write_imm_verify - RC RDMA_WRITE_WITH_IMM stream with payload checks.
 *
 * The sender writes message n into slot n % slots of the receiver's buffer,
 * with a length that varies with n (zero, around the 4048-byte native
 * fragment, around 64 KiB, up to --max-size) and immediate data n. The
 * receiver checks, for every message, that immediates arrive in order, that
 * the completion's byte_len matches (with --check-byte-len) and that every
 * byte holds the pattern of n; then it poisons the slot, reposts a receive
 * and reports progress over TCP. The sender reuses a slot only after the
 * receiver has checked it. TCP carries only metadata and progress.
 */

#define _POSIX_C_SOURCE 200112L

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <infiniband/verbs.h>

#define MAGIC 0x54425749u
#define POISON 0xa5

struct opts {
	const char *role;
	const char *dev;
	const char *connect_host;
	int tcp_port;
	int ib_port;
	int gid_index;
	uint32_t slots;
	uint32_t depth;
	uint32_t recv_wqes;
	size_t max_size;
	double seconds;
	bool check_byte_len;
};

struct wire_info {
	uint32_t magic;
	uint32_t qpn;
	uint32_t psn;
	uint32_t lid;
	uint32_t rkey;
	uint32_t slots;
	uint64_t addr;
	uint64_t slot_size;
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
		"usage: %s --role send|recv --dev DEV [--connect HOST]\n"
		"          [--tcp-port N] [--gid-index N] [--ib-port N]\n"
		"          [--slots N] [--depth N] [--max-size BYTES]\n"
		"          [--seconds S] [--check-byte-len] [--recv-wqes N]\n"
		"--recv-wqes below --depth makes the sender hit RNR (receiver)\n",
		argv0);
}

static int parse_opts(int argc, char **argv, struct opts *o)
{
	memset(o, 0, sizeof(*o));
	o->dev = "usb4_rdma0";
	o->tcp_port = 29810;
	o->ib_port = 1;
	o->slots = 8;
	o->depth = 4;
	o->max_size = 4u << 20;
	o->seconds = 10.0;

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
		else if (!strcmp(argv[i], "--slots") && i + 1 < argc)
			o->slots = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--depth") && i + 1 < argc)
			o->depth = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--max-size") && i + 1 < argc)
			o->max_size = strtoull(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
			o->seconds = strtod(argv[++i], NULL);
		else if (!strcmp(argv[i], "--check-byte-len"))
			o->check_byte_len = true;
		else if (!strcmp(argv[i], "--recv-wqes") && i + 1 < argc)
			o->recv_wqes = (uint32_t)strtoul(argv[++i], NULL, 0);
		else
			return -1;
	}
	if (!o->recv_wqes || o->recv_wqes > o->slots)
		o->recv_wqes = o->slots;

	if (!o->role || (strcmp(o->role, "send") && strcmp(o->role, "recv")))
		return -1;
	if (!strcmp(o->role, "send") && !o->connect_host)
		return -1;
	if (!o->slots || !o->depth || o->depth > o->slots ||
	    o->max_size < 4096 || o->max_size > (64u << 20))
		return -1;
	return 0;
}

/* Message length for sequence number n: edge cases first, then a spread. */
static size_t msg_size(uint32_t n, size_t max_size)
{
	static const size_t fixed[] = {
		0, 1, 100, 4047, 4048, 4049, 8096, 8097, 65535, 65536, 65537,
		(1u << 20) - 13, 1u << 20,
	};
	uint64_t x;

	if (n < sizeof(fixed) / sizeof(fixed[0]))
		return fixed[n] < max_size ? fixed[n] : max_size;
	x = (uint64_t)n * 0x9e3779b97f4a7c15ull;
	x ^= x >> 29;
	switch (x & 3) {
	case 0:
		return x % 4096;		/* one fragment */
	case 1:
		return x % 65536;		/* below the stripe threshold */
	default:
		return x % (max_size + 1);	/* anything up to max */
	}
}

static uint64_t pattern_word(uint32_t n, size_t word)
{
	uint64_t x = ((uint64_t)n << 32) ^ (word * 0xbf58476d1ce4e5b9ull);

	x ^= x >> 31;
	return x * 0x94d049bb133111ebull;
}

static void fill_pattern(uint8_t *buf, uint32_t n, size_t len)
{
	size_t words = len / 8;

	for (size_t i = 0; i < words; i++) {
		uint64_t w = pattern_word(n, i);

		memcpy(buf + i * 8, &w, 8);
	}
	if (len % 8) {
		uint64_t w = pattern_word(n, words);

		memcpy(buf + words * 8, &w, len % 8);
	}
}

/* Returns the first differing offset, or len when the pattern matches. */
static size_t check_pattern(const uint8_t *buf, uint32_t n, size_t len)
{
	size_t words = len / 8;

	for (size_t i = 0; i < words; i++) {
		uint64_t w = pattern_word(n, i);

		if (memcmp(buf + i * 8, &w, 8))
			return i * 8;
	}
	if (len % 8) {
		uint64_t w = pattern_word(n, words);

		if (memcmp(buf + words * 8, &w, len % 8))
			return words * 8;
	}
	return len;
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

static int post_recv(struct ibv_qp *qp, uint64_t wr_id)
{
	struct ibv_recv_wr wr = {};
	struct ibv_recv_wr *bad = NULL;

	wr.wr_id = wr_id;
	return ibv_post_recv(qp, &wr, &bad);
}

/* Receiver: check each message as it completes, then report progress. */
static int run_recv(const struct opts *o, int fd, struct ibv_qp *qp,
		    struct ibv_cq *cq, uint8_t *buf)
{
	uint32_t next = 0;
	uint32_t total = UINT32_MAX;
	uint64_t bytes = 0;
	uint32_t errors = 0;

	for (uint32_t i = 0; i < o->recv_wqes; i++) {
		if (post_recv(qp, i)) {
			perror("ibv_post_recv");
			return -1;
		}
	}

	while (next != total) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		struct ibv_wc wc;
		int n;

		/* The sender's total arrives once it stops posting. */
		if (total == UINT32_MAX && poll(&pfd, 1, 0) > 0 &&
		    recv_all(fd, &total, sizeof(total)))
			return -1;

		n = ibv_poll_cq(cq, 1, &wc);
		if (n < 0) {
			fprintf(stderr, "ibv_poll_cq failed: %d\n", n);
			return -1;
		}
		if (!n)
			continue;
		if (wc.status != IBV_WC_SUCCESS ||
		    wc.opcode != IBV_WC_RECV_RDMA_WITH_IMM ||
		    !(wc.wc_flags & IBV_WC_WITH_IMM)) {
			fprintf(stderr, "bad recv wc status=%d opcode=%d flags=0x%x after %u messages\n",
				wc.status, wc.opcode, wc.wc_flags, next);
			return -1;
		}

		uint32_t seq = ntohl(wc.imm_data);
		size_t len = msg_size(seq, o->max_size);
		uint8_t *slot = buf + (size_t)(seq % o->slots) * o->max_size;
		size_t bad;

		if (seq != next) {
			fprintf(stderr, "out of order: imm=%u expected=%u\n",
				seq, next);
			errors++;
		}
		if (o->check_byte_len && wc.byte_len != len) {
			fprintf(stderr, "byte_len %u, expected %zu for %u\n",
				wc.byte_len, len, seq);
			errors++;
		}
		bad = check_pattern(slot, seq, len);
		if (bad != len) {
			fprintf(stderr, "payload mismatch: msg=%u len=%zu first_bad=%zu byte=0x%02x\n",
				seq, len, bad, slot[bad]);
			errors++;
		}
		memset(slot, POISON, len);
		bytes += len;
		next = seq + 1;
		if (post_recv(qp, seq) || send_all(fd, &next, sizeof(next)))
			return -1;
	}

	printf("recv_check messages=%u bytes=%" PRIu64 " errors=%u\n",
	       next, bytes, errors);
	return send_all(fd, &errors, sizeof(errors)) ? -1 : (errors ? 1 : 0);
}

/* Sender: write messages while the receiver keeps up, for --seconds. */
static int run_send(const struct opts *o, int fd, struct ibv_qp *qp,
		    struct ibv_cq *cq, struct ibv_mr *mr, uint8_t *buf,
		    const struct wire_info *remote)
{
	uint64_t start = now_ns();
	uint64_t deadline = start + (uint64_t)(o->seconds * 1e9);
	uint32_t posted = 0;
	uint32_t completed = 0;
	uint32_t checked = 0;
	uint32_t errors = 0;
	uint64_t bytes = 0;
	double secs;

	for (;;) {
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		bool stop = now_ns() >= deadline;
		struct ibv_wc wc;
		int n;

		while (poll(&pfd, 1, 0) > 0) {
			if (recv_all(fd, &checked, sizeof(checked))) {
				fprintf(stderr, "receiver closed the connection after %u checked, %u posted, %u completed\n",
					checked, posted, completed);
				return -1;
			}
		}
		if (stop && completed == posted)
			break;

		/* Reuse a slot only after the receiver checked it. */
		if (!stop && posted - completed < o->depth &&
		    posted - checked < o->slots) {
			struct ibv_sge sge = {};
			struct ibv_send_wr wr = {};
			struct ibv_send_wr *bad = NULL;
			size_t len = msg_size(posted, o->max_size);
			size_t off = (size_t)(posted % o->slots) * o->max_size;

			fill_pattern(buf + off, posted, len);
			sge.addr = (uintptr_t)(buf + off);
			sge.length = (uint32_t)len;
			sge.lkey = mr->lkey;
			wr.wr_id = posted;
			wr.sg_list = len ? &sge : NULL;
			wr.num_sge = len ? 1 : 0;
			wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
			wr.send_flags = IBV_SEND_SIGNALED;
			wr.imm_data = htonl(posted);
			wr.wr.rdma.remote_addr = remote->addr + off;
			wr.wr.rdma.rkey = remote->rkey;
			if (ibv_post_send(qp, &wr, &bad)) {
				perror("ibv_post_send");
				return -1;
			}
			bytes += len;
			posted++;
			continue;
		}

		n = ibv_poll_cq(cq, 1, &wc);
		if (n < 0) {
			fprintf(stderr, "ibv_poll_cq failed: %d\n", n);
			return -1;
		}
		if (!n)
			continue;
		if (wc.status != IBV_WC_SUCCESS || wc.wr_id != completed) {
			fprintf(stderr, "bad send wc wr_id=%" PRIu64 " expected=%u status=%d\n",
				wc.wr_id, completed, wc.status);
			return -1;
		}
		completed++;
	}

	secs = (double)(now_ns() - start) / 1e9;
	if (send_all(fd, &posted, sizeof(posted)))
		return -1;
	while (checked != posted) {
		if (recv_all(fd, &checked, sizeof(checked)))
			return -1;
	}
	if (recv_all(fd, &errors, sizeof(errors)))
		return -1;
	printf("send_complete messages=%u bytes=%" PRIu64 " seconds=%.2f gbit_s=%.2f receiver_errors=%u\n",
	       posted, bytes, secs, (double)bytes * 8 / secs / 1e9, errors);
	return errors ? 1 : 0;
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
	size_t buf_len;
	int listen_fd = -1;
	int fd = -1;
	int result = 1;

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
	cq = ibv_create_cq(ctx, (int)(2 * o.slots + 16), NULL, NULL, 0);
	if (!pd || !cq)
		goto out;
	buf_len = (size_t)o.slots * o.max_size;
	if (posix_memalign((void **)&buf, 4096, buf_len))
		goto out;
	memset(buf, POISON, buf_len);

	mr = ibv_reg_mr(pd, buf, buf_len, IBV_ACCESS_LOCAL_WRITE |
			IBV_ACCESS_REMOTE_WRITE);
	if (!mr) {
		perror("ibv_reg_mr");
		goto out;
	}

	qp_init.send_cq = cq;
	qp_init.recv_cq = cq;
	qp_init.qp_type = IBV_QPT_RC;
	qp_init.cap.max_send_wr = o.depth;
	qp_init.cap.max_recv_wr = o.slots;
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
	local.slots = o.slots;
	local.addr = (uintptr_t)buf;
	local.slot_size = o.max_size;
	memcpy(local.gid, &gid, sizeof(local.gid));

	if (send_all(fd, &local, sizeof(local)) ||
	    recv_all(fd, &remote, sizeof(remote)) ||
	    remote.magic != MAGIC || remote.slots != o.slots ||
	    remote.slot_size != o.max_size) {
		fprintf(stderr, "metadata exchange failed (slots and max-size must match)\n");
		goto out;
	}
	if (modify_qp(qp, o.ib_port, o.gid_index, &local, &remote)) {
		perror("ibv_modify_qp");
		goto out;
	}

	if (!strcmp(o.role, "send"))
		result = run_send(&o, fd, qp, cq, mr, buf, &remote);
	else
		result = run_recv(&o, fd, qp, cq, buf);
	if (result < 0) {
		fprintf(stderr, "%s failed\n", o.role);
		result = 1;
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
