// SPDX-License-Identifier: MIT
/*
 * Tiny macOS RDMA-over-Thunderbolt verbs probe.
 *
 * This is intentionally conservative: it opens one Apple rdma_enN device,
 * queries port/GID state, creates a UC QP, and moves it to INIT. By default
 * it does not post recv/send WRs and does not register memory.
 *
 * Set MAC_TB_RDMA_PROBE_RTR=1 to also attempt INIT->RTR->RTS. That path can
 * block inside Apple's provider when pointed at a fake/non-Apple peer.
 * Set MAC_TB_RDMA_PROBE_SEND=1 to also register a buffer, post one recv, and
 * attempt one signaled SEND after RTS. Use MAC_TB_RDMA_PROBE_ALARM_SEC=N while
 * probing fake peers so a provider-side block is bounded.
 * Set MAC_TB_RDMA_PROBE_DOC_SHAPE=1 to use TN3205-shaped QP parameters:
 * CQ depth 4096, send/recv queue depth 4095, MTU 4096, and no QP access flags.
 * Set MAC_TB_RDMA_PROBE_SLEEP_SEC=N to keep resources alive briefly before
 * cleanup so IORegistry state can be inspected from another shell.
 * Set MAC_TB_RDMA_PROBE_RECV=1 to post a receive and poll for a receive WC
 * without also posting a SEND.
 * Set MAC_TB_RDMA_PROBE_ITERS=N to post N same-sized receive/send WRs on the
 * same QP. Set MAC_TB_RDMA_PROBE_NO_RECV=1 with SEND=1 for sender-only loops.
 * Set MAC_TB_RDMA_PROBE_PATTERN=inc to fill the send buffer with incrementing
 * byte values instead of 0x5a. Use off32 to write the byte offset as a
 * little-endian uint32_t at each 4-byte boundary.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void alarm_handler(int signo)
{
	ssize_t ignored;

	(void)signo;
	ignored = write(STDERR_FILENO, "mac_tb_rdma_probe: alarm fired\n", 31);
	(void)ignored;
	_exit(124);
}

static void print_gid(const union ibv_gid *gid)
{
	for (int i = 0; i < 16; i += 2)
		printf("%02x%02x%s", gid->raw[i], gid->raw[i + 1],
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

static int parse_gid_index(const char *value, int *index)
{
	char *end = NULL;
	long parsed;

	if (!value || !strcmp(value, "auto")) {
		*index = -1;
		return 0;
	}

	errno = 0;
	parsed = strtol(value, &end, 0);
	if (errno || !end || *end || parsed < 0 || parsed > INT_MAX)
		return -1;
	*index = (int)parsed;
	return 0;
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
		if (ibv_query_gid(ctx, port, requested_index, gid))
			return -1;
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

static int parse_ipv4_gid(const char *ip, union ibv_gid *gid)
{
	struct in_addr addr;

	memset(gid, 0, sizeof(*gid));
	if (inet_pton(AF_INET, ip, &addr) != 1)
		return -1;

	gid->raw[10] = 0xff;
	gid->raw[11] = 0xff;
	memcpy(&gid->raw[12], &addr.s_addr, sizeof(addr.s_addr));
	return 0;
}

static const char *status_text(int ret, int saved_errno)
{
	if (!ret)
		return "ok";
	return strerror(ret > 0 ? ret : saved_errno);
}

static void dump_gids(struct ibv_context *ctx, int port, int limit)
{
	for (int i = 0; i < limit; i++) {
		union ibv_gid gid;

		memset(&gid, 0, sizeof(gid));
		if (ibv_query_gid(ctx, port, i, &gid))
			continue;
		if (gid_is_zero(&gid))
			continue;

		printf("gid[%d]=", i);
		print_gid(&gid);
		printf("\n");
	}
}

static struct ibv_device *find_device(struct ibv_device **list, int count,
				      const char *name)
{
	for (int i = 0; i < count; i++) {
		const char *dev_name = ibv_get_device_name(list[i]);

		if (!name || !strcmp(dev_name, name))
			return list[i];
	}

	return NULL;
}

static void fill_send_buffer(void *buf, size_t len)
{
	const char *pattern = getenv("MAC_TB_RDMA_PROBE_PATTERN");
	uint8_t *bytes = buf;
	size_t i;

	if (!pattern) {
		memset(buf, 0x5a, len);
		return;
	}

	if (!strcmp(pattern, "inc")) {
		for (i = 0; i < len; i++)
			bytes[i] = (uint8_t)i;
		return;
	}

	if (!strcmp(pattern, "off32")) {
		for (i = 0; i < len; i += 4) {
			uint32_t offset = (uint32_t)i;

			for (size_t j = 0; j < 4 && i + j < len; j++)
				bytes[i + j] = (uint8_t)(offset >> (j * 8));
		}
		return;
	}

	memset(buf, 0x5a, len);
}

static void dump_prefix(const void *buf, size_t len)
{
	const uint8_t *bytes = buf;
	size_t dump_len = len < 64 ? len : 64;

	printf("buffer[0..%zu):", dump_len);
	for (size_t i = 0; i < dump_len; i++) {
		if (i % 16 == 0)
			printf("\n  %04zx:", i);
		printf(" %02x", bytes[i]);
	}
	printf("\n");
}

static void dump_window(const void *buf, size_t len, size_t off)
{
	const uint8_t *bytes = buf;
	size_t end;

	if (off >= len)
		return;

	end = off + 32;
	if (end > len)
		end = len;

	printf("buffer[%zu..%zu):", off, end);
	for (size_t i = off; i < end; i++) {
		if ((i - off) % 16 == 0)
			printf("\n  %04zx:", i);
		printf(" %02x", bytes[i]);
	}
	printf("\n");
}

static void dump_gap_windows(const void *buf, size_t len)
{
	size_t offsets[] = {0x0f0, 0x1f0, 0x2f0, 0x300, 0x3f0, 0xff0};

	for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
		dump_window(buf, len, offsets[i]);
}

int main(int argc, char **argv)
{
	const char *dev_name = argc > 1 ? argv[1] : "rdma_en1";
	const char *remote_ip = argc > 2 ? argv[2] : NULL;
	int remote_qpn = argc > 3 ? atoi(argv[3]) : 2;
	int remote_psn = argc > 4 ? atoi(argv[4]) : 7;
	int remote_lid = argc > 5 ? atoi(argv[5]) : -1;
	int do_rtr = getenv("MAC_TB_RDMA_PROBE_RTR") != NULL;
	int do_send = getenv("MAC_TB_RDMA_PROBE_SEND") != NULL;
	int do_recv = getenv("MAC_TB_RDMA_PROBE_RECV") != NULL;
	int no_recv = getenv("MAC_TB_RDMA_PROBE_NO_RECV") != NULL;
	int doc_shape = getenv("MAC_TB_RDMA_PROBE_DOC_SHAPE") != NULL;
	int self_qpn = getenv("MAC_TB_RDMA_PROBE_SELF_QPN") != NULL;
	int wait_all = getenv("MAC_TB_RDMA_PROBE_WAIT_ALL") != NULL;
	int requested_gid_index = -1;
	int alarm_sec = getenv("MAC_TB_RDMA_PROBE_ALARM_SEC") ?
				atoi(getenv("MAC_TB_RDMA_PROBE_ALARM_SEC")) : 0;
	int sleep_sec = getenv("MAC_TB_RDMA_PROBE_SLEEP_SEC") ?
				atoi(getenv("MAC_TB_RDMA_PROBE_SLEEP_SEC")) : 0;
	int no_round = getenv("MAC_TB_RDMA_PROBE_NO_ROUND") != NULL;
	size_t send_len = getenv("MAC_TB_RDMA_PROBE_SEND_LEN") ?
				  strtoul(getenv("MAC_TB_RDMA_PROBE_SEND_LEN"),
					  NULL, 0) : 0;
	unsigned int iters = getenv("MAC_TB_RDMA_PROBE_ITERS") ?
				     (unsigned int)strtoul(getenv("MAC_TB_RDMA_PROBE_ITERS"),
							   NULL, 0) : 1;
	size_t page_align = 4096;
	size_t buf_len;
	int poll_ms = getenv("MAC_TB_RDMA_PROBE_POLL_MS") ?
			      atoi(getenv("MAC_TB_RDMA_PROBE_POLL_MS")) : 1000;
	struct ibv_qp_init_attr qpia;
	int max_wr = doc_shape ? 4095 : 4;
	int cq_depth = doc_shape ? max_wr + 1 : 8;
	int path_mtu = doc_shape ? IBV_MTU_4096 : IBV_MTU_1024;
	int access_flags = doc_shape ? 0 :
		(IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
		 IBV_ACCESS_REMOTE_WRITE);
	int mr_access = IBV_ACCESS_LOCAL_WRITE;
	struct ibv_port_attr port_attr;
	struct ibv_device **list;
	struct ibv_device *dev;
	struct ibv_context *ctx;
	struct ibv_pd *pd;
	struct ibv_cq *cq;
	struct ibv_qp *qp;
	struct ibv_mr *mr = NULL;
	void *buf = NULL;
	union ibv_gid local_gid;
	union ibv_gid remote_gid;
	int num_devices;
	int sgid_index;
	int ret;
	const uint64_t recv_wr_id_base = 0xabc00000ULL;
	const uint64_t send_wr_id_base = 0xabc10000ULL;

	setvbuf(stdout, NULL, _IOLBF, 0);
	{
		long sys_page = sysconf(_SC_PAGESIZE);

		if (sys_page > 0)
			page_align = (size_t)sys_page;
		if (!send_len)
			send_len = page_align;
		if (send_len % page_align && !no_round) {
			size_t rounded = ((send_len + page_align - 1) /
					  page_align) * page_align;

			printf("rounding send_len from %zu to page multiple %zu\n",
			       send_len, rounded);
			send_len = rounded;
		}
	}
	if (!iters)
		iters = 1;
	if (parse_gid_index(getenv("MAC_TB_RDMA_PROBE_GID_INDEX"),
			    &requested_gid_index)) {
		fprintf(stderr,
			"invalid MAC_TB_RDMA_PROBE_GID_INDEX; use auto or a non-negative integer\n");
		return 1;
	}
	if (send_len > UINT_MAX) {
		fprintf(stderr, "send_len too large for one SGE: %zu\n",
			send_len);
		return 1;
	}
	if (iters > (unsigned int)max_wr) {
		fprintf(stderr, "iters=%u exceeds max_wr=%d\n", iters,
			max_wr);
		return 1;
	}
	if (send_len && iters > ((size_t)-1) / send_len) {
		fprintf(stderr, "send_len * iters overflows\n");
		return 1;
	}
	buf_len = send_len * iters;
	if (do_send && !no_recv)
		do_recv = 1;
	if (do_send)
		do_rtr = 1;
	if (do_recv)
		do_rtr = 1;
	if (alarm_sec > 0) {
		signal(SIGALRM, alarm_handler);
		alarm((unsigned int)alarm_sec);
	}

	list = ibv_get_device_list(&num_devices);
	if (!list) {
		perror("ibv_get_device_list");
		return 1;
	}

	dev = find_device(list, num_devices, dev_name);
	if (!dev) {
		fprintf(stderr, "device %s not found\n", dev_name);
		ibv_free_device_list(list);
		return 1;
	}

	ctx = ibv_open_device(dev);
	if (!ctx) {
		perror("ibv_open_device");
		ibv_free_device_list(list);
		return 1;
	}

	printf("opened %s\n", ibv_get_device_name(dev));

	memset(&port_attr, 0, sizeof(port_attr));
	ret = ibv_query_port(ctx, 1, &port_attr);
	if (ret) {
		fprintf(stderr, "ibv_query_port failed: %d\n", ret);
		goto out_close;
	}

	printf("port state=%u lid=%u gid_tbl_len=%u active_mtu=%u\n",
	       port_attr.state, port_attr.lid, port_attr.gid_tbl_len,
	       port_attr.active_mtu);
	dump_gids(ctx, 1, port_attr.gid_tbl_len < 8 ? port_attr.gid_tbl_len : 8);

	if (select_gid(ctx, 1, requested_gid_index, port_attr.gid_tbl_len,
		       &local_gid, &sgid_index)) {
		ret = EINVAL;
		goto out_close;
	}
	printf("using sgid[%d]=", sgid_index);
	print_gid(&local_gid);
	printf("\n");

	if (remote_ip) {
		if (parse_ipv4_gid(remote_ip, &remote_gid)) {
			fprintf(stderr, "invalid remote IPv4 address: %s\n",
				remote_ip);
			ret = EINVAL;
			goto out_close;
		}
	} else {
		remote_gid = local_gid;
		remote_qpn = 2;
	}

	printf("selected sgid_index=%d remote_qpn=%d remote_psn=%d dgid=",
	       sgid_index, remote_qpn, remote_psn);
	print_gid(&remote_gid);
	printf("\n");

	pd = ibv_alloc_pd(ctx);
	if (!pd) {
		perror("ibv_alloc_pd");
		ret = errno;
		goto out_close;
	}

	cq = ibv_create_cq(ctx, cq_depth, NULL, NULL, 0);
	if (!cq) {
		perror("ibv_create_cq");
		ret = errno;
		goto out_pd;
	}

	memset(&qpia, 0, sizeof(qpia));
	qpia.send_cq = cq;
	qpia.recv_cq = cq;
	qpia.cap.max_send_wr = max_wr;
	qpia.cap.max_recv_wr = max_wr;
	qpia.cap.max_send_sge = 1;
	qpia.cap.max_recv_sge = 1;
	qpia.qp_type = IBV_QPT_UC;
	qpia.sq_sig_all = 0;

	qp = ibv_create_qp(pd, &qpia);
	if (!qp) {
		perror("ibv_create_qp");
		ret = errno;
		goto out_cq;
	}
	printf("created UC qp_num=%u\n", qp->qp_num);
	if (self_qpn) {
		remote_qpn = (int)qp->qp_num;
		remote_gid = local_gid;
		printf("self_qpn override: remote_qpn=%d dgid=", remote_qpn);
		print_gid(&remote_gid);
		printf("\n");
	}
	printf("probe shape: cq_depth=%d max_wr=%d path_mtu=%d access_flags=0x%x mr_access=0x%x\n",
	       cq_depth, max_wr, path_mtu, access_flags, mr_access);
	{
		struct ibv_qp_attr attr;
		struct ibv_qp_init_attr init_attr;

		memset(&attr, 0, sizeof(attr));
		memset(&init_attr, 0, sizeof(init_attr));
		ret = ibv_query_qp(qp, &attr, IBV_QP_CAP, &init_attr);
		if (!ret)
			printf("actual cap: max_send_wr=%u max_recv_wr=%u max_send_sge=%u max_recv_sge=%u\n",
			       init_attr.cap.max_send_wr,
			       init_attr.cap.max_recv_wr,
			       init_attr.cap.max_send_sge,
			       init_attr.cap.max_recv_sge);
		else
			printf("query_qp cap ret=%d\n", ret);
	}

	{
		struct ibv_qp_attr attr;

		memset(&attr, 0, sizeof(attr));
		attr.qp_state = IBV_QPS_INIT;
		attr.port_num = 1;
		attr.pkey_index = 0;
		attr.qp_access_flags = access_flags;
		errno = 0;
		ret = ibv_modify_qp(qp, &attr,
				    IBV_QP_STATE | IBV_QP_PKEY_INDEX |
					    IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
		printf("modify INIT ret=%d errno=%d status=%s\n", ret, errno,
		       status_text(ret, errno));
		if (ret)
			goto out_qp;
	}

	if (do_send || do_recv) {
		struct ibv_sge sge;
		struct ibv_recv_wr recv_wr;
		struct ibv_recv_wr *bad_recv = NULL;
		unsigned int i;

		if (posix_memalign(&buf, page_align, buf_len)) {
			ret = ENOMEM;
			goto out_qp;
		}
		if (do_send)
			fill_send_buffer(buf, buf_len);
		else
			memset(buf, 0xcc, buf_len);
		mr = ibv_reg_mr(pd, buf, buf_len, mr_access);
		if (!mr) {
			perror("ibv_reg_mr");
			ret = errno;
			goto out_qp;
		}
		printf("registered mr lkey=0x%x len=%zu iters=%u msg_len=%zu\n",
		       mr->lkey, buf_len, iters, send_len);

		for (i = 0; do_recv && i < iters; i++) {
			memset(&sge, 0, sizeof(sge));
			sge.addr = (uintptr_t)((char *)buf + (size_t)i * send_len);
			sge.length = (uint32_t)send_len;
			sge.lkey = mr->lkey;
			memset(&recv_wr, 0, sizeof(recv_wr));
			recv_wr.wr_id = recv_wr_id_base + i;
			recv_wr.sg_list = &sge;
			recv_wr.num_sge = 1;
			errno = 0;
			ret = ibv_post_recv(qp, &recv_wr, &bad_recv);
			printf("post_recv[%u] ret=%d errno=%d status=%s bad=%p\n",
			       i, ret, errno, status_text(ret, errno),
			       (void *)bad_recv);
			if (ret)
				goto out_qp;
		}
	}

	if (!do_rtr) {
		printf("skipping RTR/RTS; set MAC_TB_RDMA_PROBE_RTR=1 to enable\n");
		goto out_qp;
	}

	{
		struct ibv_qp_attr attr;

		memset(&attr, 0, sizeof(attr));
		attr.qp_state = IBV_QPS_RTR;
		attr.path_mtu = path_mtu;
		attr.rq_psn = remote_psn;
		attr.dest_qp_num = remote_qpn;
		attr.ah_attr.dlid = remote_lid >= 0 ? remote_lid : port_attr.lid;
		attr.ah_attr.sl = 0;
		attr.ah_attr.src_path_bits = 0;
		attr.ah_attr.port_num = 1;
		attr.ah_attr.is_global = 1;
		attr.ah_attr.grh.dgid = remote_gid;
		attr.ah_attr.grh.sgid_index = sgid_index;
		attr.ah_attr.grh.hop_limit = 1;
		errno = 0;
		ret = ibv_modify_qp(qp, &attr,
				    IBV_QP_STATE | IBV_QP_AV |
					    IBV_QP_PATH_MTU |
					    IBV_QP_DEST_QPN | IBV_QP_RQ_PSN);
		printf("modify RTR ret=%d errno=%d status=%s dlid=%u\n", ret,
		       errno, status_text(ret, errno), attr.ah_attr.dlid);
		if (ret)
			goto out_qp;
	}

	{
		struct ibv_qp_attr attr;

		memset(&attr, 0, sizeof(attr));
		attr.qp_state = IBV_QPS_RTS;
		attr.sq_psn = 7;
		errno = 0;
		ret = ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN);
		printf("modify RTS ret=%d errno=%d status=%s\n", ret, errno,
		       status_text(ret, errno));
		if (ret)
			goto out_qp;
	}

	if (do_send) {
		struct ibv_sge sge;
		struct ibv_send_wr send_wr;
		struct ibv_send_wr *bad_send = NULL;
		unsigned int i;

		for (i = 0; i < iters; i++) {
			memset(&sge, 0, sizeof(sge));
			sge.addr = (uintptr_t)((char *)buf + (size_t)i * send_len);
			sge.length = (uint32_t)send_len;
			sge.lkey = mr->lkey;
			memset(&send_wr, 0, sizeof(send_wr));
			send_wr.wr_id = send_wr_id_base + i;
			send_wr.sg_list = &sge;
			send_wr.num_sge = 1;
			send_wr.opcode = IBV_WR_SEND;
			send_wr.send_flags = IBV_SEND_SIGNALED;

			errno = 0;
			ret = ibv_post_send(qp, &send_wr, &bad_send);
			printf("post_send[%u] ret=%d errno=%d status=%s bad=%p\n",
			       i, ret, errno, status_text(ret, errno),
			       (void *)bad_send);
			if (ret)
				goto out_qp;
		}
	}

	if (do_send || do_recv) {
		struct timespec start;
		struct ibv_wc wc;
		unsigned int got_send = 0;
		unsigned int got_recv = 0;

		clock_gettime(CLOCK_MONOTONIC, &start);
		for (;;) {
			struct timespec now;
			long elapsed_ms;
			int n;

			memset(&wc, 0, sizeof(wc));
			n = ibv_poll_cq(cq, 1, &wc);
			if (n < 0) {
				printf("poll_cq ret=%d\n", n);
				ret = EIO;
				goto out_qp;
			}
			if (n > 0) {
				printf("wc wr_id=0x%llx status=%u opcode=%u byte_len=%u\n",
				       (unsigned long long)wc.wr_id, wc.status,
				       wc.opcode, wc.byte_len);
				if (wc.wr_id >= recv_wr_id_base &&
				    wc.wr_id < recv_wr_id_base + iters) {
					unsigned long long idx =
						wc.wr_id - recv_wr_id_base;
					void *recv_buf = (char *)buf + idx * send_len;

					got_recv++;
					printf("recv completion %u/%u idx=%llu\n",
					       got_recv, iters, idx);
					if (idx == 0 || idx + 1 == iters) {
						dump_prefix(recv_buf, send_len);
						if (getenv("MAC_TB_RDMA_PROBE_DUMP_GAPS"))
							dump_gap_windows(recv_buf, send_len);
					}
				}
				if (wc.wr_id >= send_wr_id_base &&
				    wc.wr_id < send_wr_id_base + iters) {
					got_send++;
					printf("send completion %u/%u\n",
					       got_send, iters);
				}
				if (!wait_all ||
				    ((do_send ? got_send >= iters : 1) &&
				     (do_recv ? got_recv >= iters : 1)))
					break;
			}

			clock_gettime(CLOCK_MONOTONIC, &now);
			elapsed_ms = (now.tv_sec - start.tv_sec) * 1000L +
				     (now.tv_nsec - start.tv_nsec) / 1000000L;
			if (elapsed_ms >= poll_ms) {
				printf("poll_cq timeout after %d ms\n", poll_ms);
				break;
			}
			usleep(1000);
		}
	}

out_qp:
	if (sleep_sec > 0) {
		printf("sleeping %d sec before cleanup\n", sleep_sec);
		sleep((unsigned int)sleep_sec);
	}
	ibv_destroy_qp(qp);
	if (mr)
		ibv_dereg_mr(mr);
	free(buf);
out_cq:
	ibv_destroy_cq(cq);
out_pd:
	ibv_dealloc_pd(pd);
out_close:
	ibv_close_device(ctx);
	ibv_free_device_list(list);
	return ret ? 1 : 0;
}
