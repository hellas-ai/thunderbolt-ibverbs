// SPDX-License-Identifier: MIT
/*
 * Probe HSA fine-grained memory pools for the USB4 GDA queue-memory contract.
 *
 * The ABI can use HSA fine-grained allocations only if their runtime alignment
 * is compatible with the SQ/CQ/doorbell layout and if usb4_rdma can register
 * them as MRs. This program prints pool properties and attempts ibv_reg_mr()
 * for candidate fine-grained allocations.
 */

#include <errno.h>
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>
#include <infiniband/verbs.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vector>

struct AgentInfo {
	hsa_agent_t agent;
	char name[64];
	hsa_device_type_t type;
};

struct PoolInfo {
	hsa_amd_memory_pool_t pool;
	size_t owner_index;
	hsa_amd_segment_t segment;
	uint32_t flags;
	bool alloc_allowed;
	bool accessible_by_all;
	hsa_amd_memory_pool_location_t location;
	size_t alloc_granule;
	size_t rec_granule;
	size_t alloc_alignment;
	size_t max_size;
};

struct PoolEnumCtx {
	const std::vector<AgentInfo> *agents;
	size_t owner_index;
	std::vector<PoolInfo> *pools;
};

static const char *hsa_status_name(hsa_status_t status)
{
	const char *name = NULL;

	if (hsa_status_string(status, &name) != HSA_STATUS_SUCCESS || !name)
		return "unknown";
	return name;
}

static const char *agent_type_name(hsa_device_type_t type)
{
	switch (type) {
	case HSA_DEVICE_TYPE_CPU:
		return "cpu";
	case HSA_DEVICE_TYPE_GPU:
		return "gpu";
	case HSA_DEVICE_TYPE_DSP:
		return "dsp";
	case HSA_DEVICE_TYPE_AIE:
		return "aie";
	default:
		return "unknown";
	}
}

static const char *segment_name(hsa_amd_segment_t segment)
{
	switch (segment) {
	case HSA_AMD_SEGMENT_GLOBAL:
		return "global";
	case HSA_AMD_SEGMENT_READONLY:
		return "readonly";
	case HSA_AMD_SEGMENT_PRIVATE:
		return "private";
	case HSA_AMD_SEGMENT_GROUP:
		return "group";
	default:
		return "unknown";
	}
}

static const char *location_name(hsa_amd_memory_pool_location_t location)
{
	switch (location) {
	case HSA_AMD_MEMORY_POOL_LOCATION_CPU:
		return "cpu";
	case HSA_AMD_MEMORY_POOL_LOCATION_GPU:
		return "gpu";
	default:
		return "unknown";
	}
}

static const char *access_name(hsa_amd_memory_pool_access_t access)
{
	switch (access) {
	case HSA_AMD_MEMORY_POOL_ACCESS_NEVER_ALLOWED:
		return "never";
	case HSA_AMD_MEMORY_POOL_ACCESS_ALLOWED_BY_DEFAULT:
		return "default";
	case HSA_AMD_MEMORY_POOL_ACCESS_DISALLOWED_BY_DEFAULT:
		return "allow_access";
	default:
		return "unknown";
	}
}

static bool pool_is_candidate(const PoolInfo &pool)
{
	if (pool.segment != HSA_AMD_SEGMENT_GLOBAL || !pool.alloc_allowed)
		return false;
	return pool.flags & (HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_FINE_GRAINED |
			     HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_EXTENDED_SCOPE_FINE_GRAINED);
}

static size_t round_up(size_t value, size_t granule)
{
	if (!granule)
		return value;
	size_t rem = value % granule;
	return rem ? value + granule - rem : value;
}

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

static hsa_status_t collect_agent(hsa_agent_t agent, void *data)
{
	std::vector<AgentInfo> *agents =
		static_cast<std::vector<AgentInfo> *>(data);
	AgentInfo info = {};
	hsa_status_t status;

	info.agent = agent;
	status = hsa_agent_get_info(agent, HSA_AGENT_INFO_NAME, info.name);
	if (status != HSA_STATUS_SUCCESS)
		snprintf(info.name, sizeof(info.name), "unknown");
	info.name[sizeof(info.name) - 1] = '\0';

	status = hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &info.type);
	if (status != HSA_STATUS_SUCCESS)
		info.type = static_cast<hsa_device_type_t>(-1);

	agents->push_back(info);
	return HSA_STATUS_SUCCESS;
}

