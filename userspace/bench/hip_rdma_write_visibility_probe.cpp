// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * Two-host probe for the exact USB4 GDA visibility pattern used by rocSHMEM
 * alltoall staging:
 *
 *   peer kernel writes payload into a HIP host allocation via RDMA WRITE
 *   peer kernel writes a later signal word via RDMA WRITE
 *   GPU observes the signal and then reads the payload
 *
 * The GPU intentionally preloads the payload before each signal wait. If the
 * allocation/load mode can return stale GPU-cached payload after the kernel's
 * CPU copy, this probe catches it without involving RCCL or rocSHMEM.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>
#include <hip/hip_runtime.h>
#include <infiniband/verbs.h>

#include <vector>

#define VIS_MAGIC 0x56495332u /* VIS2 */

enum load_mode {
	LOAD_NORMAL = 0,
	LOAD_ATOMIC = 1,
};

enum source_fill {
	SOURCE_FILL_CPU = 0,
	SOURCE_FILL_GPU = 1,
	SOURCE_FILL_GPU_HDP = 2,
	SOURCE_FILL_GPU_SYNC = 3,
	SOURCE_FILL_GPU_HDP_SYNC = 4,
	SOURCE_FILL_GPU_HOST_HDP = 5,
};

enum source_reg {
	SOURCE_REG_MR = 0,
	SOURCE_REG_DMABUF = 1,
};

struct opts {
	const char *role;
	const char *dev;
	const char *connect_host;
	const char *kind;
	const char *mode_name;
	const char *source_kind;
	const char *source_fill_name;
	const char *source_reg_name;
	int port;
	int gid_index;
	int ib_port;
	int mtu;
	int timeout_ms;
	uint32_t count;
	size_t size;
	enum load_mode mode;
	enum source_fill source_fill;
	enum source_reg source_reg;
};

struct peer_info {
	uint32_t magic;
	uint32_t qpn;
	uint32_t psn;
	uint32_t lid;
	uint32_t rkey;
	uint64_t addr;
	uint8_t gid[16];
};

struct alignas(64) ProbeState {
	uint32_t gpu_seen;
	uint32_t gpu_error_seq;
	uint32_t gpu_error_off;
	uint32_t gpu_error_mode;
	uint32_t gpu_got;
	uint32_t gpu_want;
	uint64_t gpu_spins;
	uint32_t prefetch_sink;
	uint8_t pad[64 - 6 * sizeof(uint32_t) - sizeof(uint64_t) -
		    sizeof(uint32_t)];
};

struct HipRegion {
	const char *kind;
	void *base;
	size_t bytes;
	uint8_t *payload;
	uint64_t *signal;
	ProbeState *state;
	size_t signal_offset;
	size_t state_offset;
};

struct alignas(64) SourceFillState {
	uint32_t ready_seq;
	uint32_t error_seq;
	uint32_t error_off;
	uint32_t pad[13];
};

