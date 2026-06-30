// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Probe the kernel->GPU direct-load visibility needed by USB4 GDA sync words.
 *
 * This intentionally tests the exact awkward direction that ordinary userspace
 * CPU/GPU coherency probes miss:
 *
 *   kernel writes a DV completion tail word in userspace queue memory
 *   GPU kernel spins on that word using a plain direct load
 *
 * If a host-coherent allocation passes this probe, it is a viable candidate
 * for rocSHMEM pSync/signal control memory. Payload memory can still be a
 * separate dmabuf/device allocation.
 */

#define _POSIX_C_SOURCE 200112L

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>

#include <hip/hip_runtime.h>
#include <infiniband/ib_user_ioctl_verbs.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_user_ioctl_cmds.h>

#include "usb4_rdma_dv.h"

#define PROBE_WR_ID_BASE 0x4b43514550524f42ull /* KCQEPROB */

struct alignas(64) ProbeState {
	uint32_t gpu_seen;
	uint32_t gpu_error;
	uint32_t gpu_last_tail;
	uint32_t reserved;
	uint64_t gpu_spins;
	uint8_t pad[64 - 4 * sizeof(uint32_t) - sizeof(uint64_t)];
};

static_assert(sizeof(ProbeState) == 64, "ProbeState must stay one cacheline");

struct QueueMem {
	const char *kind;
	void *base;
	size_t bytes;
	struct usb4_rdma_dv_wqe *sq;
	struct usb4_rdma_dv_cqe *cq;
	struct usb4_rdma_dv_doorbell *doorbell;
	ProbeState *state;
	size_t sq_bytes;
	size_t cq_bytes;
	size_t doorbell_bytes;
};

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uintptr_t align_up(uintptr_t value, size_t alignment)
{
	return (value + alignment - 1) & ~(uintptr_t)(alignment - 1);
}

