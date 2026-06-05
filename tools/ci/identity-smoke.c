// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include <stdio.h>

#include "proto/identity.h"

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, \
				__LINE__, #cond);                               \
			return 1;                                              \
		}                                                              \
	} while (0)

static struct tbv_id_gid mk_gid(tbv_id_u32 dev, tbv_id_u8 port,
				tbv_id_u8 index, tbv_id_u8 type,
				tbv_id_u8 a, tbv_id_u8 b, tbv_id_u8 c,
				tbv_id_u8 d)
{
	struct tbv_id_gid gid = {
		.device_id = dev,
		.port = port,
		.gid_index = index,
		.gid_type = type,
	};

	tbv_id_addr_v4(&gid.addr, a, b, c, d);
	return gid;
}

static struct tbv_id_route mk_route(tbv_id_u8 peer_a, tbv_id_u8 peer_b,
				    tbv_id_u8 peer_c, tbv_id_u8 peer_d,
				    tbv_id_u8 src_a, tbv_id_u8 src_b,
				    tbv_id_u8 src_c, tbv_id_u8 src_d)
{
	struct tbv_id_route route;

	tbv_id_addr_v4(&route.peer_addr, peer_a, peer_b, peer_c, peer_d);
	tbv_id_addr_v4(&route.local_src_addr, src_a, src_b, src_c, src_d);
	return route;
}

static int test_single_canonical_link_accepts_apps(void)
{
	struct tbv_id_gid gids[] = {
		mk_gid(4, 1, 1, TBV_ID_GID_ROCE_V2, 10, 0, 4, 2),
	};
	struct tbv_id_route route = mk_route(10, 0, 5, 2, 10, 0, 4, 2);
	struct tbv_id_nccl_policy nccl;
	struct tbv_id_selection sel;

	tbv_id_nccl_policy_default(&nccl);
	CHECK(tbv_id_validate_app_compat(gids, 1, &route, &nccl, &sel) == 0);
	CHECK(sel.valid);
	CHECK(sel.device_id == 4);
	CHECK(sel.gid_index == 1);

	return 0;
}

static int test_current_bridge_shape_is_ambiguous(void)
{
	struct tbv_id_gid gids[] = {
		mk_gid(4, 1, 1, TBV_ID_GID_ROCE_V2, 10, 0, 4, 2),
		mk_gid(5, 1, 1, TBV_ID_GID_ROCE_V2, 192, 168, 23, 136),
		mk_gid(6, 1, 1, TBV_ID_GID_ROCE_V2, 192, 168, 23, 136),
	};
	struct tbv_id_route route =
		mk_route(192, 168, 23, 192, 192, 168, 23, 136);
	struct tbv_id_nccl_policy nccl;
	struct tbv_id_selection sel;

	tbv_id_nccl_policy_default(&nccl);
	CHECK(tbv_id_select_llama(gids, 3, &route, &sel) == -EEXIST);
	CHECK(tbv_id_select_nccl(gids, 3, &nccl, &sel) == -EEXIST);
	CHECK(tbv_id_validate_app_compat(gids, 3, &route, &nccl, &sel) ==
	      -EEXIST);

	return 0;
}

static int test_nccl_addr_range_is_not_enough_for_llama(void)
{
	struct tbv_id_gid gids[] = {
		mk_gid(4, 1, 1, TBV_ID_GID_ROCE_V2, 10, 0, 4, 2),
		mk_gid(5, 1, 1, TBV_ID_GID_ROCE_V2, 192, 168, 23, 136),
	};
	struct tbv_id_route route = mk_route(10, 0, 5, 2, 192, 168, 23, 136);
	struct tbv_id_nccl_policy nccl;
	struct tbv_id_selection sel;

	tbv_id_nccl_policy_default(&nccl);
	tbv_id_addr_v4(&nccl.addr_range, 10, 0, 0, 0);
	nccl.has_addr_range = true;
	nccl.addr_range_bits = 8;

	CHECK(tbv_id_select_nccl(gids, 2, &nccl, &sel) == 0);
	CHECK(sel.device_id == 4);
	CHECK(tbv_id_select_llama(gids, 2, &route, &sel) == 0);
	CHECK(sel.device_id == 5);
	CHECK(tbv_id_validate_app_compat(gids, 2, &route, &nccl, &sel) ==
	      -EXDEV);

	return 0;
}

static int test_duplicate_gid_on_same_endpoint_prefers_rocev2(void)
{
	struct tbv_id_gid gids[] = {
		mk_gid(4, 1, 0, TBV_ID_GID_ROCE_V1, 10, 0, 4, 2),
		mk_gid(4, 1, 1, TBV_ID_GID_ROCE_V2, 10, 0, 4, 2),
	};
	struct tbv_id_route route = mk_route(10, 0, 5, 2, 10, 0, 4, 2);
	struct tbv_id_selection sel;

	CHECK(tbv_id_select_llama(gids, 2, &route, &sel) == 0);
	CHECK(sel.gid_index == 1);
	CHECK(sel.gid_type == TBV_ID_GID_ROCE_V2);

	return 0;
}

static int test_app_compat_rejects_gid_type_mismatch(void)
{
	struct tbv_id_gid gids[] = {
		mk_gid(4, 1, 1, TBV_ID_GID_ROCE_V1, 10, 0, 4, 2),
		mk_gid(4, 1, 1, TBV_ID_GID_ROCE_V2, 10, 0, 4, 2),
	};
	struct tbv_id_route route = mk_route(10, 0, 5, 2, 10, 0, 4, 2);
	struct tbv_id_nccl_policy nccl;
	struct tbv_id_selection sel;

	tbv_id_nccl_policy_default(&nccl);
	nccl.roce_version = 1;
	CHECK(tbv_id_validate_app_compat(gids, 2, &route, &nccl, &sel) ==
	      -EXDEV);

	return 0;
}

static int test_nccl_requires_global_rocev2_by_default(void)
{
	static const tbv_id_u8 link_local[TBV_ID_ADDR_BYTES] = {
		0xfe, 0x80,
	};
	struct tbv_id_gid gids[] = {
		mk_gid(1, 1, 0, TBV_ID_GID_ROCE_V2, 0, 0, 0, 0),
		mk_gid(2, 1, 0, TBV_ID_GID_ROCE_V1, 10, 0, 4, 2),
		mk_gid(3, 1, 0, TBV_ID_GID_ROCE_V2, 10, 0, 4, 2),
	};
	struct tbv_id_nccl_policy nccl;
	struct tbv_id_selection sel;

	tbv_id_addr_v6(&gids[0].addr, link_local);
	tbv_id_nccl_policy_default(&nccl);

	CHECK(tbv_id_select_nccl(gids, 3, &nccl, &sel) == 0);
	CHECK(sel.device_id == 3);

	return 0;
}

int main(void)
{
	CHECK(test_single_canonical_link_accepts_apps() == 0);
	CHECK(test_current_bridge_shape_is_ambiguous() == 0);
	CHECK(test_nccl_addr_range_is_not_enough_for_llama() == 0);
	CHECK(test_duplicate_gid_on_same_endpoint_prefers_rocev2() == 0);
	CHECK(test_app_compat_rejects_gid_type_mismatch() == 0);
	CHECK(test_nccl_requires_global_rocev2_by_default() == 0);

	puts("identity smoke OK");
	return 0;
}
