/* SPDX-License-Identifier: GPL-2.0 */
#ifndef TBV_H
#define TBV_H

#include <linux/bitops.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/types.h>
#include <linux/uuid.h>
#include <linux/workqueue.h>

#define TBV_DRV_NAME "thunderbolt_ibverbs"
#define TBV_ETH_ALEN 6
#define TBV_NATIVE_PROTOCOL_KEY "tbverbs"
#define TBV_NATIVE_PRTCID 1
#define TBV_NATIVE_PRTCVERS 1
#define TBV_NATIVE_PRTCREVS 0
#define TBV_APPLE_PRTCID 0xfa57
#define TBV_APPLE_PRTCVERS 1
#define TBV_APPLE_PRTCREVS 0

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

enum tbv_path_state {
	TBV_PATH_NEW,
	TBV_PATH_RING_ALLOCATED,
	TBV_PATH_RING_STARTED,
	TBV_PATH_TUNNEL_ENABLED,
	TBV_PATH_STOPPED,
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

struct tbv_path_config {
	u32 tx_ring_size;
	u32 rx_ring_size;
	u32 tx_flags;
	u32 rx_flags;
	u16 sof_mask;
	u16 eof_mask;
	bool e2e;
};

struct tbv_path {
	enum tbv_path_state state;
	struct tbv_path_config cfg;
	struct tb_ring *tx_ring;
	struct tb_ring *rx_ring;
	int local_transmit_path;
	int remote_transmit_path;
};

struct tbv_rail {
	struct list_head node;
	struct tbv_peer *peer;
	struct tbv_state *native_work_state;
	struct tbv_rail_key key;
	struct tbv_path path;
	struct delayed_work native_work;
	u32 rail_id;
	u32 link_speed;
	u32 link_width;
	u32 remote_rail_id;
	int remote_transmit_path;
	int remote_tx_hop;
	int remote_rx_hop;
	u32 native_attempts;
	int native_last_error;
	bool active;
	bool native_negotiated;
	bool native_work_stop;
};

struct tbv_peer {
	struct list_head node;
	refcount_t refcnt;
	u32 peer_id;
	enum tbv_backend_type backend;
	struct tb_xdomain *xd;
	struct list_head rails;
	u32 nr_rails;
};

struct tbv_tbnet_identity {
	enum tbv_tbnet_identity_mode mode;
	unsigned long state;
};

struct tbv_tbip_control {
	u64 route;
	u8 sequence;
	uuid_t initiator_uuid;
	uuid_t target_uuid;
	u32 command_id;
};

struct tbv_tbip_login_params {
	struct tbv_tbip_control ctrl;
	u32 transmit_path;
};

struct tbv_tbip_login_response_params {
	struct tbv_tbip_control ctrl;
	u32 status;
	u8 receiver_mac[TBV_ETH_ALEN];
};

struct tbv_tbnet_arp_proxy {
	__be32 ipv4;
	u8 mac[TBV_ETH_ALEN];
};

struct tbv_state {
	struct tbv_resolved_config cfg;
	struct mutex lock;
	struct list_head peers;
	u32 next_peer_id;
	struct tbv_tbnet_identity tbnet_identity;
	struct tb_property_dir *native_dir;
	struct tb_property_dir *apple_dir;
	struct dentry *debugfs_dir;
	bool allocate_rings;
	bool start_rings;
	bool negotiate_native;
	bool enable_tunnels;
	bool services_registered;
};

struct dentry;
struct tbv_service_config {
	u32 native_prtcstns;
	u32 apple_prtcstns;
	bool allocate_rings;
	bool start_rings;
	bool negotiate_native;
	bool enable_tunnels;
};

struct tb_property_dir;
struct tb_ring;
struct tb_xdomain;
extern const uuid_t tbv_native_service_uuid;

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
int tbv_tbip_build_login(void *buf, size_t size,
			 const struct tbv_tbip_login_params *params);
int tbv_tbip_build_login_response(void *buf, size_t size,
				  const struct tbv_tbip_login_response_params *params);
int tbv_tbip_parse_login(const void *buf, size_t size,
			 struct tbv_tbip_login_params *params);
int tbv_tbnet_arp_reply_for_request(void *reply, size_t reply_size,
				    const void *request, size_t request_size,
				    const struct tbv_tbnet_arp_proxy *proxy);
struct tb_property_dir *tbv_service_create_native_dir(void);
struct tb_property_dir *tbv_service_create_apple_dir(u32 prtcstns);
int tbv_services_start(struct tbv_state *state, bool bind_services,
		       const struct tbv_service_config *service_cfg);
void tbv_services_stop(struct tbv_state *state);
int tbv_native_control_start(struct tbv_state *state);
void tbv_native_control_stop(void);
void tbv_native_control_init_rail(struct tbv_rail *rail,
				  struct tbv_peer *peer);
void tbv_native_control_queue_rail(struct tbv_state *state,
				   struct tbv_rail *rail);
void tbv_native_control_cancel_rail(struct tbv_rail *rail);
int tbv_native_control_exchange(struct tbv_state *state, struct tbv_peer *peer,
				struct tbv_rail *rail);
void tbv_rail_key_init(struct tbv_rail_key *key, u64 route,
		       u32 local_adapter, u32 remote_adapter, u32 path_id);
int tbv_rail_key_cmp(const struct tbv_rail_key *a,
		     const struct tbv_rail_key *b);
u32 tbv_rail_key_hash(const struct tbv_rail_key *key);
struct tbv_peer *tbv_peer_create(struct tbv_state *state,
				 enum tbv_backend_type backend,
				 struct tb_xdomain *xd);
void tbv_peer_destroy(struct tbv_state *state, struct tbv_peer *peer);
int tbv_peer_add_rail(struct tbv_peer *peer, const struct tbv_rail_key *key);
void tbv_path_default_config(enum tbv_backend_type backend,
			     struct tbv_path_config *cfg);
void tbv_path_init(struct tbv_path *path,
		   const struct tbv_path_config *cfg);
void tbv_path_reset(struct tbv_path *path);
const char *tbv_path_state_name(enum tbv_path_state state);
int tbv_path_alloc_rings(struct tbv_path *path, struct tb_xdomain *xd,
			 int requested_transmit_path);
int tbv_path_start_rings(struct tbv_path *path);
int tbv_path_enable_tunnel(struct tbv_path *path, struct tb_xdomain *xd,
			   int remote_transmit_path);
void tbv_path_destroy(struct tbv_path *path, struct tb_xdomain *xd);

const struct tbv_backend_ops *tbv_backend_get(enum tbv_backend_type type);
int tbv_debugfs_init(struct tbv_state *state);
void tbv_debugfs_exit(struct tbv_state *state);

int tbv_core_init(struct tbv_state *state,
		  const struct tbv_resolved_config *cfg);
void tbv_core_exit(struct tbv_state *state);

#endif
