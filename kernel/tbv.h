/* SPDX-License-Identifier: GPL-2.0 */
#ifndef TBV_H
#define TBV_H

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/if.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/notifier.h>
#include <linux/refcount.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/uuid.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#define TBV_DRV_NAME "thunderbolt_ibverbs"
#define TBV_ETH_ALEN 6
#define TBV_NATIVE_PROTOCOL_KEY "tbverbs"
#define TBV_NATIVE_MAX_LANES 4
#define TBV_NATIVE_PRTCID 1
#define TBV_NATIVE_PRTCVERS 1
#define TBV_NATIVE_PRTCREVS 0
#define TBV_APPLE_PRTCID 0xfa57
#define TBV_APPLE_PRTCVERS 1
#define TBV_APPLE_PRTCREVS 0
#define TBV_APPLE_QPN_SHIFT 8
#define TBV_APPLE_FRAME_SIZE SZ_4K
#define TBV_APPLE_MAX_MSG_SIZE SZ_16M

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
	int tx_hop;
	int rx_hop;
	int transmit_path;
	int receive_path;
	u16 sof_mask;
	u16 eof_mask;
	bool e2e;
};

struct tbv_path {
	enum tbv_path_state state;
	struct tbv_path_config cfg;
	struct tbv_rail *rail;
	struct tb_ring *tx_ring;
	struct tb_ring *rx_ring;
	struct tbv_data_frame *tx_frames;
	struct tbv_data_frame *rx_frames;
	struct tbv_tx_packet *tx_control_packets;
	struct tbv_tx_packet *tx_data_packets;
	u32 tx_frame_count;
	u32 rx_frame_count;
	u32 tx_control_packet_count;
	u32 tx_data_packet_count;
	u32 tx_control_queued;
	u32 tx_data_queued;
	u32 tx_data_reserved;
	u32 tx_data_queue_limit;
	spinlock_t tx_lock;
	struct list_head tx_free;
	struct list_head tx_control_free;
	struct list_head tx_data_free;
	struct list_head tx_control_queue;
	struct list_head tx_data_queue;
	struct delayed_work tx_poll_work;
	atomic_t tx_inflight;
	atomic64_t data_tx_enqueued;
	atomic64_t data_tx_posted;
	atomic64_t data_tx_completed;
	atomic64_t data_rx_completed;
	u8 rx_raw_opcode;
	u8 rx_raw_flags;
	u32 rx_raw_dest_qp;
	u32 rx_raw_src_qp;
	u32 rx_raw_psn;
	u32 rx_raw_imm_data;
	u32 rx_raw_rkey;
	u32 rx_raw_done;
	u32 rx_raw_remaining;
	u64 rx_raw_base;
	bool rx_raw_pending;
	bool tx_scheduling;
	bool tx_raw_stream_active;
	int local_transmit_path;
	int local_tx_hop;
	int local_rx_hop;
	int remote_transmit_path;
};

struct tbv_rail {
	struct list_head node;
	struct tbv_peer *peer;
	struct tbv_state *native_work_state;
	struct tbv_rail_key key;
	struct tbv_path path;
	struct delayed_work native_work;
	refcount_t refcnt;
	struct completion refs_zero;
	u32 rail_id;
	u32 link_speed;
	u32 link_width;
	u32 remote_rail_id;
	int remote_transmit_path;
	int remote_tx_hop;
	int remote_rx_hop;
	u32 native_attempts;
	u32 native_tunnel_attempts;
	u32 native_ready_attempts;
	int native_last_error;
	bool active;
	bool removing;
	bool native_negotiated;
	bool native_ready_sent;
	bool native_remote_ready;
	bool native_work_stop;
};

struct tbv_peer {
	struct list_head node;
	refcount_t refcnt;
	struct tbv_state *state;
	u32 peer_id;
	enum tbv_backend_type backend;
	struct tb_xdomain *xd;
	struct list_head rails;
	u32 nr_rails;
};

static inline bool tbv_rail_data_ready(const struct tbv_rail *rail)
{
	return rail &&
	       rail->path.state == TBV_PATH_TUNNEL_ENABLED &&
	       rail->native_remote_ready;
}

static inline bool tbv_rail_apple_data_ready(const struct tbv_rail *rail)
{
	return rail && rail->path.state == TBV_PATH_TUNNEL_ENABLED;
}

