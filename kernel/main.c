// SPDX-License-Identifier: GPL-2.0
/*
 * Release-track Thunderbolt/USB4 RDMA verbs module.
 *
 * This file intentionally starts small. Hardware binding, verbs registration,
 * native Linux transport, and Apple-compatible transport will be added behind
 * the profile model defined here instead of inheriting the exploratory module's
 * debug parameters.
 */

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/init.h>
#include <linux/module.h>
#include <linux/string.h>

#include "tbv.h"

static char *compat = "auto";
module_param(compat, charp, 0444);
MODULE_PARM_DESC(compat, "Compatibility mode: auto, force, off");

static char *profile = "auto";
module_param(profile, charp, 0444);
MODULE_PARM_DESC(profile, "Profile: auto, mac_compat, linux_perf, mixed");

static char *tbnet = "auto";
module_param(tbnet, charp, 0444);
MODULE_PARM_DESC(tbnet, "Thunderbolt-net coexistence: auto, allow, prefer_rdma, block");

static char *tbnet_identity = "auto";
module_param(tbnet_identity, charp, 0444);
MODULE_PARM_DESC(tbnet_identity,
		 "Apple TBnet identity: auto, stock, stock_proxy, minimal_packet, off");

static char *lanes = "auto";
module_param(lanes, charp, 0444);
MODULE_PARM_DESC(lanes, "Lane request: auto, N, or MIN-MAX");

static bool bind_services;
module_param(bind_services, bool, 0444);
MODULE_PARM_DESC(bind_services,
		 "Register Thunderbolt service drivers and advertise services");

static struct tbv_state tbv_driver_state;

static int __init tbv_init(void)
{
	char lanes_desc[32];
	struct tbv_resolved_config resolved;
	struct tbv_config cfg;
	int ret;

	ret = tbv_config_parse(&cfg, compat, profile, tbnet,
			       tbnet_identity, lanes);
	if (ret)
		return ret;

	ret = tbv_config_resolve(&resolved, &cfg);
	if (ret)
		return ret;

	ret = tbv_tbnet_identity_check_config(&resolved);
	if (ret)
		return ret;

	ret = tbv_core_init(&tbv_driver_state, &resolved);
	if (ret)
		return ret;

	ret = tbv_services_start(&tbv_driver_state, bind_services);
	if (ret) {
		tbv_core_exit(&tbv_driver_state);
		return ret;
	}

	if (cfg.lanes_auto)
		strscpy(lanes_desc, "auto", sizeof(lanes_desc));
	else if (cfg.lanes_min == cfg.lanes_max)
		snprintf(lanes_desc, sizeof(lanes_desc), "%u",
			 cfg.lanes_min);
	else
		snprintf(lanes_desc, sizeof(lanes_desc), "%u-%u",
			 cfg.lanes_min, cfg.lanes_max);

	pr_info("loaded compat=%s profile=%s resolved_profile=%s tbnet=%s tbnet_identity=%s lanes=%s\n",
		tbv_compat_name(cfg.compat),
		tbv_profile_name(cfg.profile),
		tbv_profile_name(resolved.profile),
		tbv_tbnet_policy_name(cfg.tbnet),
		tbv_tbnet_identity_name(resolved.tbnet_identity),
		lanes_desc);

	return 0;
}

static void __exit tbv_exit(void)
{
	tbv_services_stop(&tbv_driver_state);
	tbv_core_exit(&tbv_driver_state);
	pr_info("unloaded\n");
}

module_init(tbv_init);
module_exit(tbv_exit);

MODULE_AUTHOR("thunderbolt-ibverbs contributors");
MODULE_DESCRIPTION("Thunderbolt/USB4 host-to-host RDMA verbs");
MODULE_LICENSE("GPL");