struct SenderSource {
	const char *kind;
	const char *fill_name;
	const char *reg_name;
	void *payload_base;
	uint8_t *payload;
	size_t payload_bytes;
	bool payload_hip_alloc;
	bool payload_hip_vmm_alloc;
	bool payload_hsa_alloc;
	bool payload_host_alloc;
	bool payload_malloc_alloc;
	hipMemGenericAllocationHandle_t payload_vmm_handle;
	size_t payload_vmm_bytes;
	struct ibv_mr *payload_mr;
	int dmabuf_fd;
	bool dmabuf_fd_is_hsa;
	uint64_t dmabuf_offset;
	void *signal_base;
	uint64_t *signal;
	struct ibv_mr *signal_mr;
	SourceFillState *fill_state;
	uint32_t *hdp_flush_ptr;
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

static uint64_t host_load_acquire_u64(uint64_t *ptr)
{
	return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

static __host__ __device__ inline uint32_t pattern_word(uint32_t seq,
							uint32_t word)
{
	return 0x9e3779b9u ^ (seq * 0x10001u) ^ (word * 0x45d9f3bu) ^
	       (word >> 3);
}

static int send_all(int fd, const void *buf, size_t len)
{
	const char *p = (const char *)buf;

	while (len) {
		ssize_t n = send(fd, p, len, 0);

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

static int recv_all(int fd, void *buf, size_t len)
{
	char *p = (char *)buf;

	while (len) {
		ssize_t n = recv(fd, p, len, MSG_WAITALL);

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
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((uint16_t)port);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) ||
	    listen(fd, 1)) {
		close(fd);
		return -1;
	}
	return fd;
}

static int tcp_connect(const char *host, int port)
{
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	struct addrinfo *ai;
	char portbuf[16];
	int fd = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_family = AF_UNSPEC;
	snprintf(portbuf, sizeof(portbuf), "%d", port);
	if (getaddrinfo(host, portbuf, &hints, &res))
		return -1;
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (!connect(fd, ai->ai_addr, ai->ai_addrlen))
			break;
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

static int qp_to_rts(struct ibv_qp *qp, int port, int sgid_index,
		     const union ibv_gid *dgid, uint32_t dlid,
		     uint32_t dest_qpn, uint32_t local_psn,
		     uint32_t remote_psn, enum ibv_mtu path_mtu,
		     int qp_access_flags)
{
	struct ibv_qp_attr a;
	int ret;

	memset(&a, 0, sizeof(a));
	a.qp_state = IBV_QPS_INIT;
	a.pkey_index = 0;
	a.port_num = (uint8_t)port;
	a.qp_access_flags = qp_access_flags;
	ret = ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
			    IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
	if (ret) {
		perror("modify INIT");
		return ret;
	}

	memset(&a, 0, sizeof(a));
	a.qp_state = IBV_QPS_RTR;
	a.path_mtu = path_mtu;
	a.rq_psn = remote_psn;
	a.dest_qp_num = dest_qpn;
	a.max_dest_rd_atomic = 1;
	a.min_rnr_timer = 12;
	a.ah_attr.dlid = (uint16_t)dlid;
	a.ah_attr.sl = 0;
	a.ah_attr.src_path_bits = 0;
	a.ah_attr.port_num = (uint8_t)port;
	if (dgid->global.interface_id) {
		a.ah_attr.is_global = 1;
		a.ah_attr.grh.dgid = *dgid;
		a.ah_attr.grh.sgid_index = sgid_index;
		a.ah_attr.grh.hop_limit = 1;
	}
	ret = ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_AV |
			    IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
			    IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
			    IBV_QP_MIN_RNR_TIMER);
	if (ret) {
		perror("modify RTR");
		return ret;
	}

	memset(&a, 0, sizeof(a));
	a.qp_state = IBV_QPS_RTS;
	a.sq_psn = local_psn;
	a.timeout = 14;
	a.retry_cnt = 7;
	a.rnr_retry = 7;
	a.max_rd_atomic = 1;
	ret = ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_SQ_PSN |
			    IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
			    IBV_QP_RNR_RETRY | IBV_QP_MAX_QP_RD_ATOMIC);
	if (ret)
		perror("modify RTS");
	return ret;
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

static int parse_mode(const char *s, enum load_mode *mode)
{
	if (!strcmp(s, "normal"))
		*mode = LOAD_NORMAL;
	else if (!strcmp(s, "atomic"))
		*mode = LOAD_ATOMIC;
	else
		return -1;
	return 0;
}

static int parse_source_fill(const char *s, enum source_fill *fill)
{
	if (!strcmp(s, "cpu"))
		*fill = SOURCE_FILL_CPU;
	else if (!strcmp(s, "gpu"))
		*fill = SOURCE_FILL_GPU;
	else if (!strcmp(s, "gpu-hdp"))
		*fill = SOURCE_FILL_GPU_HDP;
	else if (!strcmp(s, "gpu-sync"))
		*fill = SOURCE_FILL_GPU_SYNC;
	else if (!strcmp(s, "gpu-hdp-sync"))
		*fill = SOURCE_FILL_GPU_HDP_SYNC;
	else if (!strcmp(s, "gpu-host-hdp"))
		*fill = SOURCE_FILL_GPU_HOST_HDP;
	else
		return -1;
	return 0;
}

static int parse_source_reg(const char *s, enum source_reg *reg)
{
	if (!strcmp(s, "reg_mr"))
		*reg = SOURCE_REG_MR;
	else if (!strcmp(s, "dmabuf"))
		*reg = SOURCE_REG_DMABUF;
	else
		return -1;
	return 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s --role recv|send --dev DEV --gid-index N --port P\n"
		"          [--connect HOST] [--kind host-coherent|host-uncached|host|managed]\n"
		"          [--mode normal|atomic]\n"
		"          [--source-kind malloc|device|managed|host|host-coherent|host-uncached|\n"
		"                         vmm-device-pinned|vmm-device-uncached|vmm-host-pinned|vmm-host-uncached|\n"
		"                         ext-finegrained|ext-uncached|hsa-gpu-extended]\n"
		"          [--source-fill cpu|gpu|gpu-hdp|gpu-sync|gpu-hdp-sync|gpu-host-hdp]\n"
		"          [--source-reg reg_mr|dmabuf]\n"
		"          [--size BYTES] [--count N] [--timeout-ms N]\n",
		argv0);
}

static int parse_opts(int argc, char **argv, struct opts *o)
{
	memset(o, 0, sizeof(*o));
	o->dev = "usb4_rdma0";
	o->kind = "host-coherent";
	o->mode_name = "normal";
	o->source_kind = "malloc";
	o->source_fill_name = "cpu";
	o->source_reg_name = "reg_mr";
	o->mode = LOAD_NORMAL;
	o->source_fill = SOURCE_FILL_CPU;
	o->source_reg = SOURCE_REG_MR;
	o->port = 18518;
	o->gid_index = 1;
	o->ib_port = 1;
	o->mtu = 4096;
	o->timeout_ms = 5000;
	o->count = 1000;
	o->size = 32;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--role") && i + 1 < argc) {
			o->role = argv[++i];
		} else if (!strcmp(argv[i], "--dev") && i + 1 < argc) {
			o->dev = argv[++i];
		} else if (!strcmp(argv[i], "--connect") && i + 1 < argc) {
			o->connect_host = argv[++i];
		} else if (!strcmp(argv[i], "--kind") && i + 1 < argc) {
			o->kind = argv[++i];
		} else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
			o->mode_name = argv[++i];
			if (parse_mode(o->mode_name, &o->mode))
				return -1;
		} else if (!strcmp(argv[i], "--source-kind") && i + 1 < argc) {
			o->source_kind = argv[++i];
		} else if (!strcmp(argv[i], "--source-fill") && i + 1 < argc) {
			o->source_fill_name = argv[++i];
			if (parse_source_fill(o->source_fill_name,
					      &o->source_fill))
				return -1;
		} else if (!strcmp(argv[i], "--source-reg") && i + 1 < argc) {
			o->source_reg_name = argv[++i];
			if (parse_source_reg(o->source_reg_name,
					     &o->source_reg))
				return -1;
		} else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
			o->port = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--gid-index") && i + 1 < argc) {
			o->gid_index = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--ib-port") && i + 1 < argc) {
			o->ib_port = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--mtu") && i + 1 < argc) {
			o->mtu = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--timeout-ms") && i + 1 < argc) {
			o->timeout_ms = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--count") && i + 1 < argc) {
			o->count = (uint32_t)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
			o->size = strtoull(argv[++i], NULL, 0);
		} else {
			return -1;
		}
	}

	if (!o->role || (strcmp(o->role, "recv") && strcmp(o->role, "send")) ||
	    o->port <= 0 || o->gid_index < 0 || o->ib_port <= 0 ||
	    o->timeout_ms <= 0 || !o->count || !o->size ||
	    (o->size % sizeof(uint32_t)) != 0)
		return -1;
	if (o->source_fill == SOURCE_FILL_CPU &&
	    strcmp(o->source_kind, "malloc") &&
	    strcmp(o->source_kind, "host") &&
	    strcmp(o->source_kind, "host-coherent") &&
	    strcmp(o->source_kind, "host-uncached") &&
	    strcmp(o->source_kind, "managed"))
		return -1;
	if (o->source_fill != SOURCE_FILL_CPU &&
	    !strcmp(o->source_kind, "malloc"))
		return -1;
	if (o->source_reg == SOURCE_REG_DMABUF &&
	    !strcmp(o->source_kind, "malloc"))
		return -1;
	return 0;
}

static bool source_kind_is_vmm(const char *kind)
{
	return !strcmp(kind, "vmm-device-pinned") ||
	       !strcmp(kind, "vmm-device-uncached") ||
	       !strcmp(kind, "vmm-host-pinned") ||
	       !strcmp(kind, "vmm-host-uncached");
}

static bool source_kind_is_hsa(const char *kind)
{
	return !strcmp(kind, "hsa-gpu-extended");
}

