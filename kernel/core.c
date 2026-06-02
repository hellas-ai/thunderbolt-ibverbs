// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/workqueue.h>

#include "tbv.h"

static void tbv_core_log_backend(enum tbv_backend_type type)
{
	const struct tbv_backend_ops *ops = tbv_backend_get(type);

	if (!ops) {
		pr_warn("missing backend ops for %s\n", tbv_backend_name(type));
		return;
	}

	pr_info("backend %s rc=%u uc=%u tbnet_identity=%u\n",
		ops->name, ops->supports_rc, ops->supports_uc,
		ops->needs_tbnet_identity);
}

int tbv_core_init(struct tbv_state *state,
		  const struct tbv_resolved_config *cfg,
		  const struct tbv_tbnet_identity_config *identity_cfg)
{
	int ret;

	memset(state, 0, sizeof(*state));
	state->cfg = *cfg;
	mutex_init(&state->lock);
	mutex_init(&state->rail_register_lock);
	INIT_LIST_HEAD(&state->peers);
	INIT_LIST_HEAD(&state->configured_links);
	xa_init(&state->verbs_mrs_xa);
	xa_init(&state->verbs_qps_xa);
	state->next_peer_id = 1;
	state->workqueue = alloc_workqueue("tbv_ibdev",
					   WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	if (!state->workqueue) {
		mutex_destroy(&state->rail_register_lock);
		mutex_destroy(&state->lock);
		return -ENOMEM;
	}

	if (!cfg->native_enabled && !cfg->apple_enabled) {
		ret = -EINVAL;
		goto err_destroy_wq;
	}

	ret = tbv_tbnet_identity_prepare(&state->tbnet_identity, cfg,
					 identity_cfg);
	if (ret)
		goto err_destroy_wq;

	ret = tbv_debugfs_init(state);
	if (ret) {
		tbv_tbnet_identity_stop(&state->tbnet_identity);
		goto err_destroy_wq;
	}

	ret = tbv_configfs_start(state);
	if (ret) {
		tbv_debugfs_exit(state);
		tbv_tbnet_identity_stop(&state->tbnet_identity);
		goto err_destroy_wq;
	}

	if (cfg->native_enabled)
		tbv_core_log_backend(TBV_BACKEND_NATIVE);

	if (cfg->apple_enabled)
		tbv_core_log_backend(TBV_BACKEND_APPLE);

	pr_info("core ready profile=%s rc=%u uc=%u\n",
		tbv_profile_name(cfg->profile), cfg->rc_supported,
		cfg->uc_supported);
	return 0;

err_destroy_wq:
	destroy_workqueue(state->workqueue);
	state->workqueue = NULL;
	mutex_destroy(&state->rail_register_lock);
	mutex_destroy(&state->lock);
	return ret;
}

void tbv_core_exit(struct tbv_state *state)
{
	u32 configured_link_count;

	if (!list_empty(&state->peers))
		pr_warn("unloading with live peers after service teardown\n");

	tbv_configfs_stop(state);
	configured_link_count = tbv_link_count(state);
	if (configured_link_count)
		pr_warn("unloading with %u configured links after configfs teardown\n",
			configured_link_count);

	if (state->workqueue) {
		flush_workqueue(state->workqueue);
		destroy_workqueue(state->workqueue);
		state->workqueue = NULL;
	}

	tbv_debugfs_exit(state);
	if (!xa_empty(&state->verbs_mrs_xa))
		pr_warn("unloading with live MR registry entries\n");
	if (!xa_empty(&state->verbs_qps_xa))
		pr_warn("unloading with live QP registry entries\n");
	xa_destroy(&state->verbs_mrs_xa);
	xa_destroy(&state->verbs_qps_xa);
	tbv_tbnet_identity_stop(&state->tbnet_identity);
	mutex_destroy(&state->rail_register_lock);
	mutex_destroy(&state->lock);
}
