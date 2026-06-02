// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include <stdio.h>

#include "proto/config.h"

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

static int configure_canonical(struct tbv_cfg_link *link, tbv_id_u8 backend)
{
	struct tbv_id_gid gids[] = {
		mk_gid(4, 1, 1, TBV_ID_GID_ROCE_V2, 10, 0, 4, 2),
	};
	struct tbv_id_route route = mk_route(10, 0, 5, 2, 10, 0, 4, 2);

	CHECK(tbv_cfg_link_set_backend(link, backend) == 0);
	CHECK(tbv_cfg_link_set_route(link, &route) == 0);
	CHECK(tbv_cfg_link_set_app_gids(link, gids, 1) == 0);

	return 0;
}

static int test_canonical_native_activates(void)
{
	struct tbv_cfg_link link;

	tbv_cfg_link_init(&link, 1);
	CHECK(configure_canonical(&link, TBV_CFG_BACKEND_NATIVE) == 0);
	CHECK(tbv_cfg_link_seal(&link) == 0);
	CHECK(link.state == TBV_CFG_SEALED);
	CHECK(link.app_selection.device_id == 4);
	CHECK(tbv_cfg_link_activate(&link) == 0);
	CHECK(link.state == TBV_CFG_ACTIVE);

	return 0;
}

static int test_canonical_apple_uses_same_identity_gate(void)
{
	struct tbv_cfg_link link;

	tbv_cfg_link_init(&link, 2);
	CHECK(configure_canonical(&link, TBV_CFG_BACKEND_APPLE) == 0);
	CHECK(tbv_cfg_link_seal(&link) == 0);
	CHECK(link.backend == TBV_CFG_BACKEND_APPLE);
	CHECK(link.app_selection.device_id == 4);

	return 0;
}

static int test_partial_links_cannot_activate(void)
{
	struct tbv_cfg_link link;
	struct tbv_id_route route = mk_route(10, 0, 5, 2, 10, 0, 4, 2);

	tbv_cfg_link_init(&link, 3);
	CHECK(tbv_cfg_link_activate(&link) == -EINVAL);
	CHECK(tbv_cfg_link_set_backend(&link, TBV_CFG_BACKEND_NATIVE) == 0);
	CHECK(tbv_cfg_link_seal(&link) == -EINVAL);
	CHECK(tbv_cfg_link_set_route(&link, &route) == 0);
	CHECK(tbv_cfg_link_seal(&link) == -EINVAL);

	return 0;
}

static int test_current_bridge_shape_cannot_seal(void)
{
	struct tbv_id_gid gids[] = {
		mk_gid(4, 1, 1, TBV_ID_GID_ROCE_V2, 10, 0, 4, 2),
		mk_gid(5, 1, 1, TBV_ID_GID_ROCE_V2, 192, 168, 23, 136),
		mk_gid(6, 1, 1, TBV_ID_GID_ROCE_V2, 192, 168, 23, 136),
	};
	struct tbv_id_route route =
		mk_route(192, 168, 23, 192, 192, 168, 23, 136);
	struct tbv_cfg_link link;

	tbv_cfg_link_init(&link, 4);
	CHECK(tbv_cfg_link_set_backend(&link, TBV_CFG_BACKEND_NATIVE) == 0);
	CHECK(tbv_cfg_link_set_route(&link, &route) == 0);
	CHECK(tbv_cfg_link_set_app_gids(&link, gids, 3) == 0);
	CHECK(tbv_cfg_link_seal(&link) == -EEXIST);
	CHECK(link.state == TBV_CFG_DRAFT);

	return 0;
}

static int test_nccl_workaround_mismatch_cannot_seal(void)
{
	struct tbv_id_gid gids[] = {
		mk_gid(4, 1, 1, TBV_ID_GID_ROCE_V2, 10, 0, 4, 2),
		mk_gid(5, 1, 1, TBV_ID_GID_ROCE_V2, 192, 168, 23, 136),
	};
	struct tbv_id_route route = mk_route(10, 0, 5, 2, 192, 168, 23, 136);
	struct tbv_id_nccl_policy nccl;
	struct tbv_cfg_link link;

	tbv_id_nccl_policy_default(&nccl);
	tbv_id_addr_v4(&nccl.addr_range, 10, 0, 0, 0);
	nccl.has_addr_range = true;
	nccl.addr_range_bits = 8;

	tbv_cfg_link_init(&link, 5);
	CHECK(tbv_cfg_link_set_backend(&link, TBV_CFG_BACKEND_NATIVE) == 0);
	CHECK(tbv_cfg_link_set_route(&link, &route) == 0);
	CHECK(tbv_cfg_link_set_nccl_policy(&link, &nccl) == 0);
	CHECK(tbv_cfg_link_set_app_gids(&link, gids, 2) == 0);
	CHECK(tbv_cfg_link_seal(&link) == -EXDEV);

	return 0;
}

static int test_sealed_links_are_immutable(void)
{
	struct tbv_cfg_link link;
	struct tbv_id_route route = mk_route(10, 0, 5, 2, 10, 0, 4, 2);

	tbv_cfg_link_init(&link, 6);
	CHECK(configure_canonical(&link, TBV_CFG_BACKEND_NATIVE) == 0);
	CHECK(tbv_cfg_link_seal(&link) == 0);
	CHECK(tbv_cfg_link_set_route(&link, &route) == -EBUSY);
	CHECK(tbv_cfg_link_set_backend(&link, TBV_CFG_BACKEND_APPLE) == -EBUSY);

	return 0;
}

int main(void)
{
	CHECK(test_canonical_native_activates() == 0);
	CHECK(test_canonical_apple_uses_same_identity_gate() == 0);
	CHECK(test_partial_links_cannot_activate() == 0);
	CHECK(test_current_bridge_shape_cannot_seal() == 0);
	CHECK(test_nccl_workaround_mismatch_cannot_seal() == 0);
	CHECK(test_sealed_links_are_immutable() == 0);

	puts("config smoke OK");
	return 0;
}