static hipError_t alloc_region(const char *kind, size_t payload_size,
			       HipRegion *region)
{
	unsigned int flags = hipHostMallocMapped;
	uintptr_t cursor;
	void *base = NULL;
	size_t bytes = align_up(payload_size, 64) + 64 + sizeof(ProbeState) +
		       4096;
	hipError_t ret;

	memset(region, 0, sizeof(*region));
	region->kind = kind;
	region->bytes = bytes;
	if (!strcmp(kind, "managed")) {
		ret = hipMallocManaged(&base, bytes);
	} else if (!strcmp(kind, "host")) {
		ret = hipHostMalloc(&base, bytes, flags);
	} else if (!strcmp(kind, "host-coherent")) {
		ret = hipHostMalloc(&base, bytes,
				    flags | hipHostMallocCoherent);
	} else if (!strcmp(kind, "host-uncached")) {
		ret = hipHostMalloc(&base, bytes,
				    flags | hipHostMallocUncached);
	} else {
		return hipErrorInvalidValue;
	}
	if (ret != hipSuccess)
		return ret;

	memset(base, 0, bytes);
	cursor = align_up((uintptr_t)base, 64);
	region->payload = (uint8_t *)cursor;
	cursor += align_up(payload_size, 64);
	region->signal_offset = cursor - (uintptr_t)region->payload;
	region->signal = (uint64_t *)cursor;
	cursor += 64;
	cursor = align_up(cursor, 64);
	region->state_offset = cursor - (uintptr_t)region->payload;
	region->state = (ProbeState *)cursor;
	region->base = base;
	return hipSuccess;
}

static void free_region(HipRegion *region)
{
	if (!region->base)
		return;
	if (!strcmp(region->kind, "managed"))
		(void)hipFree(region->base);
	else
		(void)hipHostFree(region->base);
}

__device__ static inline uint32_t gpu_atomic_load_u32(const uint32_t *ptr)
{
	return __hip_atomic_load(const_cast<uint32_t *>(ptr), __ATOMIC_ACQUIRE,
				 __HIP_MEMORY_SCOPE_SYSTEM);
}

__device__ static inline uint64_t gpu_atomic_load_u64(const uint64_t *ptr)
{
	return __hip_atomic_load(const_cast<uint64_t *>(ptr), __ATOMIC_ACQUIRE,
				 __HIP_MEMORY_SCOPE_SYSTEM);
}

__device__ static inline uint32_t gpu_payload_load_u32(const uint32_t *ptr,
						       enum load_mode mode)
{
	if (mode == LOAD_ATOMIC)
		return gpu_atomic_load_u32(ptr);
	return *reinterpret_cast<const volatile uint32_t *>(ptr);
}

__device__ static inline void gpu_store_release_u32(uint32_t *ptr,
						    uint32_t value)
{
	__hip_atomic_store(ptr, value, __ATOMIC_RELEASE,
			   __HIP_MEMORY_SCOPE_SYSTEM);
}

__device__ static inline bool gpu_cas_system_u32(uint32_t *ptr,
						 uint32_t expected,
						 uint32_t desired)
{
	return __hip_atomic_compare_exchange_strong(
		ptr, &expected, desired, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE,
		__HIP_MEMORY_SCOPE_SYSTEM);
}

__global__ void source_fill_kernel(uint8_t *payload, SourceFillState *state,
				   uint32_t words, uint32_t seq,
				   uint32_t *hdp_flush_ptr)
{
	uint32_t *payload_words = reinterpret_cast<uint32_t *>(payload);
	uint32_t tid = threadIdx.x + blockIdx.x * blockDim.x;
	uint32_t nthreads = blockDim.x * gridDim.x;

	for (uint32_t word = tid; word < words; word += nthreads)
		payload_words[word] = pattern_word(seq, word);

	__syncthreads();
	if (tid == 0) {
		__threadfence_system();
		if (hdp_flush_ptr) {
			__atomic_store_n(hdp_flush_ptr, 1u, __ATOMIC_SEQ_CST);
			__threadfence_system();
		}
		gpu_store_release_u32(&state->ready_seq, seq);
	}
}

__global__ void rdma_write_visibility_kernel(uint8_t *payload,
					     uint64_t *signal,
					     ProbeState *state,
					     uint32_t words,
					     uint32_t count,
					     enum load_mode mode,
					     uint32_t max_spins)
{
	__shared__ uint32_t block_error;
	uint32_t *payload_words = reinterpret_cast<uint32_t *>(payload);
	uint32_t tid = threadIdx.x + blockIdx.x * blockDim.x;
	uint32_t nthreads = blockDim.x * gridDim.x;
	uint32_t sink = 0;

	for (uint32_t seq = 1; seq <= count; ++seq) {
		if (tid == 0)
			block_error = 0;
		__syncthreads();

		for (uint32_t word = tid; word < words; word += nthreads)
			sink ^= *reinterpret_cast<volatile uint32_t *>(
				payload_words + word);
		if (tid == 0)
			state->prefetch_sink ^= sink;
		__syncthreads();

		uint32_t spins = 0;
		while (gpu_atomic_load_u64(signal) != seq) {
			if (++spins == max_spins) {
				if (tid == 0) {
					state->gpu_spins += spins;
					state->gpu_error_mode =
						static_cast<uint32_t>(mode);
					gpu_store_release_u32(
						&state->gpu_error_seq, seq);
				}
				return;
			}
		}

		if (tid == 0)
			state->gpu_spins += spins;
		for (uint32_t word = tid; word < words; word += nthreads) {
			uint32_t got =
				gpu_payload_load_u32(payload_words + word, mode);
			uint32_t want = pattern_word(seq, word);

			if (got != want) {
				if (gpu_cas_system_u32(&state->gpu_error_seq,
						       0u, seq)) {
					state->gpu_error_off = word * 4;
					state->gpu_error_mode =
						static_cast<uint32_t>(mode);
					state->gpu_got = got;
					state->gpu_want = want;
					__threadfence_system();
				}
				atomicExch(&block_error, 1u);
			}
		}

		__syncthreads();
		if (block_error)
			return;
		if (tid == 0)
			gpu_store_release_u32(&state->gpu_seen, seq);
		__syncthreads();
	}
}

static int wait_gpu_seen(HipRegion *region, uint32_t seq, int timeout_ms)
{
	uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;

	while (now_ns() < deadline) {
		uint32_t err = host_load_acquire_u32(&region->state->gpu_error_seq);
		uint32_t seen = host_load_acquire_u32(&region->state->gpu_seen);

		if (err) {
			fprintf(stderr,
				"gpu error seq=%u seen=%u off=%u mode=%u got=0x%08x want=0x%08x signal=%" PRIu64 "\n",
				err, seen, region->state->gpu_error_off,
				region->state->gpu_error_mode,
				region->state->gpu_got, region->state->gpu_want,
				host_load_acquire_u64(region->signal));
			return -1;
		}
		if (seen >= seq)
			return 0;
		usleep(100);
	}
	fprintf(stderr, "timeout waiting gpu_seen=%u got=%u signal=%" PRIu64 "\n",
		seq, host_load_acquire_u32(&region->state->gpu_seen),
		host_load_acquire_u64(region->signal));
	return -1;
}

static int poll_send_cq(struct ibv_cq *cq, int timeout_ms)
{
	uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;
	struct ibv_wc wc;

	while (now_ns() < deadline) {
		int n = ibv_poll_cq(cq, 1, &wc);

		if (n < 0) {
			fprintf(stderr, "ibv_poll_cq failed: %d\n", n);
			return -1;
		}
		if (!n) {
			usleep(100);
			continue;
		}
		if (wc.status != IBV_WC_SUCCESS) {
			fprintf(stderr,
				"send wc error wr_id=%" PRIu64 " status=%u opcode=%u byte_len=%u\n",
				wc.wr_id, wc.status, wc.opcode, wc.byte_len);
			return -1;
		}
		return 0;
	}
	fprintf(stderr, "timeout waiting send CQE\n");
	return -1;
}

