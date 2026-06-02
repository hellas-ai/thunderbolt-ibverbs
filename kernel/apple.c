// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: apple: " fmt

#include <linux/errno.h>
#include <linux/printk.h>

#include "transport.h"

const struct tbv_backend_ops tbv_apple_backend_ops = {
	.type = TBV_BACKEND_APPLE,
	.name = "apple-fa57",
	.supports_rc = false,
	.supports_uc = true,
	.needs_tbnet_identity = true,
};

static int tbv_apple_validate_config(const struct tbv_cfg_link *link)
{
	if (link->backend != TBV_CFG_BACKEND_APPLE)
		return -EINVAL;
	if (!link->app_selection.valid)
		return -EINVAL;

	return 0;
}

static int tbv_apple_activate(struct tbv_state *state,
			      const struct tbv_cfg_link *link)
{
	pr_info("activate link=%u dev=%u gid=%u\n",
		link->link_id, link->app_selection.device_id,
		link->app_selection.gid_index);
	return 0;
}

static void tbv_apple_deactivate(struct tbv_state *state,
				 const struct tbv_cfg_link *link)
{
	pr_info("deactivate link=%u\n", link->link_id);
}

const struct tbv_transport_ops tbv_apple_transport_ops = {
	.type = TBV_BACKEND_APPLE,
	.name = "apple-fa57",
	.validate_config = tbv_apple_validate_config,
	.activate = tbv_apple_activate,
	.deactivate = tbv_apple_deactivate,
};
