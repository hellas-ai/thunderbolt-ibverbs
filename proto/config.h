/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef TBV_CONFIG_H
#define TBV_CONFIG_H

#include "proto/identity.h"

#define TBV_CFG_MAX_APP_GIDS 8u

enum tbv_cfg_backend {
	TBV_CFG_BACKEND_NONE = 0,
	TBV_CFG_BACKEND_NATIVE = 1,
	TBV_CFG_BACKEND_APPLE = 2,
};

enum tbv_cfg_state {
	TBV_CFG_EMPTY = 0,
	TBV_CFG_DRAFT = 1,
	TBV_CFG_SEALED = 2,
	TBV_CFG_ACTIVE = 3,
};

struct tbv_cfg_link {
	tbv_id_u32 link_id;
	tbv_id_u8 state;
	tbv_id_u8 backend;
	bool route_set;
	bool nccl_policy_set;
	struct tbv_id_route route;
	struct tbv_id_nccl_policy nccl_policy;
	struct tbv_id_gid app_gids[TBV_CFG_MAX_APP_GIDS];
	tbv_id_u32 app_gid_count;
	struct tbv_id_selection app_selection;
};

void tbv_cfg_link_init(struct tbv_cfg_link *link, tbv_id_u32 link_id);
int tbv_cfg_link_set_backend(struct tbv_cfg_link *link, tbv_id_u8 backend);
int tbv_cfg_link_set_route(struct tbv_cfg_link *link,
			   const struct tbv_id_route *route);
int tbv_cfg_link_set_nccl_policy(struct tbv_cfg_link *link,
				 const struct tbv_id_nccl_policy *policy);
int tbv_cfg_link_set_app_gids(struct tbv_cfg_link *link,
			      const struct tbv_id_gid *gids,
			      tbv_id_u32 gid_count);
int tbv_cfg_link_seal(struct tbv_cfg_link *link);
int tbv_cfg_link_activate(struct tbv_cfg_link *link);

#endif /* TBV_CONFIG_H */
