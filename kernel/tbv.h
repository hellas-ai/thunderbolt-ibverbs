/* SPDX-License-Identifier: GPL-2.0 */
#ifndef TBV_H
#define TBV_H

#include <linux/bitops.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/types.h>

#define TBV_DRV_NAME "thunderbolt_ibverbs"

#define TBV_TBNET_ID_STATE_CARRIER		BIT(0)
#define TBV_TBNET_ID_STATE_NEIGHBOR_READY	BIT(1)
#define TBV_TBNET_ID_STATE_PACKET_PATH_ACTIVE	BIT(2)
#define TBV_TBNET_ID_STATE_FULL_IP_ACTIVE	BIT(3)

enum tbv_compat_mode {
	TBV_COMPAT_AUTO,
	TBV_COMPAT_FORCE,
	TBV_COMPAT_OFF,
};

enum tbv_profile {
	TBV_PROFILE_AUTO,
	TBV_PROFILE_MAC_COMPAT,
	TBV_PROFILE_LINUX_PERF,
	TBV_PROFILE_MIXED,
};

enum tbv_tbnet_policy {
	TBV_TBNET_AUTO,
	TBV_TBNET_ALLOW,
	TBV_TBNET_PREFER_RDMA,
	TBV_TBNET_BLOCK,
};

enum tbv_tbnet_identity_mode {
	TBV_TBNET_ID_AUTO,
	TBV_TBNET_ID_STOCK,
	TBV_TBNET_ID_STOCK_PROXY,
	TBV_TBNET_ID_MINIMAL_PACKET,
	TBV_TBNET_ID_OFF,
};

enum tbv_backend_type {
	TBV_BACKEND_NATIVE,
	TBV_BACKEND_APPLE,
};

struct tbv_config {
	enum tbv_compat_mode compat;
	enum tbv_profile profile;
	enum tbv_tbnet_policy tbnet;
	enum tbv_tbnet_identity_mode tbnet_identity;
	u32 lanes_min;
	u32 lanes_max;
	bool lanes_auto;
};

struct tbv_resolved_config {
	struct tbv_config requested;
	enum tbv_profile profile;
	enum tbv_tbnet_identity_mode tbnet_identity;
	bool native_enabled;
	bool apple_enabled;
	bool rc_supported;
	bool uc_supported;
};

struct tbv_backend_ops {
	enum tbv_backend_type type;
	const char *name;
	bool supports_rc;
	bool supports_uc;
	bool needs_tbnet_identity;
};

struct tbv_rail_key {
	u64 route;
	u32 local_adapter;
	u32 remote_adapter;
	u32 path_id;
};

struct tbv_rail {
	struct list_head node;
	struct tbv_rail_key key;
	u32 rail_id;
	bool active;
};

struct tbv_peer {
	struct list_head node;
	refcount_t refcnt;
	u32 peer_id;
	enum tbv_backend_type backend;
	struct list_head rails;
	u32 nr_rails;
};

struct tbv_tbnet_identity {
	enum tbv_tbnet_identity_mode mode;
	unsigned long state;
};

struct tbv_state {
	struct tbv_resolved_config cfg;
	struct mutex lock;
	struct list_head peers;
	u32 next_peer_id;
	struct tbv_tbnet_identity tbnet_identity;
};

int tbv_config_parse(struct tbv_config *cfg, const char *compat,
		     const char *profile, const char *tbnet,
		     const char *tbnet_identity, const char *lanes);
int tbv_config_resolve(struct tbv_resolved_config *resolved,
		       const struct tbv_config *cfg);

const char *tbv_compat_name(enum tbv_compat_mode mode);
const char *tbv_profile_name(enum tbv_profile profile);
const char *tbv_tbnet_policy_name(enum tbv_tbnet_policy policy);
const char *tbv_tbnet_identity_name(enum tbv_tbnet_identity_mode mode);
const char *tbv_backend_name(enum tbv_backend_type type);

int tbv_tbnet_identity_check_config(const struct tbv_resolved_config *cfg);
int tbv_tbnet_identity_prepare(struct tbv_tbnet_identity *identity,
			       const struct tbv_resolved_config *cfg);
void tbv_tbnet_identity_stop(struct tbv_tbnet_identity *identity);

const struct tbv_backend_ops *tbv_backend_get(enum tbv_backend_type type);

int tbv_core_init(struct tbv_state *state,
		  const struct tbv_resolved_config *cfg);
void tbv_core_exit(struct tbv_state *state);

#endif
