// SPDX-License-Identifier: GPL-2.0
/*
 * Apple-compatible TBnet identity policy.
 *
 * This file will eventually own the minimal ThunderboltIP identity backend.
 * For now it validates profile combinations so the public module starts with
 * explicit behavior instead of hidden Mac-specific assumptions.
 */

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "tbv.h"

int tbv_tbnet_identity_check_config(const struct tbv_resolved_config *cfg)
{
	const struct tbv_config *requested = &cfg->requested;

	if (cfg->apple_enabled &&
	    cfg->tbnet_identity == TBV_TBNET_ID_OFF) {
		pr_err("Apple-compatible backend requires TBnet identity\n");
		return -EINVAL;
	}

	if (requested->tbnet == TBV_TBNET_BLOCK &&
	    (cfg->tbnet_identity == TBV_TBNET_ID_STOCK ||
	     cfg->tbnet_identity == TBV_TBNET_ID_STOCK_PROXY)) {
		pr_err("tbnet=block conflicts with tbnet_identity=%s\n",
		       tbv_tbnet_identity_name(cfg->tbnet_identity));
		return -EINVAL;
	}

	if (cfg->profile == TBV_PROFILE_LINUX_PERF &&
	    requested->tbnet_identity != TBV_TBNET_ID_AUTO &&
	    cfg->tbnet_identity != TBV_TBNET_ID_OFF) {
		pr_warn("linux_perf ignores Apple TBnet identity unless an Apple peer is selected\n");
	}

	return 0;
}

int tbv_tbnet_identity_prepare(struct tbv_tbnet_identity *identity,
			       const struct tbv_resolved_config *cfg)
{
	memset(identity, 0, sizeof(*identity));
	identity->mode = cfg->tbnet_identity;

	if (!cfg->apple_enabled || cfg->tbnet_identity == TBV_TBNET_ID_OFF)
		return 0;

	switch (cfg->tbnet_identity) {
	case TBV_TBNET_ID_STOCK:
		identity->state = TBV_TBNET_ID_STATE_CARRIER |
				  TBV_TBNET_ID_STATE_NEIGHBOR_READY |
				  TBV_TBNET_ID_STATE_PACKET_PATH_ACTIVE |
				  TBV_TBNET_ID_STATE_FULL_IP_ACTIVE;
		pr_info("TBnet identity uses stock ThunderboltIP\n");
		return 0;

	case TBV_TBNET_ID_STOCK_PROXY:
		identity->state = TBV_TBNET_ID_STATE_CARRIER |
				  TBV_TBNET_ID_STATE_NEIGHBOR_READY |
				  TBV_TBNET_ID_STATE_PACKET_PATH_ACTIVE;
		pr_info("TBnet identity uses stock ThunderboltIP with RDMA GID proxying\n");
		return 0;

	case TBV_TBNET_ID_MINIMAL_PACKET:
		pr_err("tbnet_identity=minimal_packet is designed but not implemented\n");
		return -EOPNOTSUPP;

	case TBV_TBNET_ID_AUTO:
	case TBV_TBNET_ID_OFF:
	default:
		return 0;
	}
}

void tbv_tbnet_identity_stop(struct tbv_tbnet_identity *identity)
{
	identity->state = 0;
}
