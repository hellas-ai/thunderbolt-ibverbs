// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#define _POSIX_C_SOURCE 200112L
/*
 * USB4 RDMA Direct Verbs (DV) QUERY_CAPS probe.
 *
 * Opens a usb4_rdma* / usb4_apple* verbs device, issues the private
 * USB4_RDMA_DV_METHOD_QUERY_CAPS method via the raw RDMA_VERBS_IOCTL ABI,
 * and prints the reported capabilities and queue-memory layout.
 *
 * Uses the raw ioctl rather than rdma-core's private execute_ioctl() helper
 * so the probe can be built as an ordinary test binary outside the provider
 * tree.
 */

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

#include <infiniband/ib_user_ioctl_verbs.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_user_ioctl_cmds.h>

#include "usb4_rdma_dv.h"

static bool device_name_matches(const char *name)
{
	return strncmp(name, "usb4_rdma", strlen("usb4_rdma")) == 0 ||
	       strncmp(name, "usb4_apple", strlen("usb4_apple")) == 0;
}

static struct ibv_device *find_device(struct ibv_device **list, int count,
				      const char *wanted)
{
	struct ibv_device *fallback = NULL;
	int i;

	for (i = 0; i < count; i++) {
		const char *name = ibv_get_device_name(list[i]);

		if (wanted && strcmp(name, wanted) == 0)
			return list[i];
		if (!fallback && device_name_matches(name))
			fallback = list[i];
	}
	return fallback;
}

static int query_caps(struct ibv_context *ctx,
		      struct usb4_rdma_dv_query_caps_resp *resp)
{
	struct {
		struct ib_uverbs_ioctl_hdr hdr;
		struct ib_uverbs_attr attrs[1];
	} cmd = {
		.hdr = {
			.length = sizeof(cmd),
			.object_id = USB4_RDMA_DV_OBJECT_DEVICE,
			.method_id = USB4_RDMA_DV_METHOD_QUERY_CAPS,
			.num_attrs = 1,
			.driver_id = RDMA_DRIVER_UNKNOWN,
		},
		.attrs = {
			{
				.attr_id = USB4_RDMA_DV_ATTR_QUERY_CAPS_RESP,
				.len = sizeof(*resp),
				.flags = UVERBS_ATTR_F_MANDATORY,
				.data = (uintptr_t)resp,
			},
		},
	};

