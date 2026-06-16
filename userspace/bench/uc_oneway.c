// SPDX-License-Identifier: MIT
/*
 * uc_oneway - minimal UC SEND benchmark/probe.
 *
 * This is for Apple RDMA-over-Thunderbolt interop debugging. TCP is used only
 * to exchange QP metadata; data flows as UC IBV_WR_SEND messages.
 *
 * Example:
 *   # Mac receiver, connecting outbound to avoid macOS inbound firewall prompts
 *   ./uc_oneway --role recv --dev rdma_en1 --gid-index 1 \
 *       --connect 192.168.1.11 --port 18515 \
 *       --size 4096 --count 2048 --depth 512
 *
 *   # Linux sender, listening for the TCP metadata connection
 *   ./uc_oneway --role send --dev usb4_rdma0 --gid-index 1 \
 *       --port 18515 --size 4096 --count 2048 --depth 64
 *
 * For credit-window probes, the receiver can post more RECV WRs than the
 * number of messages expected:
 *   ./uc_oneway --role recv ... --count 64 --depth 512 --recv-posts 512
 *
 * Same-QP bidirectional probe, matching the traffic shape that JACCL allreduce
 * uses on the Apple-compatible path:
 *   ./uc_oneway --role bidi --dev rdma_en1 --gid-index 1 --port 18515 \
 *       --size 16384 --count 64 --depth 2 --recv-posts 2 --check
 *   ./uc_oneway --role bidi --dev usb4_apple0 --gid-index 1 \
 *       --connect 192.168.1.20 --port 18515 \
 *       --size 16384 --count 64 --depth 2 --recv-posts 2 --check
 */

#include <arpa/inet.h>
#ifdef __APPLE__
#include <dlfcn.h>
#endif
#include <errno.h>
#include <infiniband/verbs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct opts {
	const char *role;
	const char *dev;
	const char *connect_host;
	int port;
	int gid_index;
	int ib_port;
	int depth;
	int send_slots;
	int recv_posts;
	int recv_post_delay_ms;
	int count;
	int mtu;
	int check;
	int check_any_order;
	int recv_wr_id_base;
	int send_wr_id_base;
	int hold_after_rts_ms;
	int hold_before_destroy_ms;
	size_t size;
};

struct peer_info {
	uint32_t magic;
	uint32_t qpn;
	uint32_t psn;
	uint32_t lid;
	uint8_t gid[16];
};

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void debug_step(const char *step)
{
	if (getenv("UC_ONEWAY_DEBUG")) {
		fprintf(stderr, "uc_oneway: %s\n", step);
		fflush(stderr);
	}
}

static void print_gid(FILE *f, const union ibv_gid *gid)
{
	for (int i = 0; i < 16; i += 2)
		fprintf(f, "%02x%02x%s", gid->raw[i], gid->raw[i + 1],
			i == 14 ? "" : ":");
}

static int gid_is_zero(const union ibv_gid *gid)
{
	static const union ibv_gid zero;

	return !memcmp(gid, &zero, sizeof(*gid));
}

static int gid_is_ipv4_mapped(const union ibv_gid *gid)
{
	static const uint8_t prefix[12] = {
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff,
	};

	return !memcmp(gid->raw, prefix, sizeof(prefix));
}

static int select_gid(struct ibv_context *ctx, int port, int requested_index,
		      int gid_tbl_len, union ibv_gid *gid, int *selected_index)
{
	if (requested_index >= 0) {
		if (requested_index >= gid_tbl_len) {
			fprintf(stderr, "gid index %d exceeds gid table length %d\n",
				requested_index, gid_tbl_len);
			return -1;
		}
		memset(gid, 0, sizeof(*gid));
		if (ibv_query_gid(ctx, port, requested_index, gid)) {
			perror("ibv_query_gid");
			return -1;
		}
		if (gid_is_zero(gid)) {
			fprintf(stderr, "gid index %d is empty\n",
				requested_index);
			return -1;
		}
		*selected_index = requested_index;
		return 0;
	}

	for (int i = 0; i < gid_tbl_len; i++) {
		memset(gid, 0, sizeof(*gid));
		if (ibv_query_gid(ctx, port, i, gid) || !gid_is_ipv4_mapped(gid))
			continue;
		*selected_index = i;
		return 0;
	}

	for (int i = 0; i < gid_tbl_len; i++) {
		memset(gid, 0, sizeof(*gid));
		if (ibv_query_gid(ctx, port, i, gid) || gid_is_zero(gid))
			continue;
		*selected_index = i;
		return 0;
	}

	fprintf(stderr, "no non-zero GID found on port %d\n", port);
	return -1;
}

static size_t align_up(size_t v, size_t a)
{
	return (v + a - 1) & ~(a - 1);
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s --role recv|send|bidi --dev DEV [--gid-index N|auto] --port P\n"
		"          [--connect HOST] [--size BYTES] [--count N]\n"
		"          [--depth N] [--send-slots N] [--recv-posts N]\n"
		"          [--mtu 256|512|1024|2048|4096]\n"
		"          [--recv-post-delay-ms N]\n"
		"          [--recv-wr-id-base N] [--send-wr-id-base N]\n"
		"          [--hold-after-rts-ms N] [--hold-before-destroy-ms N]\n"
		"          [--check] [--check-any-order]\n",
		argv0);
}

