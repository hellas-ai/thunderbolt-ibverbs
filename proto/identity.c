// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include "proto/identity.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

static void tbv_id_selection_clear(struct tbv_id_selection *selection)
{
	if (selection)
		memset(selection, 0, sizeof(*selection));
}

void tbv_id_addr_v4(struct tbv_id_addr *addr, tbv_id_u8 a, tbv_id_u8 b,
		    tbv_id_u8 c, tbv_id_u8 d)
{
	memset(addr, 0, sizeof(*addr));
	addr->family = TBV_ID_AF_INET;
	addr->bytes[12] = a;
	addr->bytes[13] = b;
	addr->bytes[14] = c;
	addr->bytes[15] = d;
}

void tbv_id_addr_v6(struct tbv_id_addr *addr,
		    const tbv_id_u8 bytes[TBV_ID_ADDR_BYTES])
{
	memset(addr, 0, sizeof(*addr));
	addr->family = TBV_ID_AF_INET6;
	memcpy(addr->bytes, bytes, TBV_ID_ADDR_BYTES);
}

bool tbv_id_addr_equal(const struct tbv_id_addr *a,
		       const struct tbv_id_addr *b)
{
	return a->family == b->family &&
	       memcmp(a->bytes, b->bytes, TBV_ID_ADDR_BYTES) == 0;
}

void tbv_id_nccl_policy_default(struct tbv_id_nccl_policy *policy)
{
	memset(policy, 0, sizeof(*policy));
	policy->family = TBV_ID_AF_INET;
	policy->roce_version = 2;
}

static bool tbv_id_gid_is_roce(const struct tbv_id_gid *gid)
{
	return gid->gid_type == TBV_ID_GID_ROCE_V1 ||
	       gid->gid_type == TBV_ID_GID_ROCE_V2;
}

static tbv_id_u8 tbv_id_gid_roce_version(const struct tbv_id_gid *gid)
{
	if (gid->gid_type == TBV_ID_GID_ROCE_V1)
		return 1;
	if (gid->gid_type == TBV_ID_GID_ROCE_V2)
		return 2;
	return 0;
}

static bool tbv_id_addr_is_zero(const struct tbv_id_addr *addr)
{
	tbv_id_u32 i;

	for (i = 0; i < TBV_ID_ADDR_BYTES; i++) {
		if (addr->bytes[i])
			return false;
	}
	return true;
}

static bool tbv_id_addr_is_link_local_v6(const struct tbv_id_addr *addr)
{
	return addr->family == TBV_ID_AF_INET6 &&
	       addr->bytes[0] == 0xfe &&
	       (addr->bytes[1] & 0xc0u) == 0x80u;
}

static bool tbv_id_gid_is_global_roce(const struct tbv_id_gid *gid)
{
	return tbv_id_gid_is_roce(gid) &&
	       gid->addr.family != TBV_ID_AF_NONE &&
	       !tbv_id_addr_is_zero(&gid->addr) &&
	       !tbv_id_addr_is_link_local_v6(&gid->addr);
}

static bool tbv_id_same_endpoint(const struct tbv_id_gid *a,
				 const struct tbv_id_gid *b)
{
	return a->device_id == b->device_id && a->port == b->port;
}

static void tbv_id_select_gid(struct tbv_id_selection *selection,
			      const struct tbv_id_gid *gid)
{
	selection->valid = true;
	selection->device_id = gid->device_id;
	selection->port = gid->port;
	selection->gid_index = gid->gid_index;
	selection->gid_type = gid->gid_type;
	selection->addr = gid->addr;
}

static bool tbv_id_prefix_match_bytes(const tbv_id_u8 *addr,
				      const tbv_id_u8 *prefix,
				      tbv_id_u8 bits, tbv_id_u8 max_bits)
{
	tbv_id_u8 full_bytes;
	tbv_id_u8 rem_bits;

	if (bits > max_bits)
		return false;

	full_bytes = bits / 8u;
	rem_bits = bits % 8u;

	if (full_bytes &&
	    memcmp(addr, prefix, full_bytes) != 0)
		return false;
	if (rem_bits) {
		tbv_id_u8 mask = (tbv_id_u8)(0xffu << (8u - rem_bits));

		if ((addr[full_bytes] & mask) !=
		    (prefix[full_bytes] & mask))
			return false;
	}

	return true;
}

