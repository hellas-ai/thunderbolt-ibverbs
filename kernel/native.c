// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: native: " fmt

#include <linux/errno.h>
#include <linux/printk.h>

#include "transport.h"

const struct tbv_backend_ops tbv_native_backend_ops = {
	.type = TBV_BACKEND_NATIVE,
	.name = "native-linux",
	.supports_rc = true,
	.supports_uc = true,
	.needs_tbnet_identity = false,
};

static int tbv_native_validate_config(const struct tbv_cfg_link *link)
{
	if (link->backend != TBV_CFG_BACKEND_NATIVE)
		return -EINVAL;
	if (!link->app_selection.valid)
		return -EINVAL;
	if (link->app_selection.gid_type != TBV_ID_GID_ROCE_V2)
		return -EINVAL;

	return 0;
}

static int tbv_native_activate(struct tbv_state *state,
			       const struct tbv_cfg_link *link)
{
	pr_info("activate link=%u dev=%u gid=%u\n",
		link->link_id, link->app_selection.device_id,
		link->app_selection.gid_index);
	return 0;
}

static void tbv_native_deactivate(struct tbv_state *state,
				  const struct tbv_cfg_link *link)
{
	pr_info("deactivate link=%u\n", link->link_id);
}

const struct tbv_transport_ops tbv_native_transport_ops = {
	.type = TBV_BACKEND_NATIVE,
	.name = "native-linux",
	.validate_config = tbv_native_validate_config,
	.activate = tbv_native_activate,
	.deactivate = tbv_native_deactivate,
};