static void fill_payload(uint32_t *payload, size_t bytes, uint32_t seq)
{
	size_t words = bytes / sizeof(uint32_t);

	for (size_t i = 0; i < words; ++i)
		payload[i] = pattern_word(seq, (uint32_t)i);
}

struct HsaAgentInfo {
	hsa_agent_t agent;
	hsa_device_type_t type;
};

struct HsaPoolInfo {
	hsa_amd_memory_pool_t pool;
	hsa_agent_t owner;
	hsa_device_type_t owner_type;
	hsa_amd_segment_t segment;
	hsa_amd_memory_pool_location_t location;
	uint32_t flags;
	bool alloc_allowed;
	size_t alloc_granule;
};

struct HsaPoolEnumCtx {
	hsa_agent_t owner;
	hsa_device_type_t owner_type;
	std::vector<HsaPoolInfo> *pools;
};

static const char *hsa_status_name(hsa_status_t status)
{
	const char *name = NULL;

	if (hsa_status_string(status, &name) != HSA_STATUS_SUCCESS || !name)
		return "unknown";
	return name;
}

static hsa_status_t collect_hsa_agent(hsa_agent_t agent, void *data)
{
	std::vector<HsaAgentInfo> *agents =
		static_cast<std::vector<HsaAgentInfo> *>(data);
	HsaAgentInfo info = {};
	hsa_status_t status;

	info.agent = agent;
	status = hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &info.type);
	if (status != HSA_STATUS_SUCCESS)
		info.type = static_cast<hsa_device_type_t>(-1);
	agents->push_back(info);
	return HSA_STATUS_SUCCESS;
}

static hsa_status_t collect_hsa_pool(hsa_amd_memory_pool_t pool, void *data)
{
	HsaPoolEnumCtx *ctx = static_cast<HsaPoolEnumCtx *>(data);
	HsaPoolInfo info = {};
	hsa_status_t status;

	info.pool = pool;
	info.owner = ctx->owner;
	info.owner_type = ctx->owner_type;

	status = hsa_amd_memory_pool_get_info(
		pool, HSA_AMD_MEMORY_POOL_INFO_SEGMENT, &info.segment);
	if (status != HSA_STATUS_SUCCESS)
		return HSA_STATUS_SUCCESS;
	if (info.segment == HSA_AMD_SEGMENT_GLOBAL) {
		(void)hsa_amd_memory_pool_get_info(
			pool, HSA_AMD_MEMORY_POOL_INFO_GLOBAL_FLAGS,
			&info.flags);
	}
	(void)hsa_amd_memory_pool_get_info(
		pool, HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_ALLOWED,
		&info.alloc_allowed);
	(void)hsa_amd_memory_pool_get_info(
		pool, HSA_AMD_MEMORY_POOL_INFO_LOCATION, &info.location);
	if (info.alloc_allowed) {
		(void)hsa_amd_memory_pool_get_info(
			pool, HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_GRANULE,
			&info.alloc_granule);
	}
	ctx->pools->push_back(info);
	return HSA_STATUS_SUCCESS;
}

static bool hsa_pool_matches_source_kind(const HsaPoolInfo &pool,
					 const char *kind)
{
	if (!strcmp(kind, "hsa-gpu-extended")) {
		return pool.owner_type == HSA_DEVICE_TYPE_GPU &&
		       pool.segment == HSA_AMD_SEGMENT_GLOBAL &&
		       pool.location == HSA_AMD_MEMORY_POOL_LOCATION_GPU &&
		       pool.alloc_allowed &&
		       (pool.flags &
			HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_EXTENDED_SCOPE_FINE_GRAINED);
	}
	return false;
}

static size_t round_up_size(size_t value, size_t alignment)
{
	if (!alignment)
		return value;
	size_t rem = value % alignment;
	return rem ? value + alignment - rem : value;
}

static hipError_t alloc_source_payload_hsa(const char *kind, size_t bytes,
					   SenderSource *src)
{
	std::vector<HsaAgentInfo> agents;
	std::vector<HsaPoolInfo> pools;
	hsa_amd_memory_pool_t selected = {};
	size_t granule = 4096;
	void *ptr = NULL;
	hsa_status_t status;

	status = hsa_init();
	if (status != HSA_STATUS_SUCCESS) {
		fprintf(stderr, "hsa_init failed: %s\n",
			hsa_status_name(status));
		return hipErrorInvalidValue;
	}

	status = hsa_iterate_agents(collect_hsa_agent, &agents);
	if (status != HSA_STATUS_SUCCESS) {
		fprintf(stderr, "hsa_iterate_agents failed: %s\n",
			hsa_status_name(status));
		return hipErrorInvalidValue;
	}

	for (const HsaAgentInfo &agent : agents) {
		HsaPoolEnumCtx ctx = { agent.agent, agent.type, &pools };
		(void)hsa_amd_agent_iterate_memory_pools(
			agent.agent, collect_hsa_pool, &ctx);
	}

	for (const HsaPoolInfo &pool : pools) {
		if (!hsa_pool_matches_source_kind(pool, kind))
			continue;
		selected = pool.pool;
		granule = pool.alloc_granule ? pool.alloc_granule : 4096;
		break;
	}
	if (!selected.handle) {
		fprintf(stderr, "no HSA pool matches source kind=%s\n", kind);
		return hipErrorInvalidValue;
	}

	size_t size = round_up_size(bytes, granule);
	status = hsa_amd_memory_pool_allocate(selected, size, 0, &ptr);
	if (status != HSA_STATUS_SUCCESS) {
		fprintf(stderr, "hsa_amd_memory_pool_allocate kind=%s failed: %s\n",
			kind, hsa_status_name(status));
		return hipErrorOutOfMemory;
	}

	std::vector<hsa_agent_t> allowed_agents;
	for (const HsaAgentInfo &agent : agents) {
		hsa_amd_memory_pool_access_t access =
			HSA_AMD_MEMORY_POOL_ACCESS_NEVER_ALLOWED;

		if (agent.type != HSA_DEVICE_TYPE_CPU &&
		    agent.type != HSA_DEVICE_TYPE_GPU)
			continue;
		status = hsa_amd_agent_memory_pool_get_info(
			agent.agent, selected,
			HSA_AMD_AGENT_MEMORY_POOL_INFO_ACCESS, &access);
		if (status == HSA_STATUS_SUCCESS &&
		    access != HSA_AMD_MEMORY_POOL_ACCESS_NEVER_ALLOWED)
			allowed_agents.push_back(agent.agent);
	}

	status = hsa_amd_agents_allow_access(
		static_cast<uint32_t>(allowed_agents.size()),
		allowed_agents.data(), NULL, ptr);
	if (status != HSA_STATUS_SUCCESS) {
		fprintf(stderr, "hsa_amd_agents_allow_access kind=%s failed: %s\n",
			kind, hsa_status_name(status));
		(void)hsa_amd_memory_pool_free(ptr);
		return hipErrorInvalidValue;
	}

	src->payload_base = ptr;
	src->payload = static_cast<uint8_t *>(ptr);
	src->payload_bytes = size;
	src->payload_hsa_alloc = true;
	memset(ptr, 0, bytes);
	return hipSuccess;
}

