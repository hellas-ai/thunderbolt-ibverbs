/*
 * u4_pingpong — minimal RC SEND/RECV ping-pong benchmark over usb4_rdma.
 *
 * Two-peer setup, both running this binary. Server side: post_recv
 * + reply with post_send. Client side: post_send + wait for response.
 * Repeat N times, report per-message latency.
 *
 * Doesn't use TCP for QP info exchange. Both peers assume they are using
 * the first RC QP allocated after module load, which is normally QPN 2.
 * The driver currently ignores the destination GID for single-peer data
 * routing, but the verbs core still requires a global AH for RoCEv2.
 *
 * Usage:
 *   u4_pingpong server <iters> <size>
 *   u4_pingpong client <iters> <size>
 *
 * Both sides must be launched before traffic begins.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <infiniband/verbs.h>

static struct ibv_context *open_dev(void)
{
	int n, i;
	struct ibv_device **list = ibv_get_device_list(&n);
	struct ibv_context *ctx = NULL;
	for (i = 0; i < n; i++)
		if (strcmp(ibv_get_device_name(list[i]), "usb4_rdma0") == 0) {
			ctx = ibv_open_device(list[i]); break;
		}
	if (list) ibv_free_device_list(list);
	return ctx;
}

static int qp_to_rts(struct ibv_context *ctx, struct ibv_qp *qp)
{
	struct ibv_qp_attr m;
	union ibv_gid gid;
	int rv;

	rv = ibv_query_gid(ctx, 1, 3, &gid);
	if (rv)
		return rv;

	memset(&m, 0, sizeof(m));
	m.qp_state=IBV_QPS_INIT; m.pkey_index=0; m.port_num=1;
	m.qp_access_flags=IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_WRITE;
	rv = ibv_modify_qp(qp, &m, IBV_QP_STATE|IBV_QP_PKEY_INDEX|IBV_QP_PORT|IBV_QP_ACCESS_FLAGS);
	if (rv) return rv;

	memset(&m, 0, sizeof(m));
	m.qp_state=IBV_QPS_RTR; m.path_mtu=IBV_MTU_4096;
	m.dest_qp_num = qp->qp_num; m.rq_psn=0;
	m.max_dest_rd_atomic=0; m.min_rnr_timer=12;
	m.ah_attr.is_global=1; m.ah_attr.port_num=1;
	m.ah_attr.grh.dgid = gid;
	m.ah_attr.grh.sgid_index = 3;
	m.ah_attr.grh.hop_limit = 1;
	rv = ibv_modify_qp(qp, &m, IBV_QP_STATE|IBV_QP_AV|IBV_QP_PATH_MTU|
				   IBV_QP_DEST_QPN|IBV_QP_RQ_PSN|
				   IBV_QP_MAX_DEST_RD_ATOMIC|IBV_QP_MIN_RNR_TIMER);
	if (rv) return rv;

	memset(&m, 0, sizeof(m));
	m.qp_state=IBV_QPS_RTS; m.timeout=14; m.retry_cnt=7;
	m.rnr_retry=7; m.sq_psn=0; m.max_rd_atomic=0;
	return ibv_modify_qp(qp, &m, IBV_QP_STATE|IBV_QP_TIMEOUT|
				     IBV_QP_RETRY_CNT|IBV_QP_RNR_RETRY|
				     IBV_QP_SQ_PSN|IBV_QP_MAX_QP_RD_ATOMIC);
}

static long ns_diff(const struct timespec *a, const struct timespec *b)
{
	return (b->tv_sec - a->tv_sec) * 1000000000L +
	       (b->tv_nsec - a->tv_nsec);
}

static int cmp_long(const void *a, const void *b)
{
	long da = *(const long *)a, db = *(const long *)b;
	return (da > db) - (da < db);
}

int main(int argc, char **argv) {
	if (argc < 4) {
		fprintf(stderr, "usage: %s {server|client} <iters> <size>\n", argv[0]);
		return 1;
	}
	int is_client = strcmp(argv[1], "client") == 0;
	int iters = atoi(argv[2]);
	size_t size = atol(argv[3]);
	if (size > 1u << 24) { fprintf(stderr, "max size 16 MiB\n"); return 1; }

	struct ibv_context *ctx = open_dev();
	if (!ctx) { fprintf(stderr, "no usb4_rdma0\n"); return 1; }

	struct ibv_pd *pd = ibv_alloc_pd(ctx);
	/* POSTED recv slots + 1 send slot at offset send_off, all size bytes. */
	#define POSTED 32
	size_t send_off = ((POSTED * size + 4095) & ~4095UL);
	size_t bufsz = ((send_off + size + 4095) & ~4095UL);
	if (bufsz < 65536) bufsz = 65536;
	char *buf = aligned_alloc(4096, bufsz);
	memset(buf, 0xa5, bufsz);
	struct ibv_mr *mr = ibv_reg_mr(pd, buf, bufsz,
				       IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_WRITE);
	struct ibv_cq *cq = ibv_create_cq(ctx, 64, NULL, NULL, 0);
	struct ibv_qp_init_attr qa = {
		.send_cq=cq, .recv_cq=cq,
		.cap = {.max_send_wr=64, .max_recv_wr=64, .max_send_sge=1, .max_recv_sge=1 },
		.qp_type=IBV_QPT_RC,
	};
	struct ibv_qp *qp = ibv_create_qp(pd, &qa);
	int rv = qp_to_rts(ctx, qp);
	if (rv) { fprintf(stderr, "qp_to_rts: %d (%s)\n", rv, strerror(rv)); return 2; }

	/* Pre-post receive buffers — one per outstanding ping. */
	struct ibv_sge rsge = { .addr=(uintptr_t)buf, .length=size, .lkey=mr->lkey };
	struct ibv_recv_wr rwr_template = { .sg_list=&rsge, .num_sge=1 };
	for (int i = 0; i < POSTED; i++) {
		struct ibv_recv_wr rwr = rwr_template;
		struct ibv_recv_wr *bad;
		rwr.wr_id = 0x1000 + i;
		rsge.addr = (uintptr_t)buf + i * size;
		rwr.sg_list = &rsge;
		ibv_post_recv(qp, &rwr, &bad);
	}

	long *latencies = is_client ? calloc(iters, sizeof(*latencies)) : NULL;

	struct ibv_send_wr swr;
	struct ibv_send_wr *bad_swr;
	struct ibv_sge ssge;
	struct ibv_wc wcs[16];

	if (is_client) {
		printf("client: %d iters of %zu-byte SEND\n", iters, size);
		struct timespec t0, t1;
		for (int i = 0; i < iters; i++) {
			clock_gettime(CLOCK_MONOTONIC, &t0);
			memset(&swr, 0, sizeof(swr));
			ssge = (struct ibv_sge){
				.addr=(uintptr_t)buf + send_off, .length=size, .lkey=mr->lkey };
			swr.sg_list=&ssge; swr.num_sge=1;
			swr.opcode=IBV_WR_SEND; swr.send_flags=IBV_SEND_SIGNALED;
			swr.wr_id = i;
			rv = ibv_post_send(qp, &swr, &bad_swr);
			if (rv) { fprintf(stderr, "post_send i=%d: %d\n", i, rv); break; }

			/* Wait for SEND completion AND response RECV. */
			int got_send = 0, got_recv = 0;
			while (!got_send || !got_recv) {
				int n = ibv_poll_cq(cq, 16, wcs);
				for (int j = 0; j < n; j++) {
					if (wcs[j].opcode == IBV_WC_SEND) got_send = 1;
					if (wcs[j].opcode == IBV_WC_RECV) {
						got_recv = 1;
						/* Repost the recv slot. */
						struct ibv_recv_wr rwr = rwr_template;
						struct ibv_recv_wr *bad;
						rwr.wr_id = wcs[j].wr_id;
						rsge.addr = (uintptr_t)buf
							+ ((wcs[j].wr_id - 0x1000) % POSTED) * size;
						rwr.sg_list = &rsge;
						ibv_post_recv(qp, &rwr, &bad);
					}
				}
			}
			clock_gettime(CLOCK_MONOTONIC, &t1);
			latencies[i] = ns_diff(&t0, &t1);
		}
		qsort(latencies, iters, sizeof(*latencies), cmp_long);
		long total = 0;
		for (int i = 0; i < iters; i++) total += latencies[i];
		printf("size=%zu iters=%d  min=%.2f us  p50=%.2f us  p99=%.2f us  avg=%.2f us  max=%.2f us\n",
		       size, iters,
		       latencies[0] / 1000.0,
		       latencies[iters/2] / 1000.0,
		       latencies[(iters * 99) / 100] / 1000.0,
		       (total / (double)iters) / 1000.0,
		       latencies[iters-1] / 1000.0);
	} else {
		printf("server: replying to %d pings of %zu bytes\n", iters, size);
		for (int i = 0; i < iters; i++) {
			/* Wait for ping recv. */
			int got_recv = 0;
			while (!got_recv) {
				int n = ibv_poll_cq(cq, 16, wcs);
				for (int j = 0; j < n; j++) {
					if (wcs[j].opcode == IBV_WC_RECV) got_recv = 1;
				}
			}
			/* Echo back. */
			memset(&swr, 0, sizeof(swr));
			ssge = (struct ibv_sge){
				.addr=(uintptr_t)buf + send_off, .length=size, .lkey=mr->lkey };
			swr.sg_list=&ssge; swr.num_sge=1;
			swr.opcode=IBV_WR_SEND; swr.send_flags=IBV_SEND_SIGNALED;
			swr.wr_id = 0x2000 + i;
			ibv_post_send(qp, &swr, &bad_swr);
			/* Drain SEND completion; repost recv. */
			int got_send = 0;
			while (!got_send) {
				int n = ibv_poll_cq(cq, 16, wcs);
				for (int j = 0; j < n; j++)
					if (wcs[j].opcode == IBV_WC_SEND) got_send = 1;
			}
			struct ibv_recv_wr rwr = rwr_template;
			struct ibv_recv_wr *bad;
			rwr.wr_id = 0x1000 + (i % POSTED);
			rsge.addr = (uintptr_t)buf + (i % POSTED) * size;
			rwr.sg_list = &rsge;
			ibv_post_recv(qp, &rwr, &bad);
		}
		printf("server: done\n");
	}

	ibv_destroy_qp(qp); ibv_destroy_cq(cq);
	ibv_dereg_mr(mr); ibv_dealloc_pd(pd); ibv_close_device(ctx);
	free(buf);
	if (latencies) free(latencies);
	return 0;
}