static int parse_int(const char *s, int *out)
{
	char *end = NULL;
	long v;

	errno = 0;
	v = strtol(s, &end, 0);
	if (errno || !end || *end || v < 0 || v > 0x7fffffff)
		return -1;
	*out = (int)v;
	return 0;
}

static int parse_gid_index(const char *s, int *out)
{
	if (!strcmp(s, "auto")) {
		*out = -1;
		return 0;
	}
	return parse_int(s, out);
}

static int parse_size(const char *s, size_t *out)
{
	char *end = NULL;
	unsigned long long v;

	errno = 0;
	v = strtoull(s, &end, 0);
	if (errno || !end || *end || v == 0)
		return -1;
	*out = (size_t)v;
	return 0;
}

static int load_verbs_provider(void)
{
#ifdef __APPLE__
	void *handle;

	handle = dlopen("librdma.dylib", RTLD_NOW | RTLD_GLOBAL);
	if (!handle) {
		fprintf(stderr, "failed to load librdma.dylib: %s\n", dlerror());
		return -1;
	}
#endif
	return 0;
}

static enum ibv_mtu mtu_enum(int mtu)
{
	switch (mtu) {
	case 256:
		return IBV_MTU_256;
	case 512:
		return IBV_MTU_512;
	case 1024:
		return IBV_MTU_1024;
	case 2048:
		return IBV_MTU_2048;
	case 4096:
	default:
		return IBV_MTU_4096;
	}
}

static int parse_opts(int argc, char **argv, struct opts *o)
{
	memset(o, 0, sizeof(*o));
	o->port = 18515;
	o->gid_index = -1;
	o->ib_port = 1;
	o->depth = 64;
	o->count = 1000;
	o->mtu = 1024;
	o->recv_wr_id_base = -1;
	o->send_wr_id_base = -1;
	o->size = 4096;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--role") && i + 1 < argc)
			o->role = argv[++i];
		else if (!strcmp(argv[i], "--dev") && i + 1 < argc)
			o->dev = argv[++i];
		else if (!strcmp(argv[i], "--connect") && i + 1 < argc)
			o->connect_host = argv[++i];
		else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
			if (parse_int(argv[++i], &o->port))
				return -1;
		} else if (!strcmp(argv[i], "--gid-index") && i + 1 < argc) {
			if (parse_gid_index(argv[++i], &o->gid_index))
				return -1;
		} else if (!strcmp(argv[i], "--ib-port") && i + 1 < argc) {
			if (parse_int(argv[++i], &o->ib_port))
				return -1;
		} else if (!strcmp(argv[i], "--depth") && i + 1 < argc) {
			if (parse_int(argv[++i], &o->depth))
				return -1;
		} else if (!strcmp(argv[i], "--send-slots") && i + 1 < argc) {
			if (parse_int(argv[++i], &o->send_slots))
				return -1;
		} else if (!strcmp(argv[i], "--recv-posts") && i + 1 < argc) {
			if (parse_int(argv[++i], &o->recv_posts))
				return -1;
		} else if (!strcmp(argv[i], "--recv-post-delay-ms") && i + 1 < argc) {
			if (parse_int(argv[++i], &o->recv_post_delay_ms))
				return -1;
		} else if (!strcmp(argv[i], "--recv-wr-id-base") && i + 1 < argc) {
			if (parse_int(argv[++i], &o->recv_wr_id_base))
				return -1;
		} else if (!strcmp(argv[i], "--send-wr-id-base") && i + 1 < argc) {
			if (parse_int(argv[++i], &o->send_wr_id_base))
				return -1;
		} else if (!strcmp(argv[i], "--hold-after-rts-ms") && i + 1 < argc) {
			if (parse_int(argv[++i], &o->hold_after_rts_ms))
				return -1;
		} else if (!strcmp(argv[i], "--hold-before-destroy-ms") && i + 1 < argc) {
			if (parse_int(argv[++i], &o->hold_before_destroy_ms))
				return -1;
		} else if (!strcmp(argv[i], "--count") && i + 1 < argc) {
			if (parse_int(argv[++i], &o->count))
				return -1;
		} else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
			if (parse_size(argv[++i], &o->size))
				return -1;
		} else if (!strcmp(argv[i], "--mtu") && i + 1 < argc) {
			if (parse_int(argv[++i], &o->mtu))
				return -1;
		} else if (!strcmp(argv[i], "--check")) {
			o->check = 1;
		} else if (!strcmp(argv[i], "--check-any-order")) {
			o->check = 1;
			o->check_any_order = 1;
		} else {
			return -1;
		}
	}

	if (!o->role || !o->dev || o->port <= 0 || o->port > 65535 ||
	    o->gid_index < -1 || o->ib_port <= 0 || o->depth <= 0 ||
	    o->depth > 4095 || o->send_slots < 0 || o->recv_posts < 0 ||
	    o->recv_posts > 4095 || o->count <= 0 ||
	    o->hold_after_rts_ms < 0 || o->hold_before_destroy_ms < 0)
		return -1;
	if (o->mtu != 256 && o->mtu != 512 && o->mtu != 1024 &&
	    o->mtu != 2048 && o->mtu != 4096)
		return -1;
	if (o->check_any_order && o->size < sizeof(uint64_t))
		return -1;
	if (strcmp(o->role, "send") && strcmp(o->role, "recv") &&
	    strcmp(o->role, "bidi"))
		return -1;
	return 0;
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
	struct sockaddr_in addr;
	int fd;
	int one = 1;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) ||
	    listen(fd, 1)) {
		perror("tcp listen");
		close(fd);
		return -1;
	}
	return fd;
}