static uint32_t host_load_acquire_u32(uint32_t *ptr)
{
	return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

static void host_store_release_u32(uint32_t *ptr, uint32_t value)
{
	__atomic_store_n(ptr, value, __ATOMIC_RELEASE);
}

__device__ static inline uint32_t gpu_plain_load_u32(const uint32_t *ptr)
{
	const volatile uint32_t *vptr = ptr;

	return *vptr;
}

__device__ static inline void gpu_store_release_u32(uint32_t *ptr,
						    uint32_t value)
{
	__hip_atomic_store(ptr, value, __ATOMIC_RELEASE,
			   __HIP_MEMORY_SCOPE_SYSTEM);
}

__device__ static inline uint32_t gpu_tail_pack(uint32_t index,
						uint32_t generation)
{
	return (index & USB4_RDMA_DV_TAIL_INDEX_MASK) |
	       (generation << USB4_RDMA_DV_TAIL_GEN_SHIFT);
}

__global__ void kernel_cqe_plain_load_kernel(
	struct usb4_rdma_dv_doorbell *doorbell, ProbeState *state,
	uint32_t iterations, uint32_t generation, uint32_t max_spins)
{
	if (blockIdx.x || threadIdx.x)
		return;

	for (uint32_t iter = 1; iter <= iterations; iter++) {
		uint32_t want = gpu_tail_pack(iter, generation);
		uint32_t tail = 0;
		uint32_t spins = 0;

		do {
			tail = gpu_plain_load_u32(&doorbell->consumer.cq_tail);
			if (tail == want)
				break;
			if (++spins == max_spins) {
				state->gpu_last_tail = tail;
				gpu_store_release_u32(&state->gpu_error, iter);
				return;
			}
		} while (true);

		state->gpu_last_tail = tail;
		state->gpu_spins += spins;
		gpu_store_release_u32(&doorbell->producer.cq_head, want);
		gpu_store_release_u32(&state->gpu_seen, iter);
	}
}

static bool device_name_matches(const char *name, const char *wanted)
{
	return strcmp(name, wanted) == 0 ||
	       strncmp(name, "usb4_rdma", strlen("usb4_rdma")) == 0 ||
	       strncmp(name, "usb4_apple", strlen("usb4_apple")) == 0;
}

static struct ibv_device *find_device(struct ibv_device **list, int count,
				      const char *wanted)
{
	struct ibv_device *fallback = NULL;

	for (int i = 0; i < count; i++) {
		const char *name = ibv_get_device_name(list[i]);

		if (!strcmp(name, wanted))
			return list[i];
		if (!fallback && device_name_matches(name, wanted))
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

static int kick_queue(struct ibv_qp *qp, const struct usb4_rdma_dv_kick *req)
{
	struct {
		struct ib_uverbs_ioctl_hdr hdr;
		struct ib_uverbs_attr attrs[2];
	} cmd = {
		.hdr = {
			.length = sizeof(cmd),
			.object_id = USB4_RDMA_DV_OBJECT_DEVICE,
			.method_id = USB4_RDMA_DV_METHOD_KICK,
			.num_attrs = 2,
			.driver_id = RDMA_DRIVER_UNKNOWN,
		},
		.attrs = {
			{
				.attr_id = USB4_RDMA_DV_ATTR_KICK_QP,
				.flags = UVERBS_ATTR_F_MANDATORY,
				.data = qp->handle,
			},
			{
				.attr_id = USB4_RDMA_DV_ATTR_KICK_REQ,
				.len = sizeof(*req),
				.flags = UVERBS_ATTR_F_MANDATORY,
				.data = (uintptr_t)req,
			},
		},
	};

	if (ioctl(qp->context->cmd_fd, RDMA_VERBS_IOCTL, &cmd) < 0)
		return errno;
	return 0;
}

static hipError_t alloc_queue_mem(const char *kind, QueueMem *mem,
				  const struct usb4_rdma_dv_query_caps_resp *caps,
				  uint32_t sq_entries, uint32_t cq_entries)
{
	const size_t slop = caps->doorbell_page_size * 3;
	unsigned int flags = hipHostMallocMapped;
	uintptr_t cursor;
	void *base = NULL;
	hipError_t ret;

	memset(mem, 0, sizeof(*mem));
	mem->kind = kind;
	mem->sq_bytes = (size_t)sq_entries * caps->wqe_size;
	mem->cq_bytes = (size_t)cq_entries * caps->cqe_size;
	mem->doorbell_bytes = caps->doorbell_page_size;
	mem->bytes = mem->sq_bytes + mem->cq_bytes + mem->doorbell_bytes +
		     sizeof(ProbeState) + slop;

	if (!strcmp(kind, "managed")) {
		ret = hipMallocManaged(&base, mem->bytes);
	} else if (!strcmp(kind, "host")) {
		ret = hipHostMalloc(&base, mem->bytes, flags);
	} else if (!strcmp(kind, "host-coherent")) {
		flags |= hipHostMallocCoherent;
		ret = hipHostMalloc(&base, mem->bytes, flags);
	} else if (!strcmp(kind, "host-noncoherent")) {
		flags |= hipHostMallocNonCoherent;
		ret = hipHostMalloc(&base, mem->bytes, flags);
	} else if (!strcmp(kind, "host-uncached")) {
		flags |= hipHostMallocUncached;
		ret = hipHostMalloc(&base, mem->bytes, flags);
	} else {
		fprintf(stderr,
			"unknown kind=%s, expected managed|host|host-coherent|host-noncoherent|host-uncached\n",
			kind);
		return hipErrorInvalidValue;
	}
	if (ret != hipSuccess)
		return ret;

	memset(base, 0, mem->bytes);
	cursor = (uintptr_t)base;
	cursor = align_up(cursor, caps->wqe_size);
	mem->sq = (struct usb4_rdma_dv_wqe *)cursor;
	cursor += mem->sq_bytes;
	cursor = align_up(cursor, caps->cqe_size);
	mem->cq = (struct usb4_rdma_dv_cqe *)cursor;
	cursor += mem->cq_bytes;
	cursor = align_up(cursor, caps->doorbell_page_size);
	mem->doorbell = (struct usb4_rdma_dv_doorbell *)cursor;
	cursor += mem->doorbell_bytes;
	cursor = align_up(cursor, 64);
	mem->state = (ProbeState *)cursor;
	mem->base = base;
	return hipSuccess;
}

static void free_queue_mem(QueueMem *mem)
{
	if (!mem->base)
		return;
	if (!strcmp(mem->kind, "managed"))
		(void)hipFree(mem->base);
	else
		(void)hipHostFree(mem->base);
}

static int wait_gpu_seen(ProbeState *state, uint32_t iter, uint32_t timeout_ms)
{
	uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;

	while (now_ns() < deadline) {
		uint32_t err = host_load_acquire_u32(&state->gpu_error);
		uint32_t seen = host_load_acquire_u32(&state->gpu_seen);

		if (err) {
			fprintf(stderr,
				"gpu reported timeout/error at iter=%u last_tail=0x%08x seen=%u\n",
				err, state->gpu_last_tail, seen);
			return -1;
		}
		if (seen >= iter)
			return 0;
	}

	fprintf(stderr, "host timeout waiting for gpu_seen=%u got=%u error=%u\n",
		iter, host_load_acquire_u32(&state->gpu_seen),
		host_load_acquire_u32(&state->gpu_error));
	return -1;
}

static int run_probe(struct ibv_context *ctx, const char *kind,
		     uint32_t iterations, uint32_t timeout_ms)
{
	struct usb4_rdma_dv_query_caps_resp caps;
	struct usb4_rdma_dv_queue_create req = {};
	struct usb4_rdma_dv_queue_resp resp = {};
	struct ibv_qp_init_attr qp_attr = {};
	struct ibv_mr *doorbell_mr = NULL;
	struct ibv_mr *cq_mr = NULL;
	struct ibv_mr *sq_mr = NULL;
	struct ibv_qp *qp = NULL;
	struct ibv_cq *verbs_cq = NULL;
	struct ibv_pd *pd = NULL;
	QueueMem mem = {};
	uint32_t sq_entries;
	uint32_t cq_entries;
	uint64_t start_ns;
	int ret = 1;
	int err;

	err = query_caps(ctx, &caps);
	if (err) {
		fprintf(stderr, "QUERY_CAPS failed: %s (%d)\n", strerror(err),
			err);
		return 1;
	}
	if (caps.abi_version != USB4_RDMA_DV_ABI_VERSION) {
		fprintf(stderr, "unsupported DV ABI kernel=%u userspace=%u\n",
			caps.abi_version, USB4_RDMA_DV_ABI_VERSION);
		return 1;
	}

	sq_entries = caps.default_sq_entries;
	cq_entries = caps.default_cq_entries;
	if (iterations < cq_entries)
		cq_entries = iterations < USB4_RDMA_DV_MIN_QUEUE_ENTRIES ?
				     USB4_RDMA_DV_MIN_QUEUE_ENTRIES :
				     iterations;

	hipError_t hret = alloc_queue_mem(kind, &mem, &caps, sq_entries,
					  cq_entries);
	if (hret != hipSuccess) {
		fprintf(stderr, "alloc kind=%s failed: %s\n", kind,
			hipGetErrorString(hret));
		return 1;
	}

	pd = ibv_alloc_pd(ctx);
	if (!pd) {
		fprintf(stderr, "ibv_alloc_pd: %s\n", strerror(errno));
		goto out;
	}
	sq_mr = ibv_reg_mr(pd, mem.sq, mem.sq_bytes, IBV_ACCESS_LOCAL_WRITE);
	cq_mr = ibv_reg_mr(pd, mem.cq, mem.cq_bytes, IBV_ACCESS_LOCAL_WRITE);
	doorbell_mr = ibv_reg_mr(pd, mem.doorbell, mem.doorbell_bytes,
				 IBV_ACCESS_LOCAL_WRITE);
	if (!sq_mr || !cq_mr || !doorbell_mr) {
		fprintf(stderr,
			"ibv_reg_mr queue memory failed kind=%s errno=%d %s\n",
			kind, errno, strerror(errno));
		goto out;
	}

	verbs_cq = ibv_create_cq(ctx, cq_entries, NULL, NULL, 0);
	if (!verbs_cq) {
		fprintf(stderr, "ibv_create_cq: %s\n", strerror(errno));
		goto out;
	}

	qp_attr.send_cq = verbs_cq;
	qp_attr.recv_cq = verbs_cq;
	qp_attr.qp_type = IBV_QPT_RC;
	qp_attr.cap.max_send_wr = sq_entries;
	qp_attr.cap.max_recv_wr = 1;
	qp_attr.cap.max_send_sge = 1;
	qp_attr.cap.max_recv_sge = 1;
	qp = ibv_create_qp(pd, &qp_attr);
	if (!qp) {
		fprintf(stderr, "ibv_create_qp: %s\n", strerror(errno));
		goto out;
	}

	req.abi_version = USB4_RDMA_DV_ABI_VERSION;
	req.sq_addr = (uintptr_t)mem.sq;
	req.cq_addr = (uintptr_t)mem.cq;
	req.doorbell_addr = (uintptr_t)mem.doorbell;
	req.sq_entries = sq_entries;
	req.cq_entries = cq_entries;
	req.sq_stride = caps.wqe_size;
	req.cq_stride = caps.cqe_size;
	err = create_queue(qp, &req, &resp);
	if (err) {
		fprintf(stderr, "CREATE_QUEUE failed: %s (%d)\n",
			strerror(err), err);
		goto out;
	}

	host_store_release_u32(&mem.doorbell->producer.generation,
			       resp.generation);
	host_store_release_u32(&mem.doorbell->producer.cq_head,
			       usb4_rdma_dv_tail_pack(0, resp.generation));
	host_store_release_u32(&mem.doorbell->producer.sq_tail,
			       usb4_rdma_dv_tail_pack(0, resp.generation));

	hret = hipMemset(mem.state, 0, sizeof(*mem.state));
	if (hret == hipSuccess)
		hret = hipDeviceSynchronize();
	if (hret != hipSuccess) {
		fprintf(stderr, "hipMemset state failed: %s\n",
			hipGetErrorString(hret));
		goto out_destroy;
	}

	hipLaunchKernelGGL(kernel_cqe_plain_load_kernel, dim3(1), dim3(1), 0,
			   0, mem.doorbell, mem.state, iterations,
			   resp.generation, 100000000u);
	hret = hipGetLastError();
	if (hret != hipSuccess) {
		fprintf(stderr, "kernel launch failed: %s\n",
			hipGetErrorString(hret));
		goto out_destroy;
	}

	start_ns = now_ns();
	for (uint32_t iter = 1; iter <= iterations; iter++) {
		uint32_t slot = (iter - 1) % sq_entries;
		uint32_t tail = usb4_rdma_dv_tail_pack(iter, resp.generation);
		struct usb4_rdma_dv_kick kick = {
			.sq_tail = tail,
		};
		struct usb4_rdma_dv_wqe *wqe = &mem.sq[slot];

		memset(wqe, 0, sizeof(*wqe));
		wqe->opcode = USB4_RDMA_DV_WQE_NOP;
		wqe->flags = USB4_RDMA_DV_WQE_F_SIGNALED;
		wqe->wr_id = PROBE_WR_ID_BASE + iter;
		wqe->generation = resp.generation;
		__atomic_thread_fence(__ATOMIC_RELEASE);
		host_store_release_u32(&mem.doorbell->producer.sq_tail, tail);

		err = kick_queue(qp, &kick);
		if (err) {
			fprintf(stderr, "KICK failed iter=%u: %s (%d)\n", iter,
				strerror(err), err);
			goto out_destroy;
		}
		if (wait_gpu_seen(mem.state, iter, timeout_ms))
			goto out_destroy;
	}

	hret = hipDeviceSynchronize();
	if (hret != hipSuccess) {
		fprintf(stderr, "hipDeviceSynchronize failed: %s\n",
			hipGetErrorString(hret));
		goto out_destroy;
	}

	{
		double secs = (double)(now_ns() - start_ns) / 1000000000.0;

		printf("kind=%s direction=kernel-cqe-to-gpu-plain-load iterations=%u status=OK elapsed_sec=%.6f avg_us=%.3f gpu_seen=%u gpu_error=%u gpu_spins=%" PRIu64 "\n",
		       kind, iterations, secs, secs * 1000000.0 / iterations,
		       host_load_acquire_u32(&mem.state->gpu_seen),
		       host_load_acquire_u32(&mem.state->gpu_error),
		       mem.state->gpu_spins);
	}
	ret = 0;

out_destroy:
	err = destroy_queue(qp);
	if (err)
		fprintf(stderr, "DESTROY_QUEUE failed: %s (%d)\n",
			strerror(err), err);
out:
	if (qp && ibv_destroy_qp(qp))
		fprintf(stderr, "ibv_destroy_qp: %s\n", strerror(errno));
	if (verbs_cq && ibv_destroy_cq(verbs_cq))
		fprintf(stderr, "ibv_destroy_cq: %s\n", strerror(errno));
	if (doorbell_mr && ibv_dereg_mr(doorbell_mr))
		fprintf(stderr, "ibv_dereg_mr(doorbell): %s\n", strerror(errno));
	if (cq_mr && ibv_dereg_mr(cq_mr))
		fprintf(stderr, "ibv_dereg_mr(CQ): %s\n", strerror(errno));
	if (sq_mr && ibv_dereg_mr(sq_mr))
		fprintf(stderr, "ibv_dereg_mr(SQ): %s\n", strerror(errno));
	if (pd && ibv_dealloc_pd(pd))
		fprintf(stderr, "ibv_dealloc_pd: %s\n", strerror(errno));
	free_queue_mem(&mem);
	return ret;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [kind] [iterations] [device] [timeout-ms]\n"
		"  kind: managed|host|host-coherent|host-noncoherent|host-uncached\n",
		argv0);
}

int main(int argc, char **argv)
{
	const char *kind = argc > 1 ? argv[1] : "host-coherent";
	uint32_t iterations = argc > 2 ? strtoul(argv[2], NULL, 0) : 10000;
	const char *wanted = argc > 3 ? argv[3] : "usb4_rdma0";
	uint32_t timeout_ms = argc > 4 ? strtoul(argv[4], NULL, 0) : 5000;
	struct ibv_device **list = NULL;
	struct ibv_context *ctx = NULL;
	struct ibv_device *dev;
	int count = 0;
	int ret = 1;
	hipError_t hret;

	if (!iterations || !timeout_ms) {
		usage(argv[0]);
		return 2;
	}

	hret = hipSetDevice(0);
	if (hret != hipSuccess) {
		fprintf(stderr, "hipSetDevice failed: %s\n",
			hipGetErrorString(hret));
		return 1;
	}

	list = ibv_get_device_list(&count);
	if (!list) {
		perror("ibv_get_device_list");
		return 1;
	}
	dev = find_device(list, count, wanted);
	if (!dev) {
		fprintf(stderr, "no USB4 RDMA device found wanted=%s\n", wanted);
		goto out;
	}
	ctx = ibv_open_device(dev);
	if (!ctx) {
		fprintf(stderr, "ibv_open_device(%s): %s\n",
			ibv_get_device_name(dev), strerror(errno));
		goto out;
	}

	printf("device=%s kind=%s iterations=%u timeout_ms=%u\n",
	       ibv_get_device_name(dev), kind, iterations, timeout_ms);
	ret = run_probe(ctx, kind, iterations, timeout_ms);

out:
	if (ctx)
		ibv_close_device(ctx);
	if (list)
		ibv_free_device_list(list);
	return ret;
}
