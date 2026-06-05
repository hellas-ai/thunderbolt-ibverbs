// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include "proto/config.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

static bool tbv_cfg_link_mutable(const struct tbv_cfg_link *link)
{
	return link->state == TBV_CFG_EMPTY || link->state == TBV_CFG_DRAFT;
}

static bool tbv_cfg_link_name_char_allowed(char c)
{
	return (c >= 'a' && c <= 'z') ||
	       (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') ||
	       c == '_' || c == '-';
}

int tbv_cfg_link_validate_name(const char *name)
{
	const char *prefix = TBV_CFG_LINK_NAME_PREFIX;
	tbv_id_u32 i;

	if (!name)
		return -EINVAL;

	for (i = 0; prefix[i]; i++) {
		if (name[i] != prefix[i])
			return -EINVAL;
	}
	if (!name[i])
		return -EINVAL;

	for (; name[i]; i++) {
		if (i >= TBV_CFG_LINK_NAME_MAX)
			return -ENAMETOOLONG;
		if (!tbv_cfg_link_name_char_allowed(name[i]))
			return -EINVAL;
	}

	return 0;
}

void tbv_cfg_link_init(struct tbv_cfg_link *link, tbv_id_u32 link_id)
{
	memset(link, 0, sizeof(*link));
	link->link_id = link_id;
	tbv_id_nccl_policy_default(&link->nccl_policy);
}

int tbv_cfg_link_set_backend(struct tbv_cfg_link *link, tbv_id_u8 backend)
{
	if (!tbv_cfg_link_mutable(link))
		return -EBUSY;
	if (backend != TBV_CFG_BACKEND_NATIVE &&
	    backend != TBV_CFG_BACKEND_APPLE)
		return -EINVAL;

	link->backend = backend;
	link->state = TBV_CFG_DRAFT;
	return 0;
}

int tbv_cfg_link_set_route(struct tbv_cfg_link *link,
			   const struct tbv_id_route *route)
{
	if (!tbv_cfg_link_mutable(link))
		return -EBUSY;

	link->route = *route;
	link->route_set = true;
	link->state = TBV_CFG_DRAFT;
	return 0;
}

int tbv_cfg_link_set_nccl_policy(struct tbv_cfg_link *link,
				 const struct tbv_id_nccl_policy *policy)
{
	if (!tbv_cfg_link_mutable(link))
		return -EBUSY;

	link->nccl_policy = *policy;
	link->nccl_policy_set = true;
	link->state = TBV_CFG_DRAFT;
	return 0;
}

int tbv_cfg_link_set_app_gids(struct tbv_cfg_link *link,
			      const struct tbv_id_gid *gids,
			      tbv_id_u32 gid_count)
{
	if (!tbv_cfg_link_mutable(link))
		return -EBUSY;
	if (!gids || gid_count == 0 || gid_count > TBV_CFG_MAX_APP_GIDS)
		return -EINVAL;

	memcpy(link->app_gids, gids, sizeof(*gids) * gid_count);
	link->app_gid_count = gid_count;
	link->state = TBV_CFG_DRAFT;
	return 0;
}

int tbv_cfg_link_seal(struct tbv_cfg_link *link)
{
	struct tbv_id_nccl_policy default_nccl;
	const struct tbv_id_nccl_policy *nccl = &link->nccl_policy;
	int ret;

	if (!tbv_cfg_link_mutable(link))
		return -EBUSY;
	if (link->backend != TBV_CFG_BACKEND_NATIVE &&
	    link->backend != TBV_CFG_BACKEND_APPLE)
		return -EINVAL;
	if (!link->route_set || link->app_gid_count == 0)
		return -EINVAL;
	if (!link->nccl_policy_set) {
		tbv_id_nccl_policy_default(&default_nccl);
		nccl = &default_nccl;
	}

	ret = tbv_id_validate_app_compat(link->app_gids, link->app_gid_count,
					 &link->route, nccl,
					 &link->app_selection);
	if (ret)
		return ret;

	link->state = TBV_CFG_SEALED;
	return 0;
}

int tbv_cfg_link_activate(struct tbv_cfg_link *link)
{
	if (link->state != TBV_CFG_SEALED)
		return -EINVAL;

	link->state = TBV_CFG_ACTIVE;
	return 0;
}

int tbv_cfg_link_deactivate(struct tbv_cfg_link *link)
{
	if (link->state != TBV_CFG_ACTIVE)
		return -EINVAL;

	link->state = TBV_CFG_SEALED;
	return 0;
}