static int tcp_connect(const char *host, int port)
{
	struct addrinfo hints;
	struct addrinfo *res = NULL, *ai;
	char port_s[16];
	int fd = -1;
	int gai;

	snprintf(port_s, sizeof(port_s), "%d", port);
	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_family = AF_UNSPEC;
	gai = getaddrinfo(host, port_s, &hints, &res);
	if (gai) {
		if (getenv("UC_ONEWAY_DEBUG")) {
			fprintf(stderr, "uc_oneway: getaddrinfo(%s,%s): %s\n",
				host, port_s, gai_strerror(gai));
			fflush(stderr);
		}
		return -1;
	}

	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (getenv("UC_ONEWAY_DEBUG")) {
			fprintf(stderr, "uc_oneway: connect family=%d\n",
				ai->ai_family);
			fflush(stderr);
		}
		if (!connect(fd, ai->ai_addr, ai->ai_addrlen))
			break;
		if (getenv("UC_ONEWAY_DEBUG")) {
			fprintf(stderr, "uc_oneway: connect failed errno=%d\n",
				errno);
			fflush(stderr);
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	return fd;
}

static struct ibv_context *open_dev(const char *name)
{
	struct ibv_device **list;
	struct ibv_context *ctx = NULL;
	int n = 0;

	list = ibv_get_device_list(&n);
	if (!list)
		return NULL;
	for (int i = 0; i < n; i++) {
		if (!strcmp(ibv_get_device_name(list[i]), name)) {
			ctx = ibv_open_device(list[i]);
			break;
		}
	}
	ibv_free_device_list(list);
	return ctx;
}

static int exchange_info(int fd, const struct peer_info *local,
			 struct peer_info *remote)
{
	if (send_all(fd, local, sizeof(*local)) ||
	    recv_all(fd, remote, sizeof(*remote))) {
		perror("metadata exchange");
		return -1;
	}
	if (remote->magic != local->magic) {
		fprintf(stderr, "bad remote magic 0x%x\n", remote->magic);
		return -1;
	}
	return 0;
}

static int qp_to_init(struct ibv_qp *qp, int port)
{
	struct ibv_qp_attr a;
	int tn3205 = getenv("UC_ONEWAY_TN3205") != NULL;
	int ret;

	memset(&a, 0, sizeof(a));
	a.qp_state = IBV_QPS_INIT;
	a.pkey_index = 0;
	a.port_num = (uint8_t)port;
	a.qp_access_flags = tn3205 ? 0 :
			    (IBV_ACCESS_LOCAL_WRITE |
			     IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
	errno = 0;
	ret = ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
			    IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
	fprintf(stderr, "modify INIT ret=%d errno=%d access_flags=0x%x\n",
		ret, errno, a.qp_access_flags);
	fflush(stderr);
	if (ret) {
		perror("modify INIT");
		return ret;
	}
	return 0;
}

static int qp_to_rts(struct ibv_qp *qp, int port, int sgid_index,
		     const union ibv_gid *dgid, uint32_t dlid,
		     uint32_t dest_qpn, uint32_t local_psn,
		     uint32_t remote_psn, enum ibv_mtu path_mtu)
{
	struct ibv_qp_attr a;
	int tn3205 = getenv("UC_ONEWAY_TN3205") != NULL;
	int ret;

	memset(&a, 0, sizeof(a));
	a.qp_state = IBV_QPS_RTR;
	a.path_mtu = path_mtu;
	a.rq_psn = remote_psn;
	a.dest_qp_num = dest_qpn;
	a.ah_attr.dlid = (uint16_t)dlid;
	a.ah_attr.sl = 0;
	a.ah_attr.src_path_bits = 0;
	a.ah_attr.port_num = (uint8_t)port;
	/* Match JACCL: only enable global routing when the dgid has a non-zero
	 * interface_id (i.e. it's a real GID, not the zero default). */
	a.ah_attr.is_global = tn3205 ? 1 : 0;
	if (tn3205 || dgid->global.interface_id) {
		a.ah_attr.is_global = 1;
		a.ah_attr.grh.dgid = *dgid;
		a.ah_attr.grh.sgid_index = sgid_index;
		a.ah_attr.grh.hop_limit = 1;
	}
	errno = 0;
	ret = ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_AV |
			    IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
			    IBV_QP_RQ_PSN);
	fprintf(stderr,
		"modify RTR ret=%d errno=%d dest_qpn=%u sgid_index=%d dlid=%u is_global=%d\n",
		ret, errno, dest_qpn, sgid_index, dlid, a.ah_attr.is_global);
	fflush(stderr);
	if (ret) {
		perror("modify RTR");
		return ret;
	}

	memset(&a, 0, sizeof(a));
	a.qp_state = IBV_QPS_RTS;
	a.sq_psn = local_psn;
	errno = 0;
	ret = ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_SQ_PSN);
	fprintf(stderr, "modify RTS ret=%d errno=%d\n", ret, errno);
	fflush(stderr);
	if (ret)
		perror("modify RTS");
	return ret;
}

static uint64_t wr_id_for_slot(int base, int slot)
{
	if (base >= 0)
		return (uint64_t)base | ((uint64_t)slot << 8);
	return (uint64_t)slot;
}