static hipError_t alloc_source_payload_vmm(const char *kind, size_t bytes,
					   SenderSource *src)
{
	hipMemAllocationProp prop = {};
	hipMemAccessDesc access = {};
	hipMemGenericAllocationHandle_t handle = NULL;
	void *ptr = NULL;
	size_t granule = 0;
	size_t map_bytes;
	int device = 0;
	hipError_t ret;

	ret = hipGetDevice(&device);
	if (ret != hipSuccess)
		return ret;

	prop.type = strstr(kind, "uncached") ? hipMemAllocationTypeUncached :
					       hipMemAllocationTypePinned;
	prop.requestedHandleTypes = hipMemHandleTypePosixFileDescriptor;
	prop.location.type = strstr(kind, "device") ?
				     hipMemLocationTypeDevice :
				     hipMemLocationTypeHost;
	prop.location.id = prop.location.type == hipMemLocationTypeDevice ?
				   device :
				   0;
	prop.allocFlags.gpuDirectRDMACapable = 1;

	ret = hipMemGetAllocationGranularity(
		&granule, &prop, hipMemAllocationGranularityMinimum);
	if (ret != hipSuccess)
		return ret;
	if (!granule)
		granule = 4096;
	map_bytes = (bytes + granule - 1) & ~(granule - 1);

	ret = hipMemAddressReserve(&ptr, map_bytes, granule, NULL, 0);
	if (ret != hipSuccess)
		return ret;

	ret = hipMemCreate(&handle, map_bytes, &prop, 0);
	if (ret != hipSuccess)
		goto err_addr;

	ret = hipMemMap(ptr, map_bytes, 0, handle, 0);
	if (ret != hipSuccess)
		goto err_handle;

	access.location.type = hipMemLocationTypeDevice;
	access.location.id = device;
	access.flags = hipMemAccessFlagsProtReadWrite;
	ret = hipMemSetAccess(ptr, map_bytes, &access, 1);
	if (ret != hipSuccess)
		goto err_unmap;

	src->payload_base = ptr;
	src->payload = static_cast<uint8_t *>(ptr);
	src->payload_vmm_handle = handle;
	src->payload_vmm_bytes = map_bytes;
	src->payload_hip_vmm_alloc = true;
	return hipMemset(ptr, 0, bytes);

err_unmap:
	(void)hipMemUnmap(ptr, map_bytes);
err_handle:
	(void)hipMemRelease(handle);
err_addr:
	(void)hipMemAddressFree(ptr, map_bytes);
	return ret;
}

static hipError_t alloc_source_payload(const char *kind, size_t bytes,
				       SenderSource *src)
{
	unsigned int flags = hipHostMallocMapped;

	if (!strcmp(kind, "malloc")) {
		if (posix_memalign(&src->payload_base, 64, bytes))
			return hipErrorOutOfMemory;
		src->payload_malloc_alloc = true;
		memset(src->payload_base, 0, bytes);
		src->payload = (uint8_t *)src->payload_base;
		return hipSuccess;
	}

	if (source_kind_is_hsa(kind)) {
		return alloc_source_payload_hsa(kind, bytes, src);
	} else if (source_kind_is_vmm(kind)) {
		return alloc_source_payload_vmm(kind, bytes, src);
	} else if (!strcmp(kind, "ext-finegrained")) {
		hipError_t ret = hipExtMallocWithFlags(
			&src->payload_base, bytes, hipDeviceMallocFinegrained);
		if (ret != hipSuccess)
			return ret;
		src->payload_hip_alloc = true;
	} else if (!strcmp(kind, "ext-uncached")) {
		hipError_t ret = hipExtMallocWithFlags(
			&src->payload_base, bytes, hipDeviceMallocUncached);
		if (ret != hipSuccess)
			return ret;
		src->payload_hip_alloc = true;
	} else if (!strcmp(kind, "device")) {
		hipError_t ret = hipMalloc(&src->payload_base, bytes);
		if (ret != hipSuccess)
			return ret;
		src->payload_hip_alloc = true;
	} else if (!strcmp(kind, "managed")) {
		hipError_t ret = hipMallocManaged(&src->payload_base, bytes);
		if (ret != hipSuccess)
			return ret;
		src->payload_hip_alloc = true;
	} else if (!strcmp(kind, "host")) {
		hipError_t ret = hipHostMalloc(&src->payload_base, bytes,
					       flags);
		if (ret != hipSuccess)
			return ret;
		src->payload_host_alloc = true;
	} else if (!strcmp(kind, "host-coherent")) {
		hipError_t ret = hipHostMalloc(&src->payload_base, bytes,
					       flags |
						       hipHostMallocCoherent);
		if (ret != hipSuccess)
			return ret;
		src->payload_host_alloc = true;
	} else if (!strcmp(kind, "host-uncached")) {
		hipError_t ret = hipHostMalloc(&src->payload_base, bytes,
					       flags |
						       hipHostMallocUncached);
		if (ret != hipSuccess)
			return ret;
		src->payload_host_alloc = true;
	} else {
		return hipErrorInvalidValue;
	}

	src->payload = (uint8_t *)src->payload_base;
	return hipMemset(src->payload_base, 0, bytes);
}

