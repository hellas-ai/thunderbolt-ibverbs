// SPDX-License-Identifier: MIT
/*
 * rc_qpn_churn - local QP identity reuse smoke test.
 *
 * Creates and destroys RC QPs on a usb4_rdma device. A provider that reuses
 * the same native QPN immediately after destroy is unsafe for short-lived RC
 * connections because delayed wire frames can target the next incarnation.
 */

#define _POSIX_C_SOURCE 200112L

#include <errno.h>
#include <infiniband/verbs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct opts {
	const char *dev;
	unsigned int count;
};

static void usage(const char *argv0)
{
	fprintf(stderr, "usage: %s [-d usb4_rdmaN] [-n count]\n", argv0);
}

static int parse_uint(const char *s, unsigned int *out)
{
	char *end = NULL;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 0);
	if (errno || !end || *end || v > 1000000ul)
		return -1;
	*out = (unsigned int)v;
	return 0;
}

static int parse_opts(int argc, char **argv, struct opts *opts)
{
	opts->dev = NULL;
	opts->count = 8;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d") && i + 1 < argc) {
			opts->dev = argv[++i];
		} else if (!strcmp(argv[i], "-n") && i + 1 < argc) {
			if (parse_uint(argv[++i], &opts->count))
				return -1;
		} else if (!strcmp(argv[i], "-h") ||
			   !strcmp(argv[i], "--help")) {
			usage(argv[0]);
			exit(0);
		} else {
			return -1;
		}
	}

	return opts->count ? 0 : -1;
}

static int name_matches(const char *name, const struct opts *opts)
{
	if (!name)
		return 0;
	if (opts->dev)
		return !strcmp(name, opts->dev);
	return !strncmp(name, "usb4_rdma", strlen("usb4_rdma"));
}

static struct ibv_qp *create_qp(struct ibv_pd *pd, struct ibv_cq *cq)
{
	struct ibv_qp_init_attr attr = {};

	attr.send_cq = cq;
	attr.recv_cq = cq;
	attr.qp_type = IBV_QPT_RC;
	attr.cap.max_send_wr = 4;
	attr.cap.max_recv_wr = 4;
	attr.cap.max_send_sge = 1;
	attr.cap.max_recv_sge = 1;

	return ibv_create_qp(pd, &attr);
}

int main(int argc, char **argv)
{
	struct opts opts;
	struct ibv_device **devices;
	struct ibv_context *ctx = NULL;
	struct ibv_pd *pd = NULL;
	struct ibv_cq *cq = NULL;
	int num_devices = 0;
	uint32_t prev_qpn = 0;
	int have_prev = 0;
	int ret = 1;

	if (parse_opts(argc, argv, &opts)) {
		usage(argv[0]);
		return 2;
	}

	devices = ibv_get_device_list(&num_devices);
	if (!devices) {
		fprintf(stderr, "ibv_get_device_list: %s\n", strerror(errno));
		return 1;
	}

	for (int i = 0; i < num_devices; i++) {
		const char *name = ibv_get_device_name(devices[i]);

		if (!name_matches(name, &opts))
			continue;

		ctx = ibv_open_device(devices[i]);
		if (!ctx) {
			fprintf(stderr, "ibv_open_device(%s): %s\n", name,
				strerror(errno));
			goto out;
		}
		printf("device=%s\n", name);
		break;
	}

	if (!ctx) {
		fprintf(stderr, "no matching usb4_rdma device found\n");
		goto out;
	}

	pd = ibv_alloc_pd(ctx);
	if (!pd) {
		fprintf(stderr, "ibv_alloc_pd: %s\n", strerror(errno));
		goto out;
	}

	cq = ibv_create_cq(ctx, 16, NULL, NULL, 0);
	if (!cq) {
		fprintf(stderr, "ibv_create_cq: %s\n", strerror(errno));
		goto out;
	}

	for (unsigned int i = 0; i < opts.count; i++) {
		struct ibv_qp *qp = create_qp(pd, cq);
		uint32_t qpn;

		if (!qp) {
			fprintf(stderr, "ibv_create_qp[%u]: %s\n", i,
				strerror(errno));
			goto out;
		}

		qpn = qp->qp_num;
		printf("qp[%u]=0x%x\n", i, qpn);
		if (have_prev && qpn == prev_qpn) {
			fprintf(stderr, "QPN reused immediately: 0x%x\n", qpn);
			ibv_destroy_qp(qp);
			goto out;
		}
		prev_qpn = qpn;
		have_prev = 1;

		if (ibv_destroy_qp(qp)) {
			fprintf(stderr, "ibv_destroy_qp[%u]: %s\n", i,
				strerror(errno));
			goto out;
		}
	}

	ret = 0;

out:
	if (cq)
		ibv_destroy_cq(cq);
	if (pd)
		ibv_dealloc_pd(pd);
	if (ctx)
		ibv_close_device(ctx);
	ibv_free_device_list(devices);
	return ret;
}