static bool tbv_id_gid_matches_range(const struct tbv_id_gid *gid,
				     const struct tbv_id_nccl_policy *policy)
{
	if (!policy->has_addr_range)
		return true;
	if (gid->addr.family != policy->family ||
	    policy->addr_range.family != policy->family)
		return false;

	if (policy->family == TBV_ID_AF_INET)
		return tbv_id_prefix_match_bytes(&gid->addr.bytes[12],
						 &policy->addr_range.bytes[12],
						 policy->addr_range_bits, 32);
	if (policy->family == TBV_ID_AF_INET6)
		return tbv_id_prefix_match_bytes(gid->addr.bytes,
						 policy->addr_range.bytes,
						 policy->addr_range_bits, 128);
	return false;
}

int tbv_id_select_llama(const struct tbv_id_gid *gids, tbv_id_u32 gid_count,
			const struct tbv_id_route *route,
			struct tbv_id_selection *selection)
{
	const struct tbv_id_gid *best = NULL;
	tbv_id_u32 i;

	tbv_id_selection_clear(selection);
	for (i = 0; i < gid_count; i++) {
		const struct tbv_id_gid *gid = &gids[i];

		if (!tbv_id_gid_is_roce(gid) ||
		    !tbv_id_addr_equal(&gid->addr, &route->local_src_addr))
			continue;

		if (!best) {
			best = gid;
			continue;
		}

		if (tbv_id_same_endpoint(best, gid)) {
			if (best->gid_type != TBV_ID_GID_ROCE_V2 &&
			    gid->gid_type == TBV_ID_GID_ROCE_V2)
				best = gid;
			continue;
		}

		return -EEXIST;
	}

	if (!best)
		return -ENODEV;

	tbv_id_select_gid(selection, best);
	return 0;
}

int tbv_id_select_nccl(const struct tbv_id_gid *gids, tbv_id_u32 gid_count,
		       const struct tbv_id_nccl_policy *policy,
		       struct tbv_id_selection *selection)
{
	const struct tbv_id_gid *best = NULL;
	tbv_id_u32 i;

	tbv_id_selection_clear(selection);
	for (i = 0; i < gid_count; i++) {
		const struct tbv_id_gid *gid = &gids[i];

		if (!tbv_id_gid_is_global_roce(gid) ||
		    gid->addr.family != policy->family ||
		    tbv_id_gid_roce_version(gid) != policy->roce_version ||
		    !tbv_id_gid_matches_range(gid, policy))
			continue;

		if (!best) {
			best = gid;
			continue;
		}

		if (tbv_id_same_endpoint(best, gid))
			continue;

		return -EEXIST;
	}

	if (!best)
		return -ENODEV;

	tbv_id_select_gid(selection, best);
	return 0;
}

int tbv_id_validate_app_compat(const struct tbv_id_gid *gids,
			       tbv_id_u32 gid_count,
			       const struct tbv_id_route *route,
			       const struct tbv_id_nccl_policy *policy,
			       struct tbv_id_selection *selection)
{
	struct tbv_id_selection llama;
	struct tbv_id_selection nccl;
	int ret;

	tbv_id_selection_clear(selection);

	ret = tbv_id_select_llama(gids, gid_count, route, &llama);
	if (ret)
		return ret;

	ret = tbv_id_select_nccl(gids, gid_count, policy, &nccl);
	if (ret)
		return ret;

	if (llama.device_id != nccl.device_id || llama.port != nccl.port ||
	    llama.gid_index != nccl.gid_index)
		return -EXDEV;

	if (selection)
		*selection = llama;
	return 0;
}
