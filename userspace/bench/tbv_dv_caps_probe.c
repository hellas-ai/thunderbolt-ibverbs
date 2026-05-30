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

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [-d <device>] [-l]\n"
		"\n"
		"Probe USB4 RDMA Direct Verbs QUERY_CAPS on a usb4_rdma* device.\n"
		"\n"
		"Options:\n"
		"  -d <device>   Match device by name (default: first usb4_rdma* / usb4_apple*)\n"
		"  -l            Print doorbell/WQE layout only; do not open a device\n"
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
	int num_devices = 0;
	int opt;
	int err;

	while ((opt = getopt(argc, argv, "d:lh")) != -1) {
		switch (opt) {
		case 'd':
			wanted = optarg;
			break;
		case 'l':
			layout_only = true;
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

	ibv_close_device(ctx);
	ibv_free_device_list(list);
	return 0;
}