static hsa_status_t collect_pool(hsa_amd_memory_pool_t pool, void *data)
{
	PoolEnumCtx *ctx = static_cast<PoolEnumCtx *>(data);
	PoolInfo info = {};
	hsa_status_t status;

	info.pool = pool;
	info.owner_index = ctx->owner_index;

	status = hsa_amd_memory_pool_get_info(
		pool, HSA_AMD_MEMORY_POOL_INFO_SEGMENT, &info.segment);
	if (status != HSA_STATUS_SUCCESS) {
		fprintf(stderr, "pool=0x%016" PRIx64 " query=segment status=%s\n",
			pool.handle, hsa_status_name(status));
		return HSA_STATUS_SUCCESS;
	}

	if (info.segment == HSA_AMD_SEGMENT_GLOBAL) {
		status = hsa_amd_memory_pool_get_info(
			pool, HSA_AMD_MEMORY_POOL_INFO_GLOBAL_FLAGS,
			&info.flags);
		if (status != HSA_STATUS_SUCCESS)
			info.flags = 0;
	}

	status = hsa_amd_memory_pool_get_info(
		pool, HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_ALLOWED,
		&info.alloc_allowed);
	if (status != HSA_STATUS_SUCCESS)
		info.alloc_allowed = false;

	status = hsa_amd_memory_pool_get_info(
		pool, HSA_AMD_MEMORY_POOL_INFO_ACCESSIBLE_BY_ALL,
		&info.accessible_by_all);
	if (status != HSA_STATUS_SUCCESS)
		info.accessible_by_all = false;

	status = hsa_amd_memory_pool_get_info(
		pool, HSA_AMD_MEMORY_POOL_INFO_LOCATION, &info.location);
	if (status != HSA_STATUS_SUCCESS)
		info.location = static_cast<hsa_amd_memory_pool_location_t>(-1);

	if (info.alloc_allowed) {
		(void)hsa_amd_memory_pool_get_info(
			pool, HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_GRANULE,
			&info.alloc_granule);
		(void)hsa_amd_memory_pool_get_info(
			pool, HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_REC_GRANULE,
			&info.rec_granule);
		(void)hsa_amd_memory_pool_get_info(
			pool, HSA_AMD_MEMORY_POOL_INFO_RUNTIME_ALLOC_ALIGNMENT,
			&info.alloc_alignment);
		(void)hsa_amd_memory_pool_get_info(
			pool, HSA_AMD_MEMORY_POOL_INFO_ALLOC_MAX_SIZE,
			&info.max_size);
	}

	ctx->pools->push_back(info);
	return HSA_STATUS_SUCCESS;
}

static void print_pool_access(const PoolInfo &pool,
			      const std::vector<AgentInfo> &agents)
{
	for (size_t i = 0; i < agents.size(); i++) {
		hsa_amd_memory_pool_access_t access =
			HSA_AMD_MEMORY_POOL_ACCESS_NEVER_ALLOWED;
		hsa_status_t status = hsa_amd_agent_memory_pool_get_info(
			agents[i].agent, pool.pool,
			HSA_AMD_AGENT_MEMORY_POOL_INFO_ACCESS, &access);

		printf("pool_access pool=0x%016" PRIx64 " agent=%zu name=\"%s\" access=%s status=%s\n",
		       pool.pool.handle, i, agents[i].name,
		       status == HSA_STATUS_SUCCESS ? access_name(access) : "query-fail",
		       hsa_status_name(status));
	}
}

static void probe_allocation(const PoolInfo &pool,
			     const std::vector<AgentInfo> &agents,
			     struct ibv_pd *pd, size_t requested_size)
{
	std::vector<hsa_agent_t> allowed_agents;
	void *ptr = NULL;
	size_t size = round_up(requested_size, pool.alloc_granule);
	hsa_status_t status;

	for (const AgentInfo &agent : agents) {
		hsa_amd_memory_pool_access_t access =
			HSA_AMD_MEMORY_POOL_ACCESS_NEVER_ALLOWED;

		if (agent.type != HSA_DEVICE_TYPE_CPU &&
		    agent.type != HSA_DEVICE_TYPE_GPU)
			continue;

		status = hsa_amd_agent_memory_pool_get_info(
			agent.agent, pool.pool,
			HSA_AMD_AGENT_MEMORY_POOL_INFO_ACCESS, &access);
		if (status == HSA_STATUS_SUCCESS &&
		    access != HSA_AMD_MEMORY_POOL_ACCESS_NEVER_ALLOWED)
			allowed_agents.push_back(agent.agent);
	}

	status = hsa_amd_memory_pool_allocate(pool.pool, size, 0, &ptr);
	if (status != HSA_STATUS_SUCCESS) {
		printf("alloc_probe pool=0x%016" PRIx64 " owner=%zu requested=%zu size=%zu alloc=FAIL status=%s\n",
		       pool.pool.handle, pool.owner_index, requested_size, size,
		       hsa_status_name(status));
		return;
	}

	status = hsa_amd_agents_allow_access(
		static_cast<uint32_t>(allowed_agents.size()),
		allowed_agents.data(), NULL, ptr);

	uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
	printf("alloc_probe pool=0x%016" PRIx64 " owner=%zu grain=fine extended=%u ptr=%p requested=%zu size=%zu alloc_alignment=%zu ptr_mod_alignment=%zu ptr_mod_4096=%zu allow_scope=cpu_gpu allow_agents=%zu allow_access=%s",
	       pool.pool.handle, pool.owner_index,
	       !!(pool.flags &
		  HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_EXTENDED_SCOPE_FINE_GRAINED),
	       ptr, requested_size, size, pool.alloc_alignment,
	       pool.alloc_alignment ? addr % pool.alloc_alignment : 0,
	       addr % 4096, allowed_agents.size(), hsa_status_name(status));

