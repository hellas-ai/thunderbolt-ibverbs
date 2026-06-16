/* SPDX-License-Identifier: GPL-2.0 */
#ifndef TBV_H
#define TBV_H

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/idr.h>
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

#include "proto/config.h"

#define TBV_DRV_NAME "thunderbolt_ibverbs"
#define TBV_ETH_ALEN 6
#define TBV_NATIVE_PROTOCOL_KEY "tbverbs"
#define TBV_NATIVE_MAX_LANES 4
#define TBV_DATA_PDF_FRAME_START 1
#define TBV_DATA_PDF_FRAME_END 3
#define TBV_NATIVE_PRTCID 1
#define TBV_NATIVE_PRTCVERS 1
#define TBV_NATIVE_PRTCREVS 0
#define TBV_APPLE_PRTCID 0xfa57
#define TBV_APPLE_PRTCVERS 1
#define TBV_APPLE_PRTCREVS 0
#define TBV_APPLE_QPN_SHIFT 8
#define TBV_APPLE_FRAME_SIZE SZ_4K
#define TBV_APPLE_MAX_MSG_SIZE SZ_16M

static inline bool tbv_dma_device_ready(const struct device *dev)
{
	if (!dev)
		return false;

	/*
	 * A Thunderbolt core reprobe can briefly expose ring DMA devices whose
	 * IOMMU group is attached before dev->iommu is populated. Calling
	 * dma_map_* in that window reaches iommu_get_dma_domain() and oopses.
	 * Treat it as probe deferral; direct-DMA systems have no iommu_group and
	 * still pass this check.
	 */
	return !dev->iommu_group || dev->iommu;
}

#define TBV_TBNET_ID_STATE_CARRIER		BIT(0)
#define TBV_TBNET_ID_STATE_NEIGHBOR_READY	BIT(1)
#define TBV_TBNET_ID_STATE_PACKET_PATH_ACTIVE	BIT(2)
#define TBV_TBNET_ID_STATE_FULL_IP_ACTIVE	BIT(3)

struct tb_property_dir;
struct tbv_tbnet_minimal_session;
struct seq_file;

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

struct tbv_path_owned_frame {
	struct list_head node;
	void *data;
	u32 len;
	u8 sof;
	u8 eof;
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
	u32 tx_remote_data_credits;
	u32 tx_remote_data_credit_max;
	u32 rx_data_credit_pending;
	spinlock_t tx_lock;
	struct list_head tx_free;
	struct list_head tx_control_free;
	struct list_head tx_data_free;
	struct list_head tx_control_queue;
	struct list_head tx_data_queue;
	struct list_head tx_zcopy_inflight;
	struct delayed_work tx_poll_work;
	struct delayed_work rx_supp_poll_work;
	atomic_t tx_inflight;
	atomic64_t data_tx_enqueued;
	atomic64_t data_tx_posted;
	atomic64_t data_tx_completed;
	atomic64_t control_tx_enqueued;
	atomic64_t control_tx_posted;
	atomic64_t control_tx_completed;
	atomic64_t control_tx_queue_max_ms;
	atomic64_t data_tx_credit_stalls;
	atomic64_t data_tx_credit_received;
	atomic64_t data_rx_completed;
	atomic64_t data_rx_canceled;
	atomic64_t data_rx_credit_sent;
	atomic64_t data_rx_credit_send_error;
	atomic64_t data_rx_repost_failed;
	atomic64_t tx_poll_calls;
	atomic64_t tx_poll_completed;
	atomic64_t rx_supp_poll_calls;
	atomic64_t rx_supp_poll_completed;
	unsigned long rx_supp_poll_until;
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
	bool tx_poll_enabled;
	bool rx_supp_poll_enabled;
	bool tx_scheduling;
	bool tx_raw_stream_active;
	bool tx_raw_stream_end_seen;
	u32 tx_raw_stream_inflight;
	void *tx_raw_stream_owner;
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
	/*
	 * Per-rail IB device. Lifecycle managed by tbv_ibdev_rail_event()
	 * (see ibdev.c) under state->rail_register_lock. NULL means this rail
	 * has not yet reached the data-ready edge (or has been torn down).
	 */
	struct tbv_ibdev *ibdev;
	atomic_t native_qp_bind_count;
	u32 apple_tunnel_qps;
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
	/*
	 * Physical lane index for native rails (0..TBV_NATIVE_MAX_LANES-1).
	 * Set in tbv_peer_add_rail() from the matched service id and consumed
	 * by tbv_ibdev_rail_name_index(). Don't derive this from rail->key.path_id;
	 * the encoded path_id uses TBV_NATIVE_PRTCID as its low byte for lane 0,
	 * which collides with the (prtcid << 8) | lane scheme used for the
	 * other lanes (both lane 0 and lane 1 would round-trip to "lane 1").
	 * Undefined for non-native backends.
	 */
	u32 native_lane;
	bool active;
	bool removing;
	/*
	 * Registration unwinding marker. Set under state->rail_register_lock
	 * when ib_register_device() returns nonzero for this rail. While true
	 * tbv_ibdev_start()'s catchup loop and tbv_ibdev_rail_event() will
	 * skip the rail, breaking the spin loop the failed lane would
	 * otherwise cause.
	 */
	bool ibdev_register_failed;
	/*
	 * Retryable registration blocker. A ready rail can reach verbs
	 * registration before the configured RoCE netdev exists; keep it out of
	 * the catchup loop until a matching netdev notifier event retries it.
	 */
	bool ibdev_register_deferred;
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
	struct ida rail_ids;
	/* Serializes XDomain control and tunnel setup transactions per link. */
	struct mutex control_lock;
	u32 native_qp_rr_rail_id;
	u32 nr_rails;
};

