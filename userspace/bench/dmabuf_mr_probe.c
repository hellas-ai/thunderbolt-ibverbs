// SPDX-License-Identifier: MIT

#include <errno.h>
#include <infiniband/verbs.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *errno_name(int err)
{
	switch (err) {
	case 0:
		return "OK";
	case EBADF:
		return "EBADF";
	case EINVAL:
		return "EINVAL";
	case ENODEV:
		return "ENODEV";
	case ENOMEM:
		return "ENOMEM";
	case EOPNOTSUPP:
		return "EOPNOTSUPP";
#ifdef EPROTONOSUPPORT
	case EPROTONOSUPPORT:
		return "EPROTONOSUPPORT";
#endif
	default:
		return "UNKNOWN";
	}
}

static int parse_errno(const char *value)
{
	char *end = NULL;
	long parsed;

	if (!strcmp(value, "OK"))
		return 0;
	if (!strcmp(value, "EBADF"))
		return EBADF;
	if (!strcmp(value, "EINVAL"))
		return EINVAL;
	if (!strcmp(value, "ENODEV"))
		return ENODEV;
	if (!strcmp(value, "ENOMEM"))
		return ENOMEM;
	if (!strcmp(value, "EOPNOTSUPP"))
		return EOPNOTSUPP;
#ifdef EPROTONOSUPPORT
	if (!strcmp(value, "EPROTONOSUPPORT"))
		return EPROTONOSUPPORT;
#endif

	errno = 0;
	parsed = strtol(value, &end, 0);
	if (!errno && end && *end == '\0' && parsed >= 0 && parsed <= 4095)
		return (int)parsed;

	return -1;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [--device HCA] [--fd FD] [--length BYTES] [--iova ADDR] [--expect-errno NAME|NUM]\n",
		argv0);
}

int main(int argc, char **argv)
{
	const char *want_device = NULL;
	const char *expect_value = NULL;
	struct ibv_device **dev_list;
	struct ibv_device *dev = NULL;
	struct ibv_context *ctx = NULL;
	struct ibv_pd *pd = NULL;
	struct ibv_mr *mr = NULL;
	uint64_t iova = 0x100000000ULL;
	size_t length = 4096;
	int expect_errno = -1;
	int num_devices = 0;
	int fd = -1;
	int access;
	int err = 0;
	int ret = 1;
	int i;

	for (i = 1; i < argc; i++) {
		char *end = NULL;

		if (!strcmp(argv[i], "--device") && i + 1 < argc) {
			want_device = argv[++i];
		} else if (!strcmp(argv[i], "--fd") && i + 1 < argc) {
			fd = (int)strtol(argv[++i], &end, 0);
			if (!end || *end) {
				usage(argv[0]);
				return 2;
			}
		} else if (!strcmp(argv[i], "--length") && i + 1 < argc) {
			unsigned long long value;

			value = strtoull(argv[++i], &end, 0);
			if (!end || *end) {
				usage(argv[0]);
				return 2;
			}
			length = (size_t)value;
		} else if (!strcmp(argv[i], "--iova") && i + 1 < argc) {
			iova = strtoull(argv[++i], &end, 0);
			if (!end || *end) {
				usage(argv[0]);
				return 2;
			}
		} else if (!strcmp(argv[i], "--expect-errno") && i + 1 < argc) {
			expect_value = argv[++i];
			expect_errno = parse_errno(expect_value);
			if (expect_errno < 0) {
				fprintf(stderr, "unknown errno expectation: %s\n",
					expect_value);
				return 2;
			}
		} else {
			usage(argv[0]);
			return 2;
		}
	}

	dev_list = ibv_get_device_list(&num_devices);
	if (!dev_list) {
		err = errno;
		fprintf(stderr, "ibv_get_device_list failed: errno=%d %s %s\n",
			err, errno_name(err), strerror(err));
		return 1;
	}

	for (i = 0; i < num_devices; i++) {
		const char *name = ibv_get_device_name(dev_list[i]);

		if ((want_device && !strcmp(name, want_device)) ||
		    (!want_device && !strncmp(name, "usb4_rdma", 9))) {
			dev = dev_list[i];
			break;
		}
	}
	if (!dev) {
		fprintf(stderr, "no matching RDMA device found");
		if (want_device)
			fprintf(stderr, ": %s", want_device);
		fprintf(stderr, "\n");
		goto out_free_list;
	}

	ctx = ibv_open_device(dev);
	if (!ctx) {
		err = errno;
		fprintf(stderr, "ibv_open_device(%s) failed: errno=%d %s %s\n",
			ibv_get_device_name(dev), err, errno_name(err),
			strerror(err));
		goto out_free_list;
	}

	pd = ibv_alloc_pd(ctx);
	if (!pd) {
		err = errno;
		fprintf(stderr, "ibv_alloc_pd(%s) failed: errno=%d %s %s\n",
			ibv_get_device_name(dev), err, errno_name(err),
			strerror(err));
		goto out_close;
	}

	access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
		 IBV_ACCESS_REMOTE_WRITE;
	errno = 0;
	mr = ibv_reg_dmabuf_mr(pd, 0, length, iova, fd, access);
	if (mr) {
		printf("device=%s fd=%d length=%zu iova=0x%" PRIx64 " result=success lkey=0x%x rkey=0x%x\n",
		       ibv_get_device_name(dev), fd, length, iova, mr->lkey,
		       mr->rkey);
		err = 0;
		ibv_dereg_mr(mr);
	} else {
		err = errno;
		printf("device=%s fd=%d length=%zu iova=0x%" PRIx64 " result=error errno=%d name=%s message=%s\n",
		       ibv_get_device_name(dev), fd, length, iova, err,
		       errno_name(err), strerror(err));
	}

	if (expect_errno >= 0)
		ret = err == expect_errno ? 0 : 1;
	else
		ret = err ? 1 : 0;

	ibv_dealloc_pd(pd);
out_close:
	ibv_close_device(ctx);
out_free_list:
	ibv_free_device_list(dev_list);
	return ret;
}
