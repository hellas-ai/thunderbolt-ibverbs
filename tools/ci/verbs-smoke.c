// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include <errno.h>
#include <infiniband/verbs.h>
#include <stdio.h>
#include <string.h>

static int name_matches(const char *name)
{
	return !strncmp(name, "usb4_rdma", strlen("usb4_rdma")) ||
	       !strncmp(name, "usb4_apple", strlen("usb4_apple"));
}

int main(void)
{
	struct ibv_device **devices;
	struct ibv_context *ctx;
	struct ibv_device_attr dev_attr;
	struct ibv_port_attr port_attr;
	struct ibv_pd *pd;
	struct ibv_cq *cq;
	int num_devices = 0;
	int ret = 1;
	int i;

	devices = ibv_get_device_list(&num_devices);
	if (!devices) {
		fprintf(stderr, "ibv_get_device_list: %s\n", strerror(errno));
		return 1;
	}

	for (i = 0; i < num_devices; i++) {
		const char *name = ibv_get_device_name(devices[i]);

		if (!name || !name_matches(name))
			continue;

		printf("found %s\n", name);
		ctx = ibv_open_device(devices[i]);
		if (!ctx) {
			fprintf(stderr, "ibv_open_device(%s): %s\n", name,
				strerror(errno));
			ret = 2;
			goto out;
		}

		memset(&dev_attr, 0, sizeof(dev_attr));
		if (ibv_query_device(ctx, &dev_attr)) {
			fprintf(stderr, "ibv_query_device(%s): %s\n", name,
				strerror(errno));
			ret = 3;
			goto close;
		}

		memset(&port_attr, 0, sizeof(port_attr));
		if (ibv_query_port(ctx, 1, &port_attr)) {
			fprintf(stderr, "ibv_query_port(%s): %s\n", name,
				strerror(errno));
			ret = 4;
			goto close;
		}

		pd = ibv_alloc_pd(ctx);
		if (!pd) {
			fprintf(stderr, "ibv_alloc_pd(%s): %s\n", name,
				strerror(errno));
			ret = 5;
			goto close;
		}

		cq = ibv_create_cq(ctx, 8, NULL, NULL, 0);
		if (!cq) {
			fprintf(stderr, "ibv_create_cq(%s): %s\n", name,
				strerror(errno));
			ret = 6;
			goto dealloc_pd;
		}

		printf("queried %s max_qp=%d max_cq=%d port_state=%d\n",
		       name, dev_attr.max_qp, dev_attr.max_cq,
		       port_attr.state);

		if (ibv_destroy_cq(cq)) {
			fprintf(stderr, "ibv_destroy_cq(%s): %s\n", name,
				strerror(errno));
			ret = 7;
			goto dealloc_pd;
		}
		if (ibv_dealloc_pd(pd)) {
			fprintf(stderr, "ibv_dealloc_pd(%s): %s\n", name,
				strerror(errno));
			ret = 8;
			goto close;
		}
		if (ibv_close_device(ctx)) {
			fprintf(stderr, "ibv_close_device(%s): %s\n", name,
				strerror(errno));
			ret = 9;
			goto out;
		}

		ret = 0;
		goto out;

dealloc_pd:
		ibv_dealloc_pd(pd);
close:
		ibv_close_device(ctx);
		goto out;
	}

	fprintf(stderr, "no usb4_rdma/usb4_apple device found among %d RDMA devices\n",
		num_devices);

out:
	ibv_free_device_list(devices);
	return ret;
}