static inline bool tbv_rail_data_ready(const struct tbv_rail *rail)
{
	if (!rail || rail->path.state != TBV_PATH_TUNNEL_ENABLED)
		return false;
	if (!rail->peer || rail->peer->backend != TBV_BACKEND_NATIVE)
		return true;
	return rail->native_ready_sent && rail->native_remote_ready;
}

static inline bool tbv_rail_apple_data_ready(const struct tbv_rail *rail)
{
	return rail && rail->path.state == TBV_PATH_TUNNEL_ENABLED;
}

static inline bool tbv_rail_apple_service_ready(const struct tbv_rail *rail)
{
	if (!rail)
		return false;
	return rail->path.state == TBV_PATH_RING_STARTED ||
	       rail->path.state == TBV_PATH_TUNNEL_ENABLED;
}

struct tbv_tbnet_identity {
	enum tbv_tbnet_identity_mode mode;
	unsigned long state;
	struct mutex lock;
	struct list_head minimal_sessions;
	struct tb_property_dir *minimal_dir;
	char tbnet_netdev_name[IFNAMSIZ];
	char gid_netdev_name[IFNAMSIZ];
	struct net_device *tbnet_dev;
	struct net_device *gid_dev;
	struct notifier_block netdev_nb;
	struct notifier_block inetaddr_nb;
	__be32 proxy_ipv4;
	bool minimal_e2e;
	bool minimal_apple_only;
	bool minimal_neighbor_seen;
	bool minimal_dir_registered;
	bool minimal_driver_registered;
	bool minimal_started;
	bool netdev_nb_registered;
	bool inetaddr_nb_registered;
	bool rx_handler_registered;
	atomic64_t minimal_login_rx;
	atomic64_t minimal_login_tx;
	atomic64_t minimal_logout_rx;
	atomic64_t minimal_logout_tx;
	atomic64_t minimal_status_rx;
	atomic64_t minimal_status_tx;
	atomic64_t minimal_packet_rx;
	atomic64_t minimal_packet_tx_posted;
	atomic64_t minimal_packet_tx;
	atomic64_t minimal_packet_tx_errors;
	atomic64_t minimal_path_errors;
	atomic64_t arp_requests;
	atomic64_t arp_replies;
	atomic64_t arp_ignored;
	atomic64_t arp_errors;
};