static int slot_from_wr_id(const struct opts *o, uint64_t wr_id)
{
	if (o->recv_wr_id_base >= 0)
		return (int)((wr_id >> 8) & 0xffu);
	return (int)wr_id;
}

static int post_recv_slot(const struct opts *o, struct ibv_qp *qp,
			  struct ibv_mr *mr, char *buf, size_t stride,
			  size_t size, int slot)
{
	struct ibv_sge sge;
	struct ibv_recv_wr wr;
	struct ibv_recv_wr *bad = NULL;

	memset(&sge, 0, sizeof(sge));
	sge.addr = (uintptr_t)(buf + (size_t)slot * stride);
	sge.length = (uint32_t)size;
	sge.lkey = mr->lkey;
	memset(&wr, 0, sizeof(wr));
	wr.wr_id = wr_id_for_slot(o->recv_wr_id_base, slot);
	wr.sg_list = &sge;
	wr.num_sge = 1;
	return ibv_post_recv(qp, &wr, &bad);
}

static void store_le64(char *p, uint64_t v)
{
	for (size_t i = 0; i < sizeof(v); i++)
		p[i] = (char)((v >> (i * 8)) & 0xff);
}

static uint64_t load_le64(const char *p)
{
	uint64_t v = 0;

	for (size_t i = 0; i < sizeof(v); i++)
		v |= (uint64_t)(unsigned char)p[i] << (i * 8);
	return v;
}

static unsigned char pattern_byte(uint64_t seq, size_t off)
{
	return (unsigned char)(0x40u ^ (unsigned int)seq ^
			       (unsigned int)(seq >> 8) ^
			       (unsigned int)(seq >> 16) ^
			       (unsigned int)(off * 131u) ^
			       (unsigned int)(off >> 8));
}

static void fill_pattern(char *p, size_t size, uint64_t seq)
{
	for (size_t i = 0; i < size; i++)
		p[i] = (char)pattern_byte(seq, i);
	if (size >= sizeof(seq))
		store_le64(p, seq);
}

static int check_pattern(const char *p, size_t size, uint64_t expected_seq,
			 int recv_slot, int completed, uint64_t wr_id,
			 uint32_t byte_len)
{
	uint64_t observed_seq = size >= sizeof(observed_seq) ? load_le64(p) : 0;

	for (size_t i = 0; i < size; i++) {
		unsigned char got = (unsigned char)p[i];
		unsigned char want;

		if (i < sizeof(expected_seq) && size >= sizeof(expected_seq))
			want = (unsigned char)((expected_seq >> (i * 8)) & 0xff);
		else
			want = pattern_byte(expected_seq, i);

		if (got != want) {
			fprintf(stderr,
				"check failed completed=%d wr_id=%llu recv_slot=%d expected_seq=%llu observed_seq=%llu byte_len=%u off=%zu got=0x%02x want=0x%02x\n",
				completed, (unsigned long long)wr_id,
				recv_slot, (unsigned long long)expected_seq,
				(unsigned long long)observed_seq, byte_len,
				i, got, want);
			return -1;
		}
	}
	return 0;
}

static int check_seen(uint8_t *seen, int count, uint64_t observed_seq,
		      int recv_slot, int completed, uint64_t wr_id)
{
	size_t byte;
	uint8_t bit;

	if (observed_seq >= (uint64_t)count) {
		fprintf(stderr,
			"check failed completed=%d wr_id=%llu recv_slot=%d observed_seq=%llu out_of_range count=%d\n",
			completed, (unsigned long long)wr_id, recv_slot,
			(unsigned long long)observed_seq, count);
		return -1;
	}

	byte = (size_t)(observed_seq / 8);
	bit = (uint8_t)(1u << (observed_seq % 8));
	if (seen[byte] & bit) {
		fprintf(stderr,
			"check failed completed=%d wr_id=%llu recv_slot=%d observed_seq=%llu duplicate\n",
			completed, (unsigned long long)wr_id, recv_slot,
			(unsigned long long)observed_seq);
		return -1;
	}

	seen[byte] |= bit;
	return 0;
}

static void print_rate(const char *role, int done, int count, size_t size,
		       uint64_t start, uint64_t *last_t, int *last_done)
{
	uint64_t now = now_ns();

	if (now - *last_t < 1000000000ull && done != count)
		return;
	if (done == count && done == *last_done)
		return;

	double dt = (now - *last_t) / 1e9;
	double total = (now - start) / 1e9;
	int delta = done - *last_done;
	double mbps = dt > 0 ? (double)delta * (double)size * 8.0 / dt / 1e6 : 0;

	printf("%s progress done=%d/%d delta=%d rate=%.2f Mbit/s elapsed=%.3f s\n",
	       role, done, count, delta, mbps, total);
	fflush(stdout);
	*last_t = now;
	*last_done = done;
}