	memset(resp, 0, sizeof(*resp));
	if (ioctl(ctx->cmd_fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return errno;
	return 0;
}

static int create_queue(struct ibv_qp *qp,
			const struct usb4_rdma_dv_queue_create *req,
			struct usb4_rdma_dv_queue_resp *resp)
{
	struct {
		struct ib_uverbs_ioctl_hdr hdr;
		struct ib_uverbs_attr attrs[3];
	} cmd = {
		.hdr = {
			.length = sizeof(cmd),
			.object_id = USB4_RDMA_DV_OBJECT_DEVICE,
			.method_id = USB4_RDMA_DV_METHOD_CREATE_QUEUE,
			.num_attrs = 3,
			.driver_id = RDMA_DRIVER_UNKNOWN,
		},
		.attrs = {
			{
				.attr_id = USB4_RDMA_DV_ATTR_CREATE_QUEUE_QP,
				.flags = UVERBS_ATTR_F_MANDATORY,
				.data = qp->handle,
			},
			{
				.attr_id = USB4_RDMA_DV_ATTR_CREATE_QUEUE_REQ,
				.len = sizeof(*req),
				.flags = UVERBS_ATTR_F_MANDATORY,
				.data = (uintptr_t)req,
			},
			{
				.attr_id = USB4_RDMA_DV_ATTR_CREATE_QUEUE_RESP,
				.len = sizeof(*resp),
				.flags = UVERBS_ATTR_F_MANDATORY,
				.data = (uintptr_t)resp,
			},
		},
	};

	memset(resp, 0, sizeof(*resp));
	if (ioctl(qp->context->cmd_fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return errno;
	return 0;
}

static int destroy_queue(struct ibv_qp *qp)
{
	struct {
		struct ib_uverbs_ioctl_hdr hdr;
		struct ib_uverbs_attr attrs[1];
	} cmd = {
		.hdr = {
			.length = sizeof(cmd),
			.object_id = USB4_RDMA_DV_OBJECT_DEVICE,
			.method_id = USB4_RDMA_DV_METHOD_DESTROY_QUEUE,
			.num_attrs = 1,
			.driver_id = RDMA_DRIVER_UNKNOWN,
		},
		.attrs = {
			{
				.attr_id = USB4_RDMA_DV_ATTR_DESTROY_QUEUE_QP,
				.flags = UVERBS_ATTR_F_MANDATORY,
				.data = qp->handle,
			},
		},
	};

	if (ioctl(qp->context->cmd_fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return errno;
	return 0;
}

static int alloc_aligned(size_t alignment, size_t size, void **ptr)
{
	int ret;

	*ptr = NULL;
	ret = posix_memalign(ptr, alignment, size);
	if (ret)
		return ret;
	memset(*ptr, 0, size);
	return 0;
}

static void print_caps_bitmap(uint32_t caps)
{
	const struct {
		uint32_t bit;
		const char *name;
	} bits[] = {
		{ USB4_RDMA_DV_CAP_SEND, "send" },
		{ USB4_RDMA_DV_CAP_SEND_IMM, "send_imm" },
		{ USB4_RDMA_DV_CAP_WRITE, "write" },
		{ USB4_RDMA_DV_CAP_WRITE_IMM, "write_imm" },
		{ USB4_RDMA_DV_CAP_FENCE, "fence" },
		{ USB4_RDMA_DV_CAP_READ, "read" },
		{ USB4_RDMA_DV_CAP_ATOMIC_FETCH_ADD, "atomic_fetch_add" },
		{ USB4_RDMA_DV_CAP_ATOMIC_SWAP, "atomic_swap" },
		{ USB4_RDMA_DV_CAP_ATOMIC_CMP_SWAP, "atomic_cmp_swap" },
	};
	bool first = true;
	size_t i;

	printf("caps=0x%08" PRIx32, caps);
	if (!caps) {
		printf(" (none)\n");
		return;
	}
	printf(" (");
	for (i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
		if (!(caps & bits[i].bit))
			continue;
		printf("%s%s", first ? "" : "|", bits[i].name);
		first = false;
	}
	printf(")\n");
}

static void print_doorbell_field(const char *name, const char *writer,
				 const char *reader, size_t offset, size_t size)
{
	printf("doorbell_field name=%s offset=%zu size=%zu writer=%s reader=%s\n",
	       name, offset, size, writer, reader);
}

static void print_doorbell_layout(void)
{
	size_t producer_off = offsetof(struct usb4_rdma_dv_doorbell, producer);
	size_t consumer_off = offsetof(struct usb4_rdma_dv_doorbell, consumer);

	printf("doorbell_layout record_size=%zu producer_line_offset=%zu producer_line_size=%zu consumer_line_offset=%zu consumer_line_size=%zu\n",
	       sizeof(struct usb4_rdma_dv_doorbell), producer_off,
	       sizeof(struct usb4_rdma_dv_doorbell_producer_line),
	       consumer_off, sizeof(struct usb4_rdma_dv_doorbell_consumer_line));
	print_doorbell_field(
		"producer.sq_tail", "gpu", "kernel",
		producer_off +
			offsetof(struct usb4_rdma_dv_doorbell_producer_line, sq_tail),
		sizeof(((struct usb4_rdma_dv_doorbell_producer_line *)0)->sq_tail));
	print_doorbell_field(
		"producer.cq_head", "gpu", "kernel",
		producer_off +
			offsetof(struct usb4_rdma_dv_doorbell_producer_line, cq_head),
		sizeof(((struct usb4_rdma_dv_doorbell_producer_line *)0)->cq_head));
	print_doorbell_field(
		"producer.generation", "gpu", "kernel",
		producer_off +
			offsetof(struct usb4_rdma_dv_doorbell_producer_line, generation),
		sizeof(((struct usb4_rdma_dv_doorbell_producer_line *)0)->generation));
	print_doorbell_field(
		"consumer.sq_head", "kernel", "gpu",
		consumer_off +
			offsetof(struct usb4_rdma_dv_doorbell_consumer_line, sq_head),
		sizeof(((struct usb4_rdma_dv_doorbell_consumer_line *)0)->sq_head));
	print_doorbell_field(
		"consumer.cq_tail", "kernel", "gpu",
		consumer_off +
			offsetof(struct usb4_rdma_dv_doorbell_consumer_line, cq_tail),
		sizeof(((struct usb4_rdma_dv_doorbell_consumer_line *)0)->cq_tail));
	print_doorbell_field(
		"consumer.qp_state", "kernel", "gpu",
		consumer_off +
			offsetof(struct usb4_rdma_dv_doorbell_consumer_line, qp_state),
		sizeof(((struct usb4_rdma_dv_doorbell_consumer_line *)0)->qp_state));
	print_doorbell_field(
		"consumer.generation", "kernel", "gpu",
		consumer_off +
			offsetof(struct usb4_rdma_dv_doorbell_consumer_line, generation),
		sizeof(((struct usb4_rdma_dv_doorbell_consumer_line *)0)->generation));
	printf("wqe_field name=generation offset=%zu size=%zu\n",
	       offsetof(struct usb4_rdma_dv_wqe, generation),
	       sizeof(((struct usb4_rdma_dv_wqe *)0)->generation));
}

static int run_queue_test(struct ibv_context *ctx,
			  const struct usb4_rdma_dv_query_caps_resp *caps)
{
	struct usb4_rdma_dv_queue_create req = {};
	struct usb4_rdma_dv_queue_resp resp = {};
	struct ibv_qp_init_attr qp_attr = {};
	struct ibv_mr *doorbell_mr = NULL;
	struct ibv_mr *cq_mr = NULL;
	struct ibv_mr *sq_mr = NULL;
	struct ibv_qp *qp = NULL;
	struct ibv_cq *cq = NULL;
	struct ibv_pd *pd = NULL;
	size_t doorbell_bytes = caps->doorbell_page_size;
	size_t cq_bytes = (size_t)caps->default_cq_entries * caps->cqe_size;
	size_t sq_bytes = (size_t)caps->default_sq_entries * caps->wqe_size;
	void *doorbell = NULL;
	void *cq_buf = NULL;
	void *sq_buf = NULL;
	int access = IBV_ACCESS_LOCAL_WRITE;
	int ret = 1;
	int err;

	if (caps->abi_version != USB4_RDMA_DV_ABI_VERSION) {
		fprintf(stderr, "queue test skipped: unsupported ABI %u\n",
			caps->abi_version);
		return 2;
	}

	err = alloc_aligned(caps->wqe_size, sq_bytes, &sq_buf);
	if (err) {
		fprintf(stderr, "alloc SQ: %s\n", strerror(err));
		goto out;
	}
	err = alloc_aligned(caps->cqe_size, cq_bytes, &cq_buf);
	if (err) {
		fprintf(stderr, "alloc CQ: %s\n", strerror(err));
		goto out;
	}
	err = alloc_aligned(caps->doorbell_page_size, doorbell_bytes, &doorbell);
	if (err) {
		fprintf(stderr, "alloc doorbell: %s\n", strerror(err));
		goto out;
	}

	pd = ibv_alloc_pd(ctx);
	if (!pd) {
		fprintf(stderr, "ibv_alloc_pd: %s\n", strerror(errno));
		goto out;
	}
	sq_mr = ibv_reg_mr(pd, sq_buf, sq_bytes, access);
	if (!sq_mr) {
		fprintf(stderr, "ibv_reg_mr(SQ): %s\n", strerror(errno));
		goto out;
	}
	cq_mr = ibv_reg_mr(pd, cq_buf, cq_bytes, access);
	if (!cq_mr) {
		fprintf(stderr, "ibv_reg_mr(CQ): %s\n", strerror(errno));
		goto out;
	}
	doorbell_mr = ibv_reg_mr(pd, doorbell, doorbell_bytes, access);
	if (!doorbell_mr) {
		fprintf(stderr, "ibv_reg_mr(doorbell): %s\n", strerror(errno));
		goto out;
	}

	cq = ibv_create_cq(ctx, caps->default_cq_entries, NULL, NULL, 0);
	if (!cq) {
		fprintf(stderr, "ibv_create_cq: %s\n", strerror(errno));
		goto out;
	}

	qp_attr.send_cq = cq;
	qp_attr.recv_cq = cq;
	qp_attr.qp_type = IBV_QPT_RC;
	qp_attr.cap.max_send_wr = caps->default_sq_entries;
	qp_attr.cap.max_recv_wr = 1;
	qp_attr.cap.max_send_sge = 1;
	qp_attr.cap.max_recv_sge = 1;
	qp = ibv_create_qp(pd, &qp_attr);
	if (!qp) {
		fprintf(stderr, "ibv_create_qp: %s\n", strerror(errno));
		goto out;
	}

	req.abi_version = USB4_RDMA_DV_ABI_VERSION;
	req.sq_addr = (uintptr_t)sq_buf;
	req.cq_addr = (uintptr_t)cq_buf;
	req.doorbell_addr = (uintptr_t)doorbell;
	req.sq_entries = caps->default_sq_entries;
	req.cq_entries = caps->default_cq_entries;
	req.sq_stride = caps->wqe_size;
	req.cq_stride = caps->cqe_size;

	err = create_queue(qp, &req, &resp);
	if (err) {
		fprintf(stderr, "CREATE_QUEUE failed: %s (%d)\n",
			strerror(err), err);
		goto out;
	}

	printf("create_queue qp_num=%u generation=%u sq_entries=%u cq_entries=%u\n",
	       resp.qp_num, resp.generation, req.sq_entries, req.cq_entries);

	if (resp.qp_num != qp->qp_num) {
		fprintf(stderr,
			"CREATE_QUEUE returned qp_num=%u (expected %u)\n",
			resp.qp_num, qp->qp_num);
		(void)destroy_queue(qp);
		goto out;
	}
	if (!resp.generation) {
		fprintf(stderr, "CREATE_QUEUE returned generation=0 (must be nonzero)\n");
		(void)destroy_queue(qp);
		goto out;
	}

	{
		/*
		 * The kernel publishes the LIVE state into the consumer line
		 * of the doorbell. Verify the GPU side would read what the
		 * contract promises: packed (index=0, generation=X) tails
		 * and qp_state=LIVE. Without a fence the test buffer is
		 * already coherent on the CPU side after the ioctl returns.
		 */
		const struct usb4_rdma_dv_doorbell *db = doorbell;

		printf("doorbell_after_create sq_head=0x%08x cq_tail=0x%08x qp_state=%u generation=%u\n",
		       db->consumer.sq_head, db->consumer.cq_tail,
		       db->consumer.qp_state, db->consumer.generation);

		if (db->consumer.generation != resp.generation ||
		    db->consumer.qp_state != USB4_RDMA_DV_QP_LIVE ||
		    usb4_rdma_dv_tail_generation(db->consumer.sq_head) != resp.generation ||
		    usb4_rdma_dv_tail_generation(db->consumer.cq_tail) != resp.generation ||
		    usb4_rdma_dv_tail_index(db->consumer.sq_head) != 0 ||
		    usb4_rdma_dv_tail_index(db->consumer.cq_tail) != 0) {
			fprintf(stderr,
				"doorbell consumer line did not match LIVE state\n");
			(void)destroy_queue(qp);
			goto out;
		}
	}

	/* Second CREATE_QUEUE must be rejected with -EBUSY while the
	 * first is still attached. */
	err = create_queue(qp, &req, &resp);
	if (err != EBUSY) {
		fprintf(stderr,
			"second CREATE_QUEUE should have failed with EBUSY, got %s (%d)\n",
			err ? strerror(err) : "success", err);
		(void)destroy_queue(qp);
		goto out;
	}
	printf("create_queue_again rejected ebusy=ok\n");

	err = destroy_queue(qp);
	if (err) {
		fprintf(stderr, "DESTROY_QUEUE failed: %s (%d)\n",
			strerror(err), err);
		goto out;
	}
	{
		const struct usb4_rdma_dv_doorbell *db = doorbell;

		printf("doorbell_after_destroy qp_state=%u generation=%u\n",
		       db->consumer.qp_state, db->consumer.generation);
		if (db->consumer.qp_state != USB4_RDMA_DV_QP_DEAD ||
		    db->consumer.generation == resp.generation ||
		    !db->consumer.generation) {
			fprintf(stderr,
				"doorbell consumer line did not reach DEAD with bumped generation\n");
			goto out;
		}
	}
	printf("destroy_queue ok\n");

	/* DESTROY_QUEUE on an already-destroyed queue must return ENOENT. */
	err = destroy_queue(qp);
	if (err != ENOENT) {
		fprintf(stderr,
			"second DESTROY_QUEUE should have failed with ENOENT, got %s (%d)\n",
			err ? strerror(err) : "success", err);
		goto out;
	}
	printf("destroy_queue_again rejected enoent=ok\n");

	ret = 0;
out:
	if (qp && ibv_destroy_qp(qp))
		fprintf(stderr, "ibv_destroy_qp: %s\n", strerror(errno));
	if (cq && ibv_destroy_cq(cq))
		fprintf(stderr, "ibv_destroy_cq: %s\n", strerror(errno));
	if (doorbell_mr && ibv_dereg_mr(doorbell_mr))
		fprintf(stderr, "ibv_dereg_mr(doorbell): %s\n", strerror(errno));
	if (cq_mr && ibv_dereg_mr(cq_mr))
		fprintf(stderr, "ibv_dereg_mr(CQ): %s\n", strerror(errno));
	if (sq_mr && ibv_dereg_mr(sq_mr))
		fprintf(stderr, "ibv_dereg_mr(SQ): %s\n", strerror(errno));
	if (pd && ibv_dealloc_pd(pd))
		fprintf(stderr, "ibv_dealloc_pd: %s\n", strerror(errno));
	free(doorbell);
	free(cq_buf);
	free(sq_buf);
	return ret;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [-d <device>] [-l] [-q]\n"
		"\n"
		"Probe USB4 RDMA Direct Verbs QUERY_CAPS on a usb4_rdma* device.\n"
		"\n"
		"Options:\n"
		"  -d <device>   Match device by name (default: first usb4_rdma* / usb4_apple*)\n"
		"  -l            Print doorbell/WQE layout only; do not open a device\n"
		"  -q            After QUERY_CAPS, exercise CREATE_QUEUE/DESTROY_QUEUE on a fresh QP\n"
		"  -h            Print this help\n",
		argv0);
}

int main(int argc, char **argv)
{
	struct usb4_rdma_dv_query_caps_resp resp;
	struct ibv_device **list;
	struct ibv_device *dev;
	struct ibv_context *ctx;
	const char *wanted = NULL;
	bool layout_only = false;
	bool queue_test = false;
	int num_devices = 0;
	int opt;
	int err;

	while ((opt = getopt(argc, argv, "d:lqh")) != -1) {
		switch (opt) {
		case 'd':
			wanted = optarg;
			break;
		case 'l':
			layout_only = true;
			break;
		case 'q':
			queue_test = true;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 2;
		}
	}

	if (layout_only) {
		print_doorbell_layout();
		return 0;
	}

	list = ibv_get_device_list(&num_devices);
	if (!list) {
		fprintf(stderr, "ibv_get_device_list failed: %s\n",
			strerror(errno));
		return 1;
	}

	dev = find_device(list, num_devices, wanted);
	if (!dev) {
		fprintf(stderr,
			"no matching usb4_rdma/usb4_apple device found%s%s\n",
			wanted ? " for name=" : "", wanted ? wanted : "");
		ibv_free_device_list(list);
		return 1;
	}

	ctx = ibv_open_device(dev);
	if (!ctx) {
		fprintf(stderr, "ibv_open_device(%s) failed: %s\n",
			ibv_get_device_name(dev), strerror(errno));
		ibv_free_device_list(list);
		return 1;
	}

	err = query_caps(ctx, &resp);
	if (err) {
		fprintf(stderr, "QUERY_CAPS ioctl failed on %s: %s\n",
			ibv_get_device_name(dev), strerror(err));
		ibv_close_device(ctx);
		ibv_free_device_list(list);
		return 1;
	}

	printf("device=%s\n", ibv_get_device_name(dev));
	printf("abi_version=%" PRIu32 "\n", resp.abi_version);
	print_caps_bitmap(resp.caps);
	printf("max_sq_entries=%" PRIu32 " default_sq_entries=%" PRIu32 "\n",
	       resp.max_sq_entries, resp.default_sq_entries);
	printf("max_cq_entries=%" PRIu32 " default_cq_entries=%" PRIu32 "\n",
	       resp.max_cq_entries, resp.default_cq_entries);
	printf("wqe_size=%" PRIu32 " cqe_size=%" PRIu32 "\n",
	       resp.wqe_size, resp.cqe_size);
	printf("doorbell_record_size=%" PRIu32 " doorbell_page_size=%" PRIu32 "\n",
	       resp.doorbell_record_size, resp.doorbell_page_size);
	printf("tail_index_bits=%" PRIu32 " tail_generation_bits=%" PRIu32 "\n",
	       resp.tail_index_bits, resp.tail_generation_bits);
	print_doorbell_layout();

	if (queue_test) {
		int rc = run_queue_test(ctx, &resp);

		ibv_close_device(ctx);
		ibv_free_device_list(list);
		return rc;
	}

	ibv_close_device(ctx);
	ibv_free_device_list(list);
	return 0;
}