enum tbv_tbip_type {
	TBV_TBIP_LOGIN,
	TBV_TBIP_LOGIN_RESPONSE,
	TBV_TBIP_LOGOUT,
	TBV_TBIP_STATUS,
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

struct tbv_tbip_status_params {
	struct tbv_tbip_control ctrl;
	u32 status;
};

struct tbv_tbip_status_result {
	struct tbv_tbip_control ctrl;
	u32 status;
};

struct tbv_tbip_login_response_result {
	struct tbv_tbip_control ctrl;
	u32 status;
	u8 receiver_mac[TBV_ETH_ALEN];
	u32 receiver_mac_len;
};

struct tbv_tbnet_arp_proxy {
	__be32 ipv4;
	u8 mac[TBV_ETH_ALEN];
};

struct tbv_tbnet_identity_config {
	const char *tbnet_netdev;
	const char *gid_netdev;
	bool minimal_e2e;
	bool minimal_apple_only;
};

#define TBV_CONFIGURED_LINK_NAME_LEN (TBV_CFG_LINK_NAME_MAX + 1u)

struct tbv_configured_link {
	struct list_head node;
	u32 link_id;
	enum tbv_backend_type backend;
	struct tbv_id_selection app_selection;
	char name[TBV_CONFIGURED_LINK_NAME_LEN];
};

struct tbv_state {
	struct tbv_resolved_config cfg;
	struct mutex lock;
	struct list_head peers;
	struct list_head configured_links;
	u32 next_peer_id;
	u32 configured_link_count;
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
	bool native_fragment_striping;
	bool register_verbs;
	bool services_registered;
	bool verbs_registered;
	bool native_control_registered;
	bool native_control_source_aware;
	bool native_legacy_multicable_warned;
	bool apple_rails_wait_tbnet;
	bool apple_rails_pending;
	struct work_struct apple_rail_work;
	struct workqueue_struct *workqueue;
	struct notifier_block ibdev_netdev_nb;
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
	atomic64_t data_wr_no_recv_credit;
	atomic64_t data_wr_copied;
	atomic64_t data_wr_zcopy;
	atomic64_t data_wr_zcopy_fallback;
	atomic64_t data_wr_zcopy_fallback_striping;
	atomic64_t data_wr_zcopy_fallback_unsafe_sge;
	atomic64_t data_wr_copy_error;
	atomic64_t data_wr_path_send;
	atomic64_t data_wr_path_send_error;
	atomic64_t data_wr_retransmit;
	atomic64_t data_wr_rnr_retransmit;
	atomic64_t data_wr_retry_enqueue_error;
	atomic64_t data_wr_retry_exhausted;
	atomic64_t data_wr_rnr_retry_exhausted;
	atomic64_t data_wr_timeout;
	atomic64_t apple_sq_queued;
	atomic64_t apple_sq_dequeued;
	atomic64_t apple_sq_full;
	atomic64_t apple_sq_flushed;
	atomic64_t data_tx_accepted;
	atomic64_t data_tx_posted;
	atomic64_t data_tx_completed;
	atomic64_t data_tx_canceled;
	atomic64_t data_tx_errors;
	atomic64_t data_tx_credit_stalls;
	atomic64_t data_tx_credit_received;
	atomic64_t data_rx_completed;
	atomic64_t data_rx_canceled;
	atomic64_t data_rx_credit_sent;
	atomic64_t data_rx_credit_send_error;
	atomic64_t data_rx_repost_failed;
	atomic64_t data_rx_bad_frame;
	atomic64_t data_rx_bad_header;
	atomic64_t data_rx_bad_header_parse;
	atomic64_t data_rx_bad_header_len;
	atomic64_t data_rx_bad_header_path_credit;
	atomic64_t data_rx_bad_header_opcode;
	atomic64_t data_rx_bad_header_recv_credit;
	atomic64_t data_rx_bad_header_ack;
	atomic64_t data_rx_bad_header_write;
	atomic64_t data_rx_bad_header_read_req;
	atomic64_t data_rx_bad_header_mad;
	atomic64_t data_rx_send;
	atomic64_t data_rx_op_send;
	atomic64_t data_rx_op_send_imm;
	atomic64_t data_rx_op_write;
	atomic64_t data_rx_op_write_imm;
	atomic64_t data_rx_ack;
	atomic64_t data_rx_ack_matched;
	atomic64_t data_rx_ack_match_retried;
	atomic64_t data_rx_ack_match_max_ms;
	atomic64_t data_rx_ack_match_current_max_ms;
	atomic64_t data_rx_ack_match_over_10ms;
	atomic64_t data_rx_ack_match_over_64ms;
	atomic64_t data_rx_ack_miss;
	atomic64_t data_rx_late_ack;
	atomic64_t data_rx_ack_cumulative;
	atomic64_t data_tx_ack_ok;
	atomic64_t data_tx_ack_rnr;
	atomic64_t data_tx_ack_error;
	atomic64_t data_tx_ack_send_error;
	atomic64_t data_rx_ack_rnr;
	atomic64_t data_rx_duplicate_ack;
	atomic64_t data_rx_ack_history_miss;
	atomic64_t data_tx_read_ack_ok;
	atomic64_t data_tx_read_ack_retry;
	atomic64_t data_tx_read_ack_error;
	atomic64_t data_rx_read_ack_ok;
	atomic64_t data_rx_read_ack_retry;
	atomic64_t data_rx_read_ack_error;
	atomic64_t data_read_resp_retransmit;
	atomic64_t data_read_resp_drop;
	atomic64_t data_rx_read_resp_duplicate;
	atomic64_t data_rx_read_resp_gap;
	atomic64_t data_rx_read_resp_remote_error;
	atomic64_t data_rx_read_resp_bad_header;
	atomic64_t data_rx_read_resp_copy_error;
	atomic64_t data_rx_read_resp_short;
	atomic64_t data_rx_read_req_no_access;
	atomic64_t data_rx_read_req_no_mr;
	atomic64_t data_rx_read_req_mr_access;
	atomic64_t data_rx_read_req_too_large;
	atomic64_t data_rx_read_req_bad_iova;
	atomic64_t data_rx_read_req_alloc_error;
	atomic64_t data_rx_read_req_resp_busy;
	atomic64_t data_rx_read_req_resp_error;
	atomic64_t data_rx_no_qp;
	atomic64_t data_rx_bad_peer;
	atomic64_t data_rx_unconnected_qp;
	atomic64_t data_rx_qp_error;
	atomic64_t data_rx_no_recv;
	atomic64_t data_rx_rnr;
	atomic64_t data_rx_rnr_suppressed;
	atomic64_t data_rx_copy_error;
	atomic64_t data_rx_send_len_error;
	atomic64_t data_rx_send_prot_error;
	atomic64_t data_rx_send_cq_error;
	atomic64_t data_rx_send_bad_fragment;
	atomic64_t data_rx_send_sequence_error;
	atomic64_t data_rx_active_timeout;
	atomic64_t data_rx_reorder_buffered;
	atomic64_t data_rx_reorder_delivered;
	atomic64_t data_rx_reorder_dropped;
	atomic64_t data_rx_reorder_timeout;
	atomic64_t data_rx_reorder_window;
	atomic64_t data_rx_pending_discarded;
	atomic64_t apple_rx_sof;
	atomic64_t apple_rx_eof3;
	atomic64_t apple_rx_eof_other;
	atomic64_t apple_rx_sof_while_active;
	atomic64_t apple_rx_no_sof_when_idle;
	atomic64_t apple_rx_eof_without_active;
	atomic64_t apple_rx_len_overrun;
	atomic64_t apple_rx_raw_crc_error;
	atomic64_t apple_rx_resync_dropped;
	atomic64_t apple_rx_rail_mismatch;
	atomic64_t apple_rx_cq_overflow;
	atomic64_t data_cq_overflow;
	atomic64_t native_legacy_ambiguous_limited;
	struct xarray verbs_mrs_xa;
	struct xarray verbs_qps_xa;
	/*
	 * Serializes per-rail ib_device registration against teardown.
	 * tbv_ibdev_rail_event() publishes one ib_device per active rail as
	 * its data path comes up; tbv_peer_remove_rail() and module-exit
	 * tear them down. Kept separate from state->lock so the sleeping
	 * ib_(un)register_device path doesn't invert against verbs ops that
	 * take state->lock.
	 */
	struct mutex rail_register_lock;
	struct work_struct ibdev_netdev_retry_work;
	/*
	 * Up-event gate, owned by rail_register_lock. Set to true by
	 * tbv_ibdev_start() before any rising-edge events may publish; cleared
	 * by tbv_ibdev_stop() before draining so no late ready-edge event
	 * sneaks a fresh ib_device past module exit. Down events ignore this
	 * flag so existing devices can always be torn down.
	 */
	bool register_enabled;
	bool ibdev_netdev_nb_registered;
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
/*
 * Notify the verbs layer that rail's data path has come up (joined=true) or
 * is about to be torn down (joined=false). Safe to call repeatedly; only the
 * rising/falling edge of "ibdev published" causes registration changes.
 *
 * Up events are gated on state->register_enabled (flipped off by
 * tbv_ibdev_stop()). Down events are unconditional so module-exit and rail
 * remove can always undo a published device. Returns 0 on success/no-op, or a
 * negative errno if an up event failed to publish (the rail is then
 * permanently marked failed and will be skipped on retry).
 */
int tbv_ibdev_rail_event(struct tbv_state *state, struct tbv_rail *rail,
			 bool joined);
void tbv_ibdev_rx_frame(struct tbv_state *state, struct tbv_path *rx_path,
			const void *data, u32 len);
void tbv_ibdev_rx_native_frame(struct tbv_state *state,
			       struct tbv_path *rx_path,
			       const struct tbv_native_data_header *hdr,
			       const void *payload);
void tbv_ibdev_rx_apple_frame(struct tbv_state *state,
			      const struct tbv_path *path,
			      const void *payload, u32 len, u8 sof, u8 eof);

int tbv_tbnet_identity_check_config(const struct tbv_resolved_config *cfg);
int tbv_tbnet_identity_prepare(struct tbv_tbnet_identity *identity,
			       const struct tbv_resolved_config *cfg,
			       const struct tbv_tbnet_identity_config *identity_cfg);
void tbv_tbnet_identity_stop(struct tbv_tbnet_identity *identity);
int tbv_tbip_build_login(void *buf, size_t size,
			 const struct tbv_tbip_login_params *params);
int tbv_tbip_build_login_response(void *buf, size_t size,
				  const struct tbv_tbip_login_response_params *params);
int tbv_tbip_build_logout(void *buf, size_t size,
			  const struct tbv_tbip_control *ctrl);
int tbv_tbip_build_status(void *buf, size_t size,
			  const struct tbv_tbip_status_params *params);
int tbv_tbip_parse_type(const void *buf, size_t size,
			enum tbv_tbip_type *type,
			struct tbv_tbip_control *ctrl);
int tbv_tbip_parse_login(const void *buf, size_t size,
			 struct tbv_tbip_login_params *params);
int tbv_tbip_parse_login_response(const void *buf, size_t size,
				  struct tbv_tbip_login_response_result *result);
int tbv_tbip_parse_status(const void *buf, size_t size,
			  struct tbv_tbip_status_result *result);
int tbv_tbnet_arp_reply_for_request(void *reply, size_t reply_size,
				    const void *request, size_t request_size,
				    const struct tbv_tbnet_arp_proxy *proxy);
int tbv_tbnet_minimal_start(struct tbv_tbnet_identity *identity);
void tbv_tbnet_minimal_stop(struct tbv_tbnet_identity *identity);
void tbv_tbnet_minimal_recompute_state_locked(struct tbv_tbnet_identity *identity);
bool tbv_tbnet_minimal_neighbor_ready(struct tbv_tbnet_identity *identity,
				      const uuid_t *remote_uuid);
bool tbv_tbnet_minimal_path_ready(struct tbv_tbnet_identity *identity,
				  const uuid_t *remote_uuid);
void tbv_tbnet_minimal_clear_neighbors_locked(struct tbv_tbnet_identity *identity);
void tbv_tbnet_minimal_debugfs_show(struct seq_file *s,
				    struct tbv_tbnet_identity *identity);
void tbv_services_tbnet_identity_ready(struct tbv_tbnet_identity *identity);
struct tb_property_dir *tbv_service_create_native_dir(void);
struct tb_property_dir *tbv_service_create_apple_dir(u32 prtcstns);
int tbv_services_start(struct tbv_state *state, bool bind_services,
		       const struct tbv_service_config *service_cfg);
void tbv_services_stop(struct tbv_state *state);
int tbv_native_control_start(struct tbv_state *state);
void tbv_native_control_stop(struct tbv_state *state);
const char *tbv_native_control_mode_name(const struct tbv_state *state);
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
				   const struct tbv_rail_key *key,
				   u32 native_lane);
void tbv_peer_remove_rail(struct tbv_rail *rail);
void tbv_rail_put(struct tbv_rail *rail);
bool tbv_path_apple_tx_raw_mode(void);
bool tbv_path_apple_rx_raw_mode(void);
void tbv_path_default_config(enum tbv_backend_type backend,
			     struct tbv_path_config *cfg);
void tbv_path_init_optional_symbols(void);
void tbv_path_exit_optional_symbols(void);
void tbv_path_init(struct tbv_path *path,
		   const struct tbv_path_config *cfg, struct tbv_rail *rail);
void tbv_path_reset(struct tbv_path *path);
const char *tbv_path_state_name(enum tbv_path_state state);
int tbv_path_alloc_rings(struct tbv_path *path, struct tb_xdomain *xd,
			 int requested_transmit_path);
int tbv_path_start_rings(struct tbv_path *path);
int tbv_path_enable_tunnel(struct tbv_path *path, struct tb_xdomain *xd,
			   int remote_transmit_path);
int tbv_path_disable_tunnel(struct tbv_path *path, struct tb_xdomain *xd);
void tbv_path_set_remote_rx_capacity(struct tbv_path *path, u32 rx_ring_size);
void tbv_path_add_remote_rx_credits(struct tbv_path *path, u32 credits);
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
int tbv_path_send_owned_list_reserved(struct tbv_path *path,
				      struct list_head *frames,
				      unsigned int flags,
				      tbv_path_tx_done_fn done,
				      void *done_ctx);
int tbv_path_prepare_owned_list(struct tbv_path *path,
				struct list_head *frames,
				struct list_head *packets,
				u32 *packet_count,
				unsigned int flags,
				tbv_path_tx_done_fn done,
				void *done_ctx);
int tbv_path_enqueue_prepared_reserved(struct tbv_path *path,
				       struct list_head *packets,
				       u32 packet_count,
				       unsigned int flags);
void tbv_path_release_prepared_list_silent(struct list_head *packets,
					   int status);
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
void tbv_path_cancel_data_done_ctx(struct tbv_path *path,
				   tbv_path_tx_done_fn done, void *done_ctx);
void tbv_path_cancel_data_owner_ctx(struct tbv_path *path, void *owner_ctx);
void tbv_path_destroy(struct tbv_path *path, struct tb_xdomain *xd);

const struct tbv_backend_ops *tbv_backend_get(enum tbv_backend_type type);
int tbv_link_activate_config(struct tbv_state *state, const char *name,
			     enum tbv_backend_type backend,
			     const struct tbv_cfg_link *cfg);
void tbv_link_deactivate_config(struct tbv_state *state, u32 link_id);
u32 tbv_link_count(struct tbv_state *state);
void tbv_link_debugfs_show(struct seq_file *s, struct tbv_state *state);
int tbv_debugfs_init(struct tbv_state *state);
void tbv_debugfs_exit(struct tbv_state *state);
int tbv_configfs_start(struct tbv_state *state);
void tbv_configfs_stop(struct tbv_state *state);

int tbv_core_init(struct tbv_state *state,
		  const struct tbv_resolved_config *cfg,
		  const struct tbv_tbnet_identity_config *identity_cfg);
void tbv_core_exit(struct tbv_state *state);

#endif