int main(int argc, char **argv)
{
	struct opts o;
	struct ibv_context *ctx;
	struct ibv_port_attr port_attr;
	union ibv_gid local_gid, remote_gid;
	struct ibv_pd *pd;
	struct ibv_cq *cq;
	struct ibv_qp *qp;
	struct ibv_qp_init_attr qpia;
	struct peer_info local, remote;
	int is_sender;
	int is_bidi;
	int sock = -1, listen_fd = -1;
	int ret = 1;
	uint32_t psn;
	size_t page_size, stride, bufsz, send_region_size = 0;
	char *buf = NULL;
	char *send_buf = NULL;
	char *recv_buf = NULL;
	struct ibv_mr *mr = NULL;
	uint64_t start, last_t;
	int last_done = 0;
	int initial_recvs = 0;
	int wr_depth;
	int send_depth;
	int send_slots;
	int sgid_index;
	uint8_t *seen = NULL;

	if (parse_opts(argc, argv, &o)) {
		usage(argv[0]);
		return 2;
	}
	if (load_verbs_provider())
		return 1;
	is_sender = !strcmp(o.role, "send");
	is_bidi = !strcmp(o.role, "bidi");
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (!is_sender || is_bidi) {
		initial_recvs = o.recv_posts ? o.recv_posts : o.depth;
		if (initial_recvs > o.count && !o.recv_posts)
			initial_recvs = o.count;
	}
	wr_depth = o.depth;
	send_depth = o.depth;
	if (!is_sender && initial_recvs > wr_depth)
		wr_depth = initial_recvs;
	send_slots = (is_sender || is_bidi) ?
		     (o.send_slots ? o.send_slots : o.depth) : 0;
	if (send_slots < 0 || send_slots > o.count)
		send_slots = o.count;
	if ((is_sender || is_bidi) && o.check && send_slots < o.depth) {
		fprintf(stderr,
			"uc_oneway: --check requires --send-slots >= --depth; "
			"otherwise in-flight SEND buffers may be overwritten "
			"(send_slots=%d depth=%d)\n",
			send_slots, o.depth);
		return 2;
	}

	debug_step("open device");
	ctx = open_dev(o.dev);
	if (!ctx) {
		fprintf(stderr, "failed to open RDMA device %s\n", o.dev);
		return 1;
	}
	debug_step("query port");
	if (ibv_query_port(ctx, (uint8_t)o.ib_port, &port_attr)) {
		perror("ibv_query_port");
		goto out_ctx;
	}
	debug_step("query gid");
	if (select_gid(ctx, o.ib_port, o.gid_index, port_attr.gid_tbl_len,
		       &local_gid, &sgid_index))
		goto out_ctx;
	fprintf(stderr, "selected sgid_index=%d local_gid=", sgid_index);
	print_gid(stderr, &local_gid);
	fprintf(stderr, "\n");

	debug_step("alloc pd");
	pd = ibv_alloc_pd(ctx);
	if (!pd) {
		perror("ibv_alloc_pd");
		goto out_ctx;
	}

	long sys_page_size = sysconf(_SC_PAGESIZE);
	if (sys_page_size <= 0) {
		perror("sysconf(_SC_PAGESIZE)");
		goto out_pd;
	}
	page_size = (size_t)sys_page_size;
	stride = align_up(o.size, page_size);
	if (is_bidi) {
		send_region_size = stride * (size_t)send_slots;
		bufsz = align_up(send_region_size + stride * (size_t)wr_depth,
				 page_size);
	} else if (is_sender) {
		bufsz = align_up(stride * (size_t)send_slots, page_size);
	} else {
		bufsz = align_up(stride * (size_t)wr_depth, page_size);
	}
	if (posix_memalign((void **)&buf, page_size, bufsz)) {
		fprintf(stderr, "posix_memalign failed\n");
		goto out_pd;
	}
	memset(buf, is_sender ? 0x5a : 0xcc, bufsz);
	send_buf = buf;
	recv_buf = is_bidi ? buf + send_region_size : buf;
	debug_step("register mr");
	int mr_access = getenv("UC_ONEWAY_TN3205") ?
			IBV_ACCESS_LOCAL_WRITE :
			(IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
			 IBV_ACCESS_REMOTE_WRITE);
	fprintf(stderr, "register mr len=%zu access=0x%x\n", bufsz,
		mr_access);
	mr = ibv_reg_mr(pd, buf, bufsz, mr_access);
	if (!mr) {
		perror("ibv_reg_mr");
		goto out_buf;
	}

	if (o.check_any_order && (!is_sender || is_bidi)) {
		seen = calloc(((size_t)o.count + 7u) / 8u, 1);
		if (!seen) {
			perror("calloc seen");
			goto out_mr;
		}
	}

	debug_step("create cq");
	cq = ibv_create_cq(ctx, wr_depth + 16, NULL, NULL, 0);
	if (!cq) {
		perror("ibv_create_cq");
		goto out_mr;
	}

	debug_step("create qp");
	memset(&qpia, 0, sizeof(qpia));
	qpia.qp_context = ctx;
	qpia.send_cq = cq;
	qpia.recv_cq = cq;
	qpia.cap.max_send_wr = (is_sender || is_bidi) ? (uint32_t)send_depth : 1;
	qpia.cap.max_recv_wr = (!is_sender || is_bidi) ? (uint32_t)wr_depth : 1;
	qpia.cap.max_send_sge = 1;
	qpia.cap.max_recv_sge = 1;
	qpia.qp_type = IBV_QPT_UC;
	qp = ibv_create_qp(pd, &qpia);
	if (!qp) {
		perror("ibv_create_qp");
		goto out_cq;
	}
	fprintf(stderr,
		"created QP qp_num=%u actual_cap send_wr=%u recv_wr=%u send_sge=%u recv_sge=%u\n",
		qp->qp_num, qpia.cap.max_send_wr, qpia.cap.max_recv_wr,
		qpia.cap.max_send_sge, qpia.cap.max_recv_sge);
	fflush(stderr);
	if ((is_sender || is_bidi) && !qpia.cap.max_send_wr) {
		fprintf(stderr, "provider returned max_send_wr=0\n");
		goto out_qp;
	}
	if ((!is_sender || is_bidi) && !qpia.cap.max_recv_wr) {
		fprintf(stderr, "provider returned max_recv_wr=0\n");
		goto out_qp;
	}
	if ((is_sender || is_bidi) &&
	    send_depth > (int)qpia.cap.max_send_wr) {
		fprintf(stderr,
			"clamping send depth %d to provider max_send_wr %u\n",
			send_depth, qpia.cap.max_send_wr);
		send_depth = (int)qpia.cap.max_send_wr;
	}
	if ((!is_sender || is_bidi) &&
	    initial_recvs > (int)qpia.cap.max_recv_wr) {
		fprintf(stderr,
			"clamping initial recv posts %d to provider max_recv_wr %u\n",
			initial_recvs, qpia.cap.max_recv_wr);
		initial_recvs = (int)qpia.cap.max_recv_wr;
	}
	debug_step("modify init");
	if (qp_to_init(qp, o.ib_port))
		goto out_qp;

	/* Match JACCL's fixed initial PSN to rule it out as a delivery factor. */
	psn = 7;
	memset(&local, 0, sizeof(local));
	local.magic = 0x55433157u; /* UC1W */
	local.qpn = qp->qp_num;
	local.psn = psn;
	local.lid = port_attr.lid;
	if (getenv("UC_ONEWAY_LID_OVERRIDE"))
		local.lid = (uint32_t)strtoul(getenv("UC_ONEWAY_LID_OVERRIDE"),
					      NULL, 0);
	memcpy(local.gid, local_gid.raw, sizeof(local.gid));

	if (o.connect_host) {
		debug_step("tcp connect");
		sock = tcp_connect(o.connect_host, o.port);
		if (sock < 0) {
			perror("tcp connect");
			goto out_qp;
		}
		debug_step("tcp connected");
	} else {
		debug_step("tcp listen");
		listen_fd = tcp_listen(o.port);
		if (listen_fd < 0)
			goto out_qp;
		printf("listening on TCP port %d\n", o.port);
		debug_step("tcp accept");
		sock = accept(listen_fd, NULL, NULL);
		if (sock < 0) {
			perror("accept");
			goto out_qp;
		}
		debug_step("tcp accepted");
		close(listen_fd);
		listen_fd = -1;
	}

	debug_step("exchange info");
	if (exchange_info(sock, &local, &remote))
		goto out_qp;
	debug_step("exchange done");
	if (getenv("UC_ONEWAY_REMOTE_QPN_OVERRIDE"))
		remote.qpn = (uint32_t)strtoul(getenv("UC_ONEWAY_REMOTE_QPN_OVERRIDE"),
					       NULL, 0);
	memcpy(remote_gid.raw, remote.gid, sizeof(remote.gid));
	printf("%s local_qpn=%u remote_qpn=%u size=%zu count=%d depth=%d send_slots=%d recv_posts=%d mtu=%d\n",
	       o.role, local.qpn, remote.qpn, o.size, o.count, o.depth,
	       send_slots, initial_recvs, o.mtu);
	fprintf(stderr, "local_gid=");
	print_gid(stderr, &local_gid);
	fprintf(stderr, " remote_gid=");
	print_gid(stderr, &remote_gid);
	fprintf(stderr, "\n");

	fprintf(stderr, "transitioning QP to RTR/RTS\n");
	fflush(stderr);
	if (qp_to_rts(qp, o.ib_port, sgid_index, &remote_gid, remote.lid,
		      remote.qpn, local.psn, remote.psn, mtu_enum(o.mtu)))
		goto out_qp;
	fprintf(stderr, "QP is RTS\n");
	fflush(stderr);
	if (o.hold_after_rts_ms > 0) {
		fprintf(stderr, "holding after RTS for %d ms\n",
			o.hold_after_rts_ms);
		fflush(stderr);
		usleep((useconds_t)o.hold_after_rts_ms * 1000u);
	}

	if (!is_sender || is_bidi) {
		if (o.recv_post_delay_ms > 0) {
			fprintf(stderr, "delaying receive posts by %d ms\n",
				o.recv_post_delay_ms);
			fflush(stderr);
			usleep((useconds_t)o.recv_post_delay_ms * 1000u);
		}
		fprintf(stderr, "posting %d receives\n", initial_recvs);
		fflush(stderr);
		int posted_initial_recvs = 0;
		for (int i = 0; i < initial_recvs; i++) {
			errno = 0;
			int post_ret = post_recv_slot(&o, qp, mr, recv_buf,
						      stride, o.size, i);
			if (post_ret) {
				if (i > 0) {
					fprintf(stderr,
						"provider accepted only %d/%d initial receives; continuing with repost window %d (ret=%d errno=%d)\n",
						i, initial_recvs, i, post_ret,
						errno);
					initial_recvs = i;
					break;
				}
				fprintf(stderr,
					"ibv_post_recv[%d/%d] ret=%d errno=%d\n",
					i, initial_recvs, post_ret, errno);
				goto out_qp;
			}
			posted_initial_recvs++;
		}
		fprintf(stderr, "posted %d receives\n", posted_initial_recvs);
		fflush(stderr);
	}

	if (is_bidi) {
		char ready = 'B';
		char peer_ready;

		if (send_all(sock, &ready, 1)) {
			perror("bidi ready send");
			goto out_qp;
		}
		if (recv_all(sock, &peer_ready, 1) || peer_ready != 'B') {
			fprintf(stderr, "peer did not signal bidi ready\n");
			goto out_qp;
		}
	} else if (is_sender) {
		char ready;
		char ack = 'A';

		if (recv_all(sock, &ready, 1) || ready != 'R') {
			fprintf(stderr, "receiver did not signal ready\n");
			goto out_qp;
		}
		/* Two-way handshake: confirm to receiver that we saw 'R'
		 * before posting the first SEND. This pins down a kernel-
		 * side QP-registration race window where the first SENDs
		 * arrive at the receiver before its lookup_qp can find the
		 * QP, charging up rx_no_qp and poisoning credit accounting. */
		if (send_all(sock, &ack, 1)) {
			perror("ack send");
			goto out_qp;
		}
	} else {
		char ready = 'R';
		char ack;

		if (send_all(sock, &ready, 1)) {
			perror("ready send");
			goto out_qp;
		}
		if (recv_all(sock, &ack, 1) || ack != 'A') {
			fprintf(stderr, "sender did not ack ready\n");
			goto out_qp;
		}
	}

	start = now_ns();
	last_t = start;
	if (is_bidi) {
		struct ibv_wc wc[32];
		int send_posted = 0, send_completed = 0, send_in_flight = 0;
		int recv_posted = initial_recvs, recv_completed = 0;

		while (send_completed < o.count || recv_completed < o.count) {
			while (send_posted < o.count &&
			       send_in_flight < send_depth) {
				int slot = send_posted % send_slots;
				struct ibv_sge sge;
				struct ibv_send_wr wr;
				struct ibv_send_wr *bad = NULL;

				memset(&sge, 0, sizeof(sge));
				if (o.check)
					fill_pattern(send_buf + (size_t)slot * stride,
						     o.size, (uint64_t)send_posted);
				sge.addr = (uintptr_t)(send_buf + (size_t)slot * stride);
				sge.length = (uint32_t)o.size;
				sge.lkey = mr->lkey;
				memset(&wr, 0, sizeof(wr));
				wr.wr_id = o.send_wr_id_base >= 0 ?
					   wr_id_for_slot(o.send_wr_id_base, slot) :
					   0x8000000000000000ull |
						   (uint64_t)send_posted;
				wr.sg_list = &sge;
				wr.num_sge = 1;
				wr.opcode = IBV_WR_SEND;
				wr.send_flags = IBV_SEND_SIGNALED;
				if (ibv_post_send(qp, &wr, &bad)) {
					if (send_in_flight > 0 &&
					    send_depth > send_in_flight) {
						fprintf(stderr,
							"provider accepted only %d/%d in-flight sends; continuing with send depth %d\n",
							send_in_flight, send_depth,
							send_in_flight);
						send_depth = send_in_flight;
						break;
					}
					perror("ibv_post_send");
					goto out_qp;
				}
				send_posted++;
				send_in_flight++;
			}

			int n = ibv_poll_cq(cq, 32, wc);
			if (n < 0) {
				fprintf(stderr, "ibv_poll_cq failed: %d\n", n);
				goto out_qp;
			}
			for (int i = 0; i < n; i++) {
				if (wc[i].status != IBV_WC_SUCCESS) {
					fprintf(stderr,
						"bidi wc error wr_id=%llu status=%u opcode=%u byte_len=%u sent=%d/%d recv=%d/%d\n",
						(unsigned long long)wc[i].wr_id,
						wc[i].status, wc[i].opcode,
						wc[i].byte_len, send_completed,
						o.count, recv_completed,
						o.count);
					goto out_qp;
				}
				if (wc[i].opcode == IBV_WC_SEND ||
				    (wc[i].wr_id & 0x8000000000000000ull)) {
					send_completed++;
					send_in_flight--;
					continue;
				}

				int slot = slot_from_wr_id(&o, wc[i].wr_id);

				if (o.check) {
					char *p = recv_buf + (size_t)slot * stride;
					uint64_t seq = o.check_any_order ?
						load_le64(p) :
						(uint64_t)recv_completed;

					if (o.check_any_order &&
					    check_seen(seen, o.count, seq, slot,
						       recv_completed, wc[i].wr_id))
						goto out_qp;
					if (check_pattern(p, o.size, seq, slot,
							  recv_completed,
							  wc[i].wr_id,
							  wc[i].byte_len))
						goto out_qp;
				}
				recv_completed++;
				if (recv_posted < o.count) {
					errno = 0;
					int post_ret = post_recv_slot(&o, qp, mr,
								      recv_buf,
								      stride,
								      o.size,
								      slot);
					if (post_ret) {
						fprintf(stderr,
							"ibv_post_recv[repost slot=%d posted=%d count=%d] ret=%d errno=%d\n",
							slot, recv_posted,
							o.count, post_ret,
							errno);
						goto out_qp;
					}
					recv_posted++;
				}
			}
			print_rate("bidi-recv", recv_completed, o.count, o.size,
				   start, &last_t, &last_done);
		}
		printf("bidi done send=%d/%d recv=%d/%d\n", send_completed,
		       o.count, recv_completed, o.count);
		fflush(stdout);
		{
			char done = 'D';
			char peer_done;

			if (send_all(sock, &done, 1)) {
				perror("bidi done send");
				goto out_qp;
			}
			if (recv_all(sock, &peer_done, 1) || peer_done != 'D') {
				fprintf(stderr, "peer did not signal bidi done\n");
				goto out_qp;
			}
		}
	} else if (is_sender) {
		struct ibv_wc wc[32];
		int posted = 0, completed = 0, in_flight = 0;

		while (completed < o.count) {
			while (posted < o.count && in_flight < send_depth) {
				int slot = posted % send_slots;
				struct ibv_sge sge;
				struct ibv_send_wr wr;
				struct ibv_send_wr *bad = NULL;

				memset(&sge, 0, sizeof(sge));
				if (o.check)
					fill_pattern(send_buf + (size_t)slot * stride,
						     o.size, (uint64_t)posted);
				sge.addr = (uintptr_t)(send_buf + (size_t)slot * stride);
				sge.length = (uint32_t)o.size;
				sge.lkey = mr->lkey;
				memset(&wr, 0, sizeof(wr));
				wr.wr_id = o.send_wr_id_base >= 0 ?
					   wr_id_for_slot(o.send_wr_id_base, slot) :
					   (uint64_t)posted;
				wr.sg_list = &sge;
				wr.num_sge = 1;
				wr.opcode = IBV_WR_SEND;
				wr.send_flags = IBV_SEND_SIGNALED;
				if (ibv_post_send(qp, &wr, &bad)) {
					if (in_flight > 0 && send_depth > in_flight) {
						fprintf(stderr,
							"provider accepted only %d/%d in-flight sends; continuing with send depth %d\n",
							in_flight, send_depth,
							in_flight);
						send_depth = in_flight;
						break;
					}
					perror("ibv_post_send");
					goto out_qp;
				}
				posted++;
				in_flight++;
			}

			int n = ibv_poll_cq(cq, 32, wc);
			if (n < 0) {
				fprintf(stderr, "ibv_poll_cq failed: %d\n", n);
				goto out_qp;
			}
			for (int i = 0; i < n; i++) {
				if (wc[i].status != IBV_WC_SUCCESS) {
					fprintf(stderr,
						"send wc error wr_id=%llu status=%u opcode=%u\n",
						(unsigned long long)wc[i].wr_id,
						wc[i].status, wc[i].opcode);
					goto out_qp;
				}
				completed++;
				in_flight--;
			}
			print_rate("send", completed, o.count, o.size, start,
				   &last_t, &last_done);
		}
	} else {
		struct ibv_wc wc[32];
		int completed = 0, posted = initial_recvs;

		while (completed < o.count) {
			int n = ibv_poll_cq(cq, 32, wc);
			if (n < 0) {
				fprintf(stderr, "ibv_poll_cq failed: %d\n", n);
				goto out_qp;
			}
			for (int i = 0; i < n; i++) {
				int slot = slot_from_wr_id(&o, wc[i].wr_id);

				if (wc[i].status != IBV_WC_SUCCESS) {
					fprintf(stderr,
						"recv wc error wr_id=%llu status=%u opcode=%u byte_len=%u\n",
						(unsigned long long)wc[i].wr_id,
						wc[i].status, wc[i].opcode,
						wc[i].byte_len);
					goto out_qp;
				}
				if (o.check) {
					char *p = recv_buf + (size_t)slot * stride;
					uint64_t seq = o.check_any_order ?
						load_le64(p) : (uint64_t)completed;

					if (o.check_any_order &&
					    check_seen(seen, o.count, seq, slot,
						       completed, wc[i].wr_id))
						goto out_qp;
					if (check_pattern(p, o.size, seq, slot,
							  completed, wc[i].wr_id,
							  wc[i].byte_len))
						goto out_qp;
				}
				completed++;
				if (posted < o.count) {
					errno = 0;
					int post_ret = post_recv_slot(&o, qp, mr,
								      recv_buf,
								      stride,
								      o.size,
								      slot);
					if (post_ret) {
						fprintf(stderr,
							"ibv_post_recv[repost slot=%d posted=%d count=%d] ret=%d errno=%d\n",
							slot, posted,
							o.count, post_ret,
							errno);
						goto out_qp;
					}
					posted++;
				}
			}
			print_rate("recv", completed, o.count, o.size, start,
				   &last_t, &last_done);
		}
	}

	print_rate(o.role, o.count, o.count, o.size, start, &last_t, &last_done);
	if (o.hold_before_destroy_ms > 0) {
		fprintf(stderr, "holding before destroy for %d ms\n",
			o.hold_before_destroy_ms);
		fflush(stderr);
		usleep((useconds_t)o.hold_before_destroy_ms * 1000u);
	}
	ret = 0;

out_qp:
	if (listen_fd >= 0)
		close(listen_fd);
	if (sock >= 0)
		close(sock);
	ibv_destroy_qp(qp);
out_cq:
	ibv_destroy_cq(cq);
out_mr:
	free(seen);
	if (mr)
		ibv_dereg_mr(mr);
out_buf:
	free(buf);
out_pd:
	ibv_dealloc_pd(pd);
out_ctx:
	ibv_close_device(ctx);
	return ret;
}
