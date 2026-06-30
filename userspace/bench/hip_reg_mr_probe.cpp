// SPDX-License-Identifier: MIT
/*
 * Probe whether libibverbs can register ROCm allocation types with usb4_rdma.
 */

#include <errno.h>
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>
#include <hip/hip_runtime.h>
#include <infiniband/verbs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct ibv_context *open_context(const char *name)
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

int main(int argc, char **argv)
{
	const char *dev = "usb4_rdma0";
	const char *kind = argc > 1 ? argv[1] : "device";
	size_t size = argc > 2 ? strtoull(argv[2], NULL, 0) : 4096;
	bool gpu_touch = false;
	bool use_dmabuf = false;
	unsigned int host_flags = hipHostMallocMapped;
	struct ibv_context *ctx;
	struct ibv_pd *pd;
	struct ibv_mr *mr;
	void *ptr = NULL;
	hipError_t hret;
	int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
		     IBV_ACCESS_REMOTE_READ;
	int ret = 1;

	for (int i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "gpu-touch")) {
			gpu_touch = true;
		} else if (!strcmp(argv[i], "dmabuf")) {
			use_dmabuf = true;
		} else {
			fprintf(stderr,
				"usage: %s device|managed|host|host-coherent|host-noncoherent|host-uncached [bytes] [gpu-touch] [dmabuf]\n",
				argv[0]);
			return 2;
		}
	}

	hret = hipSetDevice(0);
	if (hret != hipSuccess) {
		fprintf(stderr, "hipSetDevice failed: %s\n",
			hipGetErrorString(hret));
		return 1;
	}

	if (!strcmp(kind, "device")) {
		hret = hipMalloc(&ptr, size);
	} else if (!strcmp(kind, "managed")) {
		hret = hipMallocManaged(&ptr, size);
	} else if (!strcmp(kind, "host")) {
		hret = hipHostMalloc(&ptr, size, host_flags);
	} else if (!strcmp(kind, "host-coherent")) {
		host_flags |= hipHostMallocCoherent;
		hret = hipHostMalloc(&ptr, size, host_flags);
	} else if (!strcmp(kind, "host-noncoherent")) {
		host_flags |= hipHostMallocNonCoherent;
		hret = hipHostMalloc(&ptr, size, host_flags);
	} else if (!strcmp(kind, "host-uncached")) {
		host_flags |= hipHostMallocUncached;
		hret = hipHostMalloc(&ptr, size, host_flags);
	} else {
		fprintf(stderr,
			"usage: %s device|managed|host|host-coherent|host-noncoherent|host-uncached [bytes] [gpu-touch] [dmabuf]\n",
			argv[0]);
		return 2;
	}
	if (hret != hipSuccess) {
		fprintf(stderr, "hip allocation kind=%s failed: %s\n", kind,
			hipGetErrorString(hret));
		return 1;
	}
	if (gpu_touch) {
		hret = hipMemset(ptr, 0x5a, size);
		if (hret == hipSuccess)
			hret = hipDeviceSynchronize();
		if (hret != hipSuccess) {
			fprintf(stderr, "gpu-touch kind=%s failed: %s\n", kind,
				hipGetErrorString(hret));
			goto out_free;
		}
	}

	ctx = open_context(dev);
	if (!ctx) {
		fprintf(stderr, "open_context(%s) failed\n", dev);
		goto out_free;
	}
	pd = ibv_alloc_pd(ctx);
	if (!pd) {
		perror("ibv_alloc_pd");
		goto out_close;
	}

	errno = 0;
	if (use_dmabuf) {
		uint64_t offset = 0;
		int fd = -1;
		hsa_status_t hsa_ret;

		hsa_ret = hsa_amd_portable_export_dmabuf(ptr, size, &fd,
							 &offset);
		if (hsa_ret != HSA_STATUS_SUCCESS) {
			printf("kind=%s method=dmabuf gpu_touch=%d ptr=%p size=%zu export=FAIL hsa_status=%d\n",
			       kind, gpu_touch, ptr, size, hsa_ret);
			goto out_pd;
		}

		mr = ibv_reg_dmabuf_mr(pd, offset, size, (uint64_t)ptr, fd,
				       access);
		if (!mr) {
			printf("kind=%s method=dmabuf gpu_touch=%d ptr=%p size=%zu fd=%d offset=%llu reg_mr=FAIL errno=%d %s\n",
			       kind, gpu_touch, ptr, size, fd,
			       (unsigned long long)offset, errno,
			       strerror(errno));
			(void)hsa_amd_portable_close_dmabuf(fd);
			goto out_pd;
		}

		printf("kind=%s method=dmabuf gpu_touch=%d ptr=%p size=%zu fd=%d offset=%llu reg_mr=OK lkey=0x%x rkey=0x%x\n",
		       kind, gpu_touch, ptr, size, fd,
		       (unsigned long long)offset, mr->lkey, mr->rkey);
		ibv_dereg_mr(mr);
		(void)hsa_amd_portable_close_dmabuf(fd);
		ret = 0;
		goto out_pd;
	}

	mr = ibv_reg_mr(pd, ptr, size, access);
	if (!mr) {
		printf("kind=%s method=reg_mr gpu_touch=%d ptr=%p size=%zu reg_mr=FAIL errno=%d %s\n",
		       kind, gpu_touch, ptr, size, errno, strerror(errno));
	} else {
		printf("kind=%s method=reg_mr gpu_touch=%d ptr=%p size=%zu reg_mr=OK lkey=0x%x rkey=0x%x\n",
		       kind, gpu_touch, ptr, size, mr->lkey, mr->rkey);
		ibv_dereg_mr(mr);
		ret = 0;
	}

out_pd:
	ibv_dealloc_pd(pd);
out_close:
	ibv_close_device(ctx);
out_free:
	if (!strncmp(kind, "host", 4))
		(void)hipHostFree(ptr);
	else
		(void)hipFree(ptr);
	return ret;
}