	if (pd) {
		int access = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
			     IBV_ACCESS_REMOTE_READ;
		errno = 0;
		struct ibv_mr *mr = ibv_reg_mr(pd, ptr, size, access);
		if (mr) {
			printf(" reg_mr=OK lkey=0x%x rkey=0x%x\n", mr->lkey,
			       mr->rkey);
			(void)ibv_dereg_mr(mr);
		} else {
			printf(" reg_mr=FAIL errno=%d %s\n", errno,
			       strerror(errno));
		}

		int dmabuf_fd = -1;
		uint64_t dmabuf_offset = 0;
		status = hsa_amd_portable_export_dmabuf(ptr, size, &dmabuf_fd,
							&dmabuf_offset);
		if (status != HSA_STATUS_SUCCESS) {
			printf(" dmabuf_export=FAIL status=%s\n",
			       hsa_status_name(status));
		} else {
			errno = 0;
			mr = ibv_reg_dmabuf_mr(pd, dmabuf_offset, size,
					       reinterpret_cast<uintptr_t>(ptr),
					       dmabuf_fd, access);
			if (mr) {
				printf(" dmabuf_export=OK fd=%d offset=%" PRIu64 " reg_dmabuf=OK lkey=0x%x rkey=0x%x\n",
				       dmabuf_fd, dmabuf_offset, mr->lkey,
				       mr->rkey);
				(void)ibv_dereg_mr(mr);
			} else {
				printf(" dmabuf_export=OK fd=%d offset=%" PRIu64 " reg_dmabuf=FAIL errno=%d %s\n",
				       dmabuf_fd, dmabuf_offset, errno,
				       strerror(errno));
			}
			(void)hsa_amd_portable_close_dmabuf(dmabuf_fd);
		}
	} else {
		printf(" reg_mr=SKIP\n");
	}

	(void)hsa_amd_memory_pool_free(ptr);
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "usb4_rdma0";
	size_t requested_size = argc > 2 ? strtoull(argv[2], NULL, 0) : 4096;
	std::vector<AgentInfo> agents;
	std::vector<PoolInfo> pools;
	struct ibv_context *verbs = NULL;
	struct ibv_pd *pd = NULL;
	hsa_status_t status;

	status = hsa_init();
	if (status != HSA_STATUS_SUCCESS) {
		fprintf(stderr, "hsa_init failed: %s\n", hsa_status_name(status));
		return 1;
	}

	status = hsa_iterate_agents(collect_agent, &agents);
	if (status != HSA_STATUS_SUCCESS) {
		fprintf(stderr, "hsa_iterate_agents failed: %s\n",
			hsa_status_name(status));
		(void)hsa_shut_down();
		return 1;
	}

	printf("requested_size=%zu rdma_device=%s agents=%zu\n", requested_size,
	       dev, agents.size());
	for (size_t i = 0; i < agents.size(); i++) {
		printf("agent index=%zu handle=0x%016" PRIx64 " type=%s name=\"%s\"\n",
		       i, agents[i].agent.handle, agent_type_name(agents[i].type),
		       agents[i].name);

		PoolEnumCtx ctx = { &agents, i, &pools };
		status = hsa_amd_agent_iterate_memory_pools(agents[i].agent,
							    collect_pool, &ctx);
		if (status != HSA_STATUS_SUCCESS)
			fprintf(stderr,
				"agent=%zu iterate_memory_pools failed: %s\n",
				i, hsa_status_name(status));
	}

	if (strcmp(dev, "none")) {
		verbs = open_context(dev);
		if (!verbs) {
			fprintf(stderr, "open_context(%s) failed; skipping reg_mr\n",
				dev);
		} else {
			pd = ibv_alloc_pd(verbs);
			if (!pd)
				perror("ibv_alloc_pd");
		}
	}

	for (const PoolInfo &pool : pools) {
		const AgentInfo &owner = agents[pool.owner_index];
		bool fine = pool.flags & HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_FINE_GRAINED;
		bool extended = pool.flags &
				HSA_AMD_MEMORY_POOL_GLOBAL_FLAG_EXTENDED_SCOPE_FINE_GRAINED;

		printf("pool owner=%zu owner_name=\"%s\" owner_type=%s handle=0x%016" PRIx64 " segment=%s location=%s flags=0x%08x fine=%u extended=%u alloc_allowed=%u alloc_granule=%zu rec_granule=%zu alloc_alignment=%zu accessible_by_all=%u max_size=%zu candidate=%u\n",
		       pool.owner_index, owner.name, agent_type_name(owner.type),
		       pool.pool.handle, segment_name(pool.segment),
		       location_name(pool.location), pool.flags, fine, extended,
		       pool.alloc_allowed, pool.alloc_granule, pool.rec_granule,
		       pool.alloc_alignment, pool.accessible_by_all, pool.max_size,
		       pool_is_candidate(pool));
		print_pool_access(pool, agents);

		if (pool_is_candidate(pool))
			probe_allocation(pool, agents, pd, requested_size);
	}

	if (pd)
		(void)ibv_dealloc_pd(pd);
	if (verbs)
		(void)ibv_close_device(verbs);
	(void)hsa_shut_down();
	return 0;
}
