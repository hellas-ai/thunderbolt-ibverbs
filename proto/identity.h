/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef TBV_IDENTITY_H
#define TBV_IDENTITY_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/types.h>
typedef u8 tbv_id_u8;
typedef u32 tbv_id_u32;
#else
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t tbv_id_u8;
typedef uint32_t tbv_id_u32;
#endif

#define TBV_ID_ADDR_BYTES 16u

enum tbv_id_addr_family {
	TBV_ID_AF_NONE = 0,
	TBV_ID_AF_INET = 4,
	TBV_ID_AF_INET6 = 6,
};

enum tbv_id_gid_type {
	TBV_ID_GID_IB = 0,
	TBV_ID_GID_ROCE_V1 = 1,
	TBV_ID_GID_ROCE_V2 = 2,
};

struct tbv_id_addr {
	tbv_id_u8 family;
	tbv_id_u8 bytes[TBV_ID_ADDR_BYTES];
};

struct tbv_id_gid {
	tbv_id_u32 device_id;
	tbv_id_u8 port;
	tbv_id_u8 gid_index;
	tbv_id_u8 gid_type;
	struct tbv_id_addr addr;
};

struct tbv_id_route {
	struct tbv_id_addr peer_addr;
	struct tbv_id_addr local_src_addr;
};

struct tbv_id_nccl_policy {
	tbv_id_u8 family;
	tbv_id_u8 roce_version;
	bool has_addr_range;
	struct tbv_id_addr addr_range;
	tbv_id_u8 addr_range_bits;
};

struct tbv_id_selection {
	bool valid;
	tbv_id_u32 device_id;
	tbv_id_u8 port;
	tbv_id_u8 gid_index;
	tbv_id_u8 gid_type;
	struct tbv_id_addr addr;
};

void tbv_id_addr_v4(struct tbv_id_addr *addr, tbv_id_u8 a, tbv_id_u8 b,
		    tbv_id_u8 c, tbv_id_u8 d);
void tbv_id_addr_v6(struct tbv_id_addr *addr,
		    const tbv_id_u8 bytes[TBV_ID_ADDR_BYTES]);
bool tbv_id_addr_equal(const struct tbv_id_addr *a,
		       const struct tbv_id_addr *b);

void tbv_id_nccl_policy_default(struct tbv_id_nccl_policy *policy);
int tbv_id_select_llama(const struct tbv_id_gid *gids, tbv_id_u32 gid_count,
			const struct tbv_id_route *route,
			struct tbv_id_selection *selection);
int tbv_id_select_nccl(const struct tbv_id_gid *gids, tbv_id_u32 gid_count,
		       const struct tbv_id_nccl_policy *policy,
		       struct tbv_id_selection *selection);
int tbv_id_validate_app_compat(const struct tbv_id_gid *gids,
			       tbv_id_u32 gid_count,
			       const struct tbv_id_route *route,
			       const struct tbv_id_nccl_policy *policy,
			       struct tbv_id_selection *selection);

#endif /* TBV_IDENTITY_H */