struct tbv_tbnet_identity {
	enum tbv_tbnet_identity_mode mode;
	unsigned long state;
	struct mutex lock;
	char tbnet_netdev_name[IFNAMSIZ];
	char gid_netdev_name[IFNAMSIZ];
	struct net_device *tbnet_dev;
	struct net_device *gid_dev;
	struct notifier_block netdev_nb;
	struct notifier_block inetaddr_nb;
	__be32 proxy_ipv4;
	bool netdev_nb_registered;
	bool inetaddr_nb_registered;
	bool rx_handler_registered;
	atomic64_t arp_requests;
	atomic64_t arp_replies;
	atomic64_t arp_ignored;
	atomic64_t arp_errors;
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

struct tbv_tbnet_identity_config {
	const char *tbnet_netdev;
	const char *gid_netdev;
};

struct tbv_state {
	struct tbv_resolved_config cfg;
	struct mutex lock;
	struct list_head peers;
	u32 next_peer_id;
	struct tbv_tbnet_identity tbnet_identity;
	struct tb_property_dir *native_dirs[TBV_NATIVE_MAX_LANES];
	u32 native_dir_count;
	struct tb_property_dir *apple_dir;
	struct dentry *debugfs_dir;
	bool allocate_rings;
	bool start_rings;
	bool negotiate_native;
	bool enable_tunnels;
	bool native_data;
	bool apple_data;
	bool native_wr_striping;
	bool native_fragment_striping;
	bool register_verbs;
	bool services_registered;
	bool verbs_registered;
	atomic_t verbs_ucontexts;
	atomic_t verbs_pds;
	atomic_t verbs_cqs;
	atomic_t verbs_qps;
	atomic_t verbs_mrs;
	atomic_t verbs_recv_wqes;
	atomic64_t data_wr_send;
	atomic64_t data_wr_op_send;
	atomic64_t data_wr_op_send_imm;
	atomic64_t data_wr_op_write;
	atomic64_t data_wr_op_write_imm;
	atomic64_t data_wr_op_unsupported;
	atomic64_t data_wr_live;
	atomic64_t data_wr_no_path;
	atomic64_t data_wr_copied;
	atomic64_t data_wr_zcopy;
	atomic64_t data_wr_zcopy_fallback;
	atomic64_t data_wr_copy_error;
	atomic64_t data_wr_path_send;
	atomic64_t data_wr_path_send_error;
	atomic64_t data_tx_accepted;
	atomic64_t data_tx_posted;
	atomic64_t data_tx_completed;
	atomic64_t data_tx_canceled;
	atomic64_t data_tx_errors;
	atomic64_t data_rx_completed;
	atomic64_t data_rx_bad_frame;
	atomic64_t data_rx_bad_header;
	atomic64_t data_rx_send;
	atomic64_t data_rx_op_send;
	atomic64_t data_rx_op_send_imm;
	atomic64_t data_rx_op_write;
	atomic64_t data_rx_op_write_imm;
	atomic64_t data_rx_ack;
	atomic64_t data_rx_no_qp;
	atomic64_t data_rx_no_recv;
	atomic64_t data_rx_copy_error;
	atomic64_t data_rx_reorder_buffered;
	atomic64_t data_rx_reorder_delivered;
	atomic64_t data_rx_reorder_dropped;
	atomic64_t data_rx_reorder_window;
	atomic64_t data_rx_pending_discarded;
	atomic64_t data_cq_overflow;
	struct xarray verbs_mrs_xa;
	struct xarray verbs_qps_xa;
	struct device *verbs_parent;
	struct tbv_ibdev *ibdev;
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
struct tbv_data_frame;
struct tbv_native_data_header;
struct tbv_tx_packet;
struct device;
struct page;
struct tb_ring;
struct tb_xdomain;
typedef void (*tbv_path_tx_done_fn)(void *ctx, int status);
typedef int (*tbv_path_tx_fill_fn)(void *ctx, void *dst, u32 len);
typedef int (*tbv_path_next_page_fn)(void *ctx, struct page **page,
				     u32 *page_off, u32 *length,
				     tbv_path_tx_done_fn *done,
				     void **done_ctx);
#define TBV_PATH_SEND_CONTROL	BIT(0)
#define TBV_PATH_SEND_DEFER	BIT(1)
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

int tbv_ibdev_start(struct tbv_state *state, bool register_verbs);
void tbv_ibdev_stop(struct tbv_state *state);
const char *tbv_ibdev_roce_netdev_name(void);
void tbv_ibdev_rx_frame(struct tbv_state *state, const void *data, u32 len);
void tbv_ibdev_rx_native_frame(struct tbv_state *state,
			       const struct tbv_native_data_header *hdr,
			       const void *payload);
void tbv_ibdev_rx_apple_frame(struct tbv_state *state,
			      const struct tbv_path *path,
			      const void *payload, u32 len, u8 eof);

int tbv_tbnet_identity_check_config(const struct tbv_resolved_config *cfg);
int tbv_tbnet_identity_prepare(struct tbv_tbnet_identity *identity,
			       const struct tbv_resolved_config *cfg,
			       const struct tbv_tbnet_identity_config *identity_cfg);
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
int tbv_native_control_xdomain_start(struct tbv_state *state);
void tbv_native_control_xdomain_stop(void);
int tbv_native_control_legacy_start(struct tbv_state *state);
void tbv_native_control_legacy_stop(void);
int tbv_native_control_handle_packet(struct tbv_state *state,
				     struct tb_xdomain *source_xd,
				     const void *buf, size_t size);
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
struct tbv_peer *tbv_peer_get_or_create(struct tbv_state *state,
					enum tbv_backend_type backend,
					struct tb_xdomain *xd);
void tbv_peer_put(struct tbv_state *state, struct tbv_peer *peer);
struct tbv_rail *tbv_peer_add_rail(struct tbv_peer *peer,
				   const struct tbv_rail_key *key);
void tbv_peer_remove_rail(struct tbv_rail *rail);
void tbv_rail_put(struct tbv_rail *rail);
void tbv_path_default_config(enum tbv_backend_type backend,
			     struct tbv_path_config *cfg);
void tbv_path_init(struct tbv_path *path,
		   const struct tbv_path_config *cfg, struct tbv_rail *rail);
void tbv_path_reset(struct tbv_path *path);
const char *tbv_path_state_name(enum tbv_path_state state);
int tbv_path_alloc_rings(struct tbv_path *path, struct tb_xdomain *xd,
			 int requested_transmit_path);
int tbv_path_start_rings(struct tbv_path *path);
int tbv_path_enable_tunnel(struct tbv_path *path, struct tb_xdomain *xd,
			   int remote_transmit_path);
int tbv_path_reserve_data(struct tbv_path *path, u32 frames);
void tbv_path_release_data_reservation(struct tbv_path *path, u32 frames);
int tbv_path_send(struct tbv_path *path, const void *data, u32 len,
		  unsigned int flags,
		  tbv_path_tx_done_fn done, void *done_ctx);
int tbv_path_send_owned(struct tbv_path *path, void *data, u32 len,
			unsigned int flags,
			tbv_path_tx_done_fn done, void *done_ctx);
int tbv_path_send_marked_owned(struct tbv_path *path, void *data, u32 len,
			       u8 sof, u8 eof, unsigned int flags,
			       tbv_path_tx_done_fn done, void *done_ctx);
int tbv_path_send_marked_fill(struct tbv_path *path, u32 len,
			      u8 sof, u8 eof, unsigned int flags,
			      tbv_path_tx_fill_fn fill, void *fill_ctx,
			      tbv_path_tx_done_fn done, void *done_ctx);
int tbv_path_send_page_stream(struct tbv_path *path,
			      const struct tbv_native_data_header *hdr,
			      u32 total_length, unsigned int flags,
			      tbv_path_tx_done_fn meta_done,
			      void *meta_done_ctx,
			      tbv_path_next_page_fn next, void *next_ctx);
void tbv_path_kick_tx(struct tbv_path *path);
u32 tbv_path_cancel_data_done_ctx(struct tbv_path *path,
				  tbv_path_tx_done_fn done, void *done_ctx);
void tbv_path_destroy(struct tbv_path *path, struct tb_xdomain *xd);

const struct tbv_backend_ops *tbv_backend_get(enum tbv_backend_type type);
int tbv_debugfs_init(struct tbv_state *state);
void tbv_debugfs_exit(struct tbv_state *state);

int tbv_core_init(struct tbv_state *state,
		  const struct tbv_resolved_config *cfg,
		  const struct tbv_tbnet_identity_config *identity_cfg);
void tbv_core_exit(struct tbv_state *state);
void tbv_state_set_verbs_parent(struct tbv_state *state, struct device *dev);
struct device *tbv_state_get_verbs_parent(struct tbv_state *state);

#endif