static int init_sender_source(struct opts *o, struct ibv_pd *pd,
			      SenderSource *src)
{
	int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
		     IBV_ACCESS_REMOTE_READ;
	hipError_t hret;

	memset(src, 0, sizeof(*src));
	src->kind = o->source_kind;
	src->fill_name = o->source_fill_name;
	src->reg_name = o->source_reg_name;
	src->dmabuf_fd = -1;
	src->payload_bytes = align_up(o->size, 4096);
	if (src->payload_bytes < 4096)
		src->payload_bytes = 4096;

	hret = alloc_source_payload(o->source_kind, src->payload_bytes, src);
	if (hret != hipSuccess) {
		fprintf(stderr, "sender payload alloc kind=%s failed: %s\n",
			o->source_kind, hipGetErrorString(hret));
		return -1;
	}
	if (src->payload_hip_alloc || src->payload_host_alloc) {
		hret = hipDeviceSynchronize();
		if (hret != hipSuccess) {
			fprintf(stderr, "sender payload init sync failed: %s\n",
				hipGetErrorString(hret));
			return -1;
		}
	}

	if (posix_memalign(&src->signal_base, 64, 64)) {
		fprintf(stderr, "sender signal alloc failed\n");
		return -1;
	}
	memset(src->signal_base, 0, 64);
	src->signal = (uint64_t *)src->signal_base;

	src->signal_mr = ibv_reg_mr(pd, src->signal_base, 64,
				    IBV_ACCESS_LOCAL_WRITE);
	if (!src->signal_mr) {
		fprintf(stderr, "sender signal ibv_reg_mr: %s\n",
			strerror(errno));
		return -1;
	}

	if (o->source_reg == SOURCE_REG_DMABUF) {
		if (src->payload_hip_vmm_alloc) {
			hipError_t export_ret;

			export_ret = hipMemExportToShareableHandle(
				&src->dmabuf_fd, src->payload_vmm_handle,
				hipMemHandleTypePosixFileDescriptor, 0);
			if (export_ret != hipSuccess) {
				fprintf(stderr,
					"sender hipMemExportToShareableHandle kind=%s failed: %s\n",
					o->source_kind,
					hipGetErrorString(export_ret));
				return -1;
			}
			src->dmabuf_offset = 0;
			src->dmabuf_fd_is_hsa = false;
		} else {
			hsa_status_t hsa_ret;

			hsa_ret = hsa_amd_portable_export_dmabuf(
				src->payload, src->payload_bytes,
				&src->dmabuf_fd, &src->dmabuf_offset);
			if (hsa_ret != HSA_STATUS_SUCCESS) {
				fprintf(stderr,
					"sender dmabuf export kind=%s failed hsa_status=%d\n",
					o->source_kind, hsa_ret);
				return -1;
			}
			src->dmabuf_fd_is_hsa = true;
		}
		src->payload_mr = ibv_reg_dmabuf_mr(
			pd, src->dmabuf_offset, src->payload_bytes,
			(uintptr_t)src->payload, src->dmabuf_fd, access);
	} else {
		src->payload_mr = ibv_reg_mr(pd, src->payload,
					     src->payload_bytes, access);
	}
	if (!src->payload_mr) {
		fprintf(stderr, "sender payload reg kind=%s method=%s: %s\n",
			o->source_kind, o->source_reg_name, strerror(errno));
		return -1;
	}

	if (o->source_fill != SOURCE_FILL_CPU) {
		hret = hipHostMalloc(reinterpret_cast<void **>(&src->fill_state),
				     sizeof(*src->fill_state),
				     hipHostMallocMapped |
					     hipHostMallocCoherent);
		if (hret != hipSuccess) {
			fprintf(stderr, "sender fill state alloc failed: %s\n",
				hipGetErrorString(hret));
			return -1;
		}
		memset(src->fill_state, 0, sizeof(*src->fill_state));
		if (o->source_fill == SOURCE_FILL_GPU_HDP ||
		    o->source_fill == SOURCE_FILL_GPU_HDP_SYNC ||
		    o->source_fill == SOURCE_FILL_GPU_HOST_HDP) {
			hret = hipDeviceGetAttribute(
				reinterpret_cast<int *>(&src->hdp_flush_ptr),
				hipDeviceAttributeHdpMemFlushCntl, 0);
			if (hret != hipSuccess || !src->hdp_flush_ptr) {
				fprintf(stderr,
					"hipDeviceAttributeHdpMemFlushCntl failed: %s ptr=%p\n",
					hipGetErrorString(hret),
					src->hdp_flush_ptr);
				return -1;
			}
		}
	}

	printf("send_source kind=%s fill=%s reg=%s payload=%p payload_bytes=%zu lkey=0x%x signal=%p signal_lkey=0x%x dmabuf_fd=%d dmabuf_offset=%" PRIu64 " hdp=%p\n",
	       src->kind, src->fill_name, src->reg_name, src->payload,
	       src->payload_bytes, src->payload_mr->lkey, src->signal,
	       src->signal_mr->lkey, src->dmabuf_fd, src->dmabuf_offset,
	       src->hdp_flush_ptr);
	return 0;
}

static void cleanup_sender_source(SenderSource *src)
{
	if (src->payload_mr && ibv_dereg_mr(src->payload_mr))
		fprintf(stderr, "sender payload ibv_dereg_mr: %s\n",
			strerror(errno));
	if (src->signal_mr && ibv_dereg_mr(src->signal_mr))
		fprintf(stderr, "sender signal ibv_dereg_mr: %s\n",
			strerror(errno));
	if (src->dmabuf_fd >= 0) {
		if (src->dmabuf_fd_is_hsa)
			(void)hsa_amd_portable_close_dmabuf(src->dmabuf_fd);
		else
			(void)close(src->dmabuf_fd);
	}
	if (src->fill_state)
		(void)hipHostFree(src->fill_state);
	if (src->payload_host_alloc)
		(void)hipHostFree(src->payload_base);
	else if (src->payload_hip_vmm_alloc) {
		(void)hipMemUnmap(src->payload_base, src->payload_vmm_bytes);
		(void)hipMemAddressFree(src->payload_base,
					src->payload_vmm_bytes);
		(void)hipMemRelease(src->payload_vmm_handle);
	}
	else if (src->payload_hsa_alloc)
		(void)hsa_amd_memory_pool_free(src->payload_base);
	else if (src->payload_hip_alloc)
		(void)hipFree(src->payload_base);
	else if (src->payload_malloc_alloc)
		free(src->payload_base);
	free(src->signal_base);
}

static int wait_source_ready(SenderSource *src, uint32_t seq, int timeout_ms)
{
	uint64_t deadline = now_ns() + (uint64_t)timeout_ms * 1000000ull;

	while (now_ns() < deadline) {
		uint32_t err = host_load_acquire_u32(&src->fill_state->error_seq);
		uint32_t ready =
			host_load_acquire_u32(&src->fill_state->ready_seq);

		if (err) {
			fprintf(stderr, "source fill error seq=%u off=%u\n",
				err, src->fill_state->error_off);
			return -1;
		}
		if (ready >= seq)
			return 0;
		usleep(50);
	}

	fprintf(stderr, "timeout waiting source_ready=%u got=%u\n", seq,
		host_load_acquire_u32(&src->fill_state->ready_seq));
	return -1;
}

static int prepare_source_payload(SenderSource *src, struct opts *o,
				  uint32_t seq)
{
	if (o->source_fill == SOURCE_FILL_CPU) {
		fill_payload((uint32_t *)src->payload, o->size, seq);
		return 0;
	}

	__atomic_store_n(&src->fill_state->ready_seq, 0, __ATOMIC_RELEASE);
	hipLaunchKernelGGL(source_fill_kernel, dim3(1), dim3(256), 0, 0,
			   src->payload, src->fill_state,
			   (uint32_t)(o->size / sizeof(uint32_t)), seq,
			   (o->source_fill == SOURCE_FILL_GPU_HDP ||
			    o->source_fill == SOURCE_FILL_GPU_HDP_SYNC) ?
				   src->hdp_flush_ptr :
				   nullptr);
	hipError_t hret = hipGetLastError();
	if (hret != hipSuccess) {
		fprintf(stderr, "source fill launch failed: %s\n",
			hipGetErrorString(hret));
		return -1;
	}
	if (wait_source_ready(src, seq, o->timeout_ms))
		return -1;
	if (o->source_fill == SOURCE_FILL_GPU_HOST_HDP) {
		__atomic_store_n(src->hdp_flush_ptr, 1u, __ATOMIC_SEQ_CST);
		__sync_synchronize();
	}
	if (o->source_fill == SOURCE_FILL_GPU_SYNC ||
	    o->source_fill == SOURCE_FILL_GPU_HDP_SYNC) {
		hret = hipDeviceSynchronize();
		if (hret != hipSuccess) {
			fprintf(stderr, "source fill sync failed: %s\n",
				hipGetErrorString(hret));
			return -1;
		}
	}
	return 0;
}

static int post_write_pair(struct ibv_qp *qp, struct ibv_mr *payload_mr,
			   struct ibv_mr *signal_mr, uint8_t *payload,
			   uint64_t *signal, size_t size,
			   uint64_t remote_payload, uint32_t remote_rkey,
			   size_t remote_signal_offset, uint32_t seq)
{
	struct ibv_sge payload_sge;
	struct ibv_sge signal_sge;
	struct ibv_send_wr payload_wr;
	struct ibv_send_wr signal_wr;
	struct ibv_send_wr *bad = NULL;

	*signal = seq;
	memset(&payload_sge, 0, sizeof(payload_sge));
	payload_sge.addr = (uintptr_t)payload;
	payload_sge.length = (uint32_t)size;
	payload_sge.lkey = payload_mr->lkey;
	memset(&signal_sge, 0, sizeof(signal_sge));
	signal_sge.addr = (uintptr_t)signal;
	signal_sge.length = sizeof(*signal);
	signal_sge.lkey = signal_mr->lkey;

	memset(&payload_wr, 0, sizeof(payload_wr));
	payload_wr.wr_id = ((uint64_t)seq << 1);
	payload_wr.sg_list = &payload_sge;
	payload_wr.num_sge = 1;
	payload_wr.opcode = IBV_WR_RDMA_WRITE;
	payload_wr.wr.rdma.remote_addr = remote_payload;
	payload_wr.wr.rdma.rkey = remote_rkey;
	payload_wr.next = &signal_wr;

	memset(&signal_wr, 0, sizeof(signal_wr));
	signal_wr.wr_id = ((uint64_t)seq << 1) | 1u;
	signal_wr.sg_list = &signal_sge;
	signal_wr.num_sge = 1;
	signal_wr.opcode = IBV_WR_RDMA_WRITE;
	signal_wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_FENCE;
	signal_wr.wr.rdma.remote_addr = remote_payload + remote_signal_offset;
	signal_wr.wr.rdma.rkey = remote_rkey;

	return ibv_post_send(qp, &payload_wr, &bad);
}

static int run_receiver(struct opts *o, int sock, HipRegion *region,
			uint32_t rkey, uint32_t qpn)
{
	char ready = 'R';
	char ack;
	hipError_t hret;
	uint64_t start_ns;
	int ret = 1;

	printf("recv_region kind=%s payload=%p signal=%p state=%p bytes=%zu signal_offset=%zu state_offset=%zu rkey=0x%x local_qpn=%u\n",
	       o->kind, region->payload, region->signal, region->state,
	       region->bytes, region->signal_offset, region->state_offset,
	       rkey, qpn);

	hret = hipMemset(region->state, 0, sizeof(*region->state));
	if (hret == hipSuccess)
		hret = hipMemset(region->payload, 0, o->size);
	if (hret == hipSuccess)
		hret = hipMemset(region->signal, 0, sizeof(*region->signal));
	if (hret == hipSuccess)
		hret = hipDeviceSynchronize();
	if (hret != hipSuccess) {
		fprintf(stderr, "receiver hip init failed: %s\n",
			hipGetErrorString(hret));
		return 1;
	}

	hipLaunchKernelGGL(rdma_write_visibility_kernel, dim3(1), dim3(256),
			   0, 0, region->payload, region->signal, region->state,
			   (uint32_t)(o->size / sizeof(uint32_t)), o->count,
			   o->mode, 100000000u);
	hret = hipGetLastError();
	if (hret != hipSuccess) {
		fprintf(stderr, "kernel launch failed: %s\n",
			hipGetErrorString(hret));
		return 1;
	}

	if (send_all(sock, &ready, 1)) {
		perror("ready send");
		goto out_sync;
	}

	start_ns = now_ns();
	for (uint32_t seq = 1; seq <= o->count; ++seq) {
		if (wait_gpu_seen(region, seq, o->timeout_ms))
			goto out_sync;
		ack = 'A';
		if (send_all(sock, &ack, 1)) {
			perror("ack send");
			goto out_sync;
		}
	}

	hret = hipDeviceSynchronize();
	if (hret != hipSuccess) {
		fprintf(stderr, "hipDeviceSynchronize failed: %s\n",
			hipGetErrorString(hret));
		goto out_sync;
	}

	{
		double secs = (double)(now_ns() - start_ns) / 1000000000.0;
		printf("recv_result kind=%s mode=%s size=%zu count=%u status=OK elapsed_sec=%.6f avg_us=%.3f gpu_spins=%" PRIu64 " prefetch_sink=0x%x\n",
		       o->kind, o->mode_name, o->size, o->count, secs,
		       secs * 1000000.0 / (double)o->count,
		       region->state->gpu_spins, region->state->prefetch_sink);
	}
	ret = 0;

out_sync:
	if (ret)
		(void)hipDeviceSynchronize();
	return ret;
}

static int run_sender(struct opts *o, int sock, struct ibv_pd *pd,
		      struct ibv_qp *qp, struct ibv_cq *cq,
		      const struct peer_info *remote)
{
	SenderSource src;
	char ready;
	char ack;
	size_t signal_offset = align_up(o->size, 64);
	uint64_t start_ns;
	int ret = 1;

	if (init_sender_source(o, pd, &src))
		return 1;

	if (recv_all(sock, &ready, 1) || ready != 'R') {
		fprintf(stderr, "receiver did not signal ready\n");
		goto out_src;
	}

	start_ns = now_ns();
	for (uint32_t seq = 1; seq <= o->count; ++seq) {
		if (prepare_source_payload(&src, o, seq))
			goto out_src;
		if (post_write_pair(qp, src.payload_mr, src.signal_mr,
				    src.payload, src.signal, o->size,
				    remote->addr, remote->rkey, signal_offset,
				    seq)) {
			perror("ibv_post_send RDMA_WRITE");
			goto out_src;
		}
		if (poll_send_cq(cq, o->timeout_ms))
			goto out_src;
		if (recv_all(sock, &ack, 1) || ack != 'A') {
			fprintf(stderr, "receiver did not ack seq=%u\n", seq);
			goto out_src;
		}
	}

	{
		double secs = (double)(now_ns() - start_ns) / 1000000000.0;
		printf("send_result source_kind=%s source_fill=%s source_reg=%s size=%zu count=%u status=OK elapsed_sec=%.6f avg_us=%.3f remote_addr=0x%016" PRIx64 " remote_rkey=0x%x\n",
		       o->source_kind, o->source_fill_name, o->source_reg_name,
		       o->size, o->count, secs,
		       secs * 1000000.0 / (double)o->count,
		       remote->addr, remote->rkey);
	}
	ret = 0;

out_src:
	cleanup_sender_source(&src);
	return ret;
}

int main(int argc, char **argv)
{
	struct opts o;
	struct ibv_context *ctx = NULL;
	struct ibv_pd *pd = NULL;
	struct ibv_cq *cq = NULL;
	struct ibv_qp *qp = NULL;
	struct ibv_qp_init_attr qp_attr;
	struct ibv_mr *recv_mr = NULL;
	HipRegion recv_region = {};
	bool recv_region_allocated = false;
	union ibv_gid local_gid, remote_gid;
	struct ibv_port_attr port_attr;
	struct peer_info local, remote;
	uint32_t psn;
	int listen_fd = -1;
	int sock = -1;
	int ret = 1;

	if (parse_opts(argc, argv, &o)) {
		usage(argv[0]);
		return 2;
	}
	if (!strcmp(o.role, "send") && !o.connect_host) {
		usage(argv[0]);
		return 2;
	}
	if (hipSetDevice(0) != hipSuccess) {
		fprintf(stderr, "hipSetDevice(0) failed\n");
		return 1;
	}

	ctx = open_dev(o.dev);
	if (!ctx) {
		fprintf(stderr, "ibv_open_device(%s): %s\n", o.dev,
			strerror(errno));
		goto out;
	}
	if (ibv_query_port(ctx, o.ib_port, &port_attr)) {
		perror("ibv_query_port");
		goto out;
	}
	if (ibv_query_gid(ctx, o.ib_port, o.gid_index, &local_gid)) {
		perror("ibv_query_gid");
		goto out;
	}
	pd = ibv_alloc_pd(ctx);
	if (!pd) {
		perror("ibv_alloc_pd");
		goto out;
	}
	cq = ibv_create_cq(ctx, 32, NULL, NULL, 0);
	if (!cq) {
		perror("ibv_create_cq");
		goto out;
	}
	memset(&qp_attr, 0, sizeof(qp_attr));
	qp_attr.send_cq = cq;
	qp_attr.recv_cq = cq;
	qp_attr.qp_type = IBV_QPT_RC;
	qp_attr.cap.max_send_wr = 32;
	qp_attr.cap.max_recv_wr = 1;
	qp_attr.cap.max_send_sge = 1;
	qp_attr.cap.max_recv_sge = 1;
	qp = ibv_create_qp(pd, &qp_attr);
	if (!qp) {
		perror("ibv_create_qp");
		goto out;
	}

	psn = (uint32_t)(now_ns() & 0xffffffu);
	memset(&local, 0, sizeof(local));
	local.magic = VIS_MAGIC;
	local.qpn = qp->qp_num;
	local.psn = psn;
	local.lid = port_attr.lid;
	memcpy(local.gid, local_gid.raw, sizeof(local.gid));

	if (!strcmp(o.role, "recv")) {
		hipError_t hret = alloc_region(o.kind, o.size, &recv_region);

		if (hret != hipSuccess) {
			fprintf(stderr, "receiver region alloc failed: %s\n",
				hipGetErrorString(hret));
			goto out;
		}
		recv_region_allocated = true;
		recv_mr = ibv_reg_mr(pd, recv_region.payload,
				     recv_region.state_offset +
					     sizeof(*recv_region.state),
				     IBV_ACCESS_LOCAL_WRITE |
					     IBV_ACCESS_REMOTE_WRITE);
		if (!recv_mr) {
			fprintf(stderr, "receiver ibv_reg_mr: %s\n",
				strerror(errno));
			goto out;
		}
		local.addr = (uintptr_t)recv_region.payload;
		local.rkey = recv_mr->rkey;

		listen_fd = tcp_listen(o.port);
		if (listen_fd < 0) {
			perror("tcp_listen");
			goto out;
		}
		printf("recv_listen port=%d kind=%s mode=%s size=%zu count=%u qpn=%u\n",
		       o.port, o.kind, o.mode_name, o.size, o.count,
		       local.qpn);
		sock = accept(listen_fd, NULL, NULL);
		if (sock < 0) {
			perror("accept");
			goto out;
		}
	} else {
		sock = tcp_connect(o.connect_host, o.port);
		if (sock < 0) {
			perror("tcp_connect");
			goto out;
		}
		printf("send_connect host=%s port=%d size=%zu count=%u qpn=%u\n",
		       o.connect_host, o.port, o.size, o.count, local.qpn);
	}

	if (exchange_info(sock, &local, &remote))
		goto out;
	memcpy(remote_gid.raw, remote.gid, sizeof(remote_gid.raw));
	if (qp_to_rts(qp, o.ib_port, o.gid_index, &remote_gid, remote.lid,
		      remote.qpn, local.psn, remote.psn, mtu_enum(o.mtu),
		      IBV_ACCESS_REMOTE_WRITE))
		goto out;

	if (!strcmp(o.role, "recv"))
		ret = run_receiver(&o, sock, &recv_region, recv_mr->rkey,
				   qp->qp_num);
	else
		ret = run_sender(&o, sock, pd, qp, cq, &remote);

out:
	if (recv_mr && ibv_dereg_mr(recv_mr))
		fprintf(stderr, "receiver ibv_dereg_mr: %s\n", strerror(errno));
	if (recv_region_allocated)
		free_region(&recv_region);
	if (sock >= 0)
		close(sock);
	if (listen_fd >= 0)
		close(listen_fd);
	if (qp && ibv_destroy_qp(qp))
		fprintf(stderr, "ibv_destroy_qp: %s\n", strerror(errno));
	if (cq && ibv_destroy_cq(cq))
		fprintf(stderr, "ibv_destroy_cq: %s\n", strerror(errno));
	if (pd && ibv_dealloc_pd(pd))
		fprintf(stderr, "ibv_dealloc_pd: %s\n", strerror(errno));
	if (ctx && ibv_close_device(ctx))
		fprintf(stderr, "ibv_close_device: %s\n", strerror(errno));
	return ret;
}
