// SPDX-License-Identifier: GPL-2.0

#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/thunderbolt.h>

#include "../proto/native_wire.h"
#include "tbv.h"

static u32 tbv_debugfs_wire_path_flags(const struct tbv_path *path)
{
	u32 flags = 0;

	if (path->cfg.tx_flags & RING_FLAG_FRAME)
		flags |= TBV_NATIVE_WIRE_PATH_FRAME;
	if (path->cfg.e2e)
		flags |= TBV_NATIVE_WIRE_PATH_E2E;

	return flags;
}

static u32 tbv_debugfs_list_count(const struct list_head *head)
{
	const struct list_head *pos;
	u32 count = 0;

	list_for_each(pos, head)
		count++;
	return count;
}

static int tbv_debugfs_ring_hop(const struct tb_ring *ring)
{
	return ring ? ring->hop : -1;
}

static unsigned int tbv_debugfs_ring_flags(const struct tb_ring *ring)
{
	return ring ? ring->flags : 0;
}

static int tbv_debugfs_ring_e2e_tx_hop(const struct tb_ring *ring)
{
	return ring ? ring->e2e_tx_hop : -1;
}

static int tbv_debugfs_summary_show(struct seq_file *s, void *unused)
{
	struct tbv_state *state = s->private;

	seq_printf(s, "profile: %s\n", tbv_profile_name(state->cfg.profile));
	seq_printf(s, "native_enabled: %u\n", state->cfg.native_enabled);
	seq_printf(s, "apple_enabled: %u\n", state->cfg.apple_enabled);
	seq_printf(s, "rc_supported: %u\n", state->cfg.rc_supported);
	seq_printf(s, "uc_supported: %u\n", state->cfg.uc_supported);
	seq_printf(s, "native_control: %s\n",
		   tbv_native_control_mode_name(state));
	seq_printf(s, "native_same_peer_multicable: %s\n",
		   !state->cfg.native_enabled ||
		   !state->native_control_registered ? "off" :
		   state->native_single_peer ? "forced" :
		   state->native_control_source_aware ? "enabled" :
						       "limited");
	seq_printf(s, "native_single_peer: %u\n",
		   state->native_single_peer);
	seq_printf(s, "native_link_speed_filter: %u\n",
		   state->native_link_speed_filter);
	seq_printf(s, "native_legacy_ambiguous_limited: %lld\n",
		   atomic64_read(&state->native_legacy_ambiguous_limited));
	seq_printf(s, "configured_links: %u\n", tbv_link_count(state));
	seq_printf(s, "tbnet_identity: %s\n",
		   tbv_tbnet_identity_name(state->cfg.tbnet_identity));
	mutex_lock(&state->tbnet_identity.lock);
	seq_printf(s, "tbnet_identity_state: 0x%lx\n",
		   state->tbnet_identity.state);
	seq_printf(s, "tbnet_identity_tbnet: %s\n",
		   state->tbnet_identity.tbnet_netdev_name[0] ?
		   state->tbnet_identity.tbnet_netdev_name : "<unset>");
	seq_printf(s, "tbnet_identity_gid: %s\n",
		   state->tbnet_identity.gid_netdev_name[0] ?
		   state->tbnet_identity.gid_netdev_name : "<unset>");
	seq_printf(s, "tbnet_identity_proxy_ipv4: %pI4\n",
		   &state->tbnet_identity.proxy_ipv4);
	seq_printf(s, "tbnet_identity_minimal_e2e: %u\n",
		   state->tbnet_identity.minimal_e2e);
	seq_printf(s, "tbnet_identity_minimal_apple_only: %u\n",
		   state->tbnet_identity.minimal_apple_only);
	seq_printf(s, "tbnet_identity_minimal_neighbor_seen: %u\n",
		   state->tbnet_identity.minimal_neighbor_seen);
	seq_printf(s, "tbnet_identity_rx_handler: %u\n",
		   state->tbnet_identity.rx_handler_registered);
	seq_printf(s, "tbnet_identity_minimal_started: %u\n",
		   state->tbnet_identity.minimal_started);
	seq_printf(s, "tbnet_identity_minimal_login_rx: %lld\n",
		   atomic64_read(&state->tbnet_identity.minimal_login_rx));
	seq_printf(s, "tbnet_identity_minimal_login_tx: %lld\n",
		   atomic64_read(&state->tbnet_identity.minimal_login_tx));
	seq_printf(s, "tbnet_identity_minimal_logout_rx: %lld\n",
		   atomic64_read(&state->tbnet_identity.minimal_logout_rx));
	seq_printf(s, "tbnet_identity_minimal_logout_tx: %lld\n",
		   atomic64_read(&state->tbnet_identity.minimal_logout_tx));
	seq_printf(s, "tbnet_identity_minimal_status_rx: %lld\n",
		   atomic64_read(&state->tbnet_identity.minimal_status_rx));
	seq_printf(s, "tbnet_identity_minimal_status_tx: %lld\n",
		   atomic64_read(&state->tbnet_identity.minimal_status_tx));
	seq_printf(s, "tbnet_identity_minimal_packet_rx: %lld\n",
		   atomic64_read(&state->tbnet_identity.minimal_packet_rx));
	seq_printf(s, "tbnet_identity_minimal_packet_tx_posted: %lld\n",
		   atomic64_read(&state->tbnet_identity.minimal_packet_tx_posted));
	seq_printf(s, "tbnet_identity_minimal_packet_tx: %lld\n",
		   atomic64_read(&state->tbnet_identity.minimal_packet_tx));
	seq_printf(s, "tbnet_identity_minimal_packet_tx_errors: %lld\n",
		   atomic64_read(&state->tbnet_identity.minimal_packet_tx_errors));
	seq_printf(s, "tbnet_identity_minimal_path_errors: %lld\n",
		   atomic64_read(&state->tbnet_identity.minimal_path_errors));
	seq_printf(s, "tbnet_identity_arp_requests: %lld\n",
		   atomic64_read(&state->tbnet_identity.arp_requests));
	seq_printf(s, "tbnet_identity_arp_replies: %lld\n",
		   atomic64_read(&state->tbnet_identity.arp_replies));
	seq_printf(s, "tbnet_identity_arp_ignored: %lld\n",
		   atomic64_read(&state->tbnet_identity.arp_ignored));
	seq_printf(s, "tbnet_identity_arp_errors: %lld\n",
		   atomic64_read(&state->tbnet_identity.arp_errors));
	tbv_tbnet_minimal_debugfs_show(s, &state->tbnet_identity);
	mutex_unlock(&state->tbnet_identity.lock);
	seq_printf(s, "tbnet_policy: %s\n",
		   tbv_tbnet_policy_name(state->cfg.requested.tbnet));
	seq_printf(s, "native_service_count: %u\n",
		   state->native_dir_count);
	seq_printf(s, "apple_service_registered: %u\n",
		   !!state->apple_dir);
	seq_printf(s, "services_registered: %u\n",
		   state->services_registered);
	seq_printf(s, "allocate_rings: %u\n", state->allocate_rings);
	seq_printf(s, "start_rings: %u\n", state->start_rings);
	seq_printf(s, "negotiate_native: %u\n", state->negotiate_native);
	seq_printf(s, "enable_tunnels: %u\n", state->enable_tunnels);
	seq_printf(s, "apple_tunnels_wait_tbnet: %u\n",
		   state->apple_tunnels_wait_tbnet);
	seq_printf(s, "apple_tunnels_pending: %u\n",
		   state->apple_tunnels_pending);
	seq_printf(s, "native_data: %u\n", state->native_data);
	seq_printf(s, "apple_data: %u\n", state->apple_data);
	seq_printf(s, "native_fragment_striping: %u\n",
		   state->native_fragment_striping);
	seq_printf(s, "register_verbs: %u\n", state->register_verbs);
	seq_printf(s, "verbs_registered: %u\n", state->verbs_registered);
	seq_printf(s, "verbs_ucontexts: %d\n",
		   atomic_read(&state->verbs_ucontexts));
	seq_printf(s, "verbs_pds: %d\n", atomic_read(&state->verbs_pds));
	seq_printf(s, "verbs_cqs: %d\n", atomic_read(&state->verbs_cqs));
	seq_printf(s, "verbs_qps: %d\n", atomic_read(&state->verbs_qps));
	seq_printf(s, "verbs_mrs: %d\n", atomic_read(&state->verbs_mrs));
	seq_printf(s, "verbs_recv_wqes: %d\n",
		   atomic_read(&state->verbs_recv_wqes));
	seq_printf(s, "dv_poll_running: %u\n",
		   !!READ_ONCE(state->dv_poll_task));
	seq_printf(s, "dv_poll_qps: %u\n",
		   READ_ONCE(state->dv_poll_qp_count));
	seq_printf(s, "dv_poll_scans: %lld\n",
		   atomic64_read(&state->dv_poll_scans));
	seq_printf(s, "dv_poll_wqes: %lld\n",
		   atomic64_read(&state->dv_poll_wqes));
	seq_printf(s, "dv_poll_budget_exhausted: %lld\n",
		   atomic64_read(&state->dv_poll_budget_exhausted));
	seq_printf(s, "dv_poll_errors: %lld\n",
		   atomic64_read(&state->dv_poll_errors));
	seq_printf(s, "dv_admission_attempts: %lld\n",
		   atomic64_read(&state->dv_admission_attempts));
	seq_printf(s, "dv_backpressure_retry: %lld\n",
		   atomic64_read(&state->dv_backpressure_retry));
	seq_printf(s, "dv_fence_retry: %lld\n",
		   atomic64_read(&state->dv_fence_retry));
	seq_printf(s, "dv_hard_error: %lld\n",
		   atomic64_read(&state->dv_hard_error));
	seq_printf(s, "data_wr_send: %lld\n",
		   atomic64_read(&state->data_wr_send));
	seq_printf(s, "data_wr_op_send: %lld\n",
		   atomic64_read(&state->data_wr_op_send));
	seq_printf(s, "data_wr_op_send_imm: %lld\n",
		   atomic64_read(&state->data_wr_op_send_imm));
	seq_printf(s, "data_wr_op_write: %lld\n",
		   atomic64_read(&state->data_wr_op_write));
	seq_printf(s, "data_wr_op_write_imm: %lld\n",
		   atomic64_read(&state->data_wr_op_write_imm));
	seq_printf(s, "data_wr_op_unsupported: %lld\n",
		   atomic64_read(&state->data_wr_op_unsupported));
	seq_printf(s, "data_wr_live: %lld\n",
		   atomic64_read(&state->data_wr_live));
	seq_printf(s, "data_wr_no_path: %lld\n",
		   atomic64_read(&state->data_wr_no_path));
	seq_printf(s, "data_wr_no_recv_credit: %lld\n",
		   atomic64_read(&state->data_wr_no_recv_credit));
	seq_printf(s, "data_wr_copied: %lld\n",
		   atomic64_read(&state->data_wr_copied));
	seq_printf(s, "data_wr_zcopy: %lld\n",
		   atomic64_read(&state->data_wr_zcopy));
	seq_printf(s, "data_wr_zcopy_fallback: %lld\n",
		   atomic64_read(&state->data_wr_zcopy_fallback));
	seq_printf(s, "data_wr_zcopy_fallback_striping: %lld\n",
		   atomic64_read(&state->data_wr_zcopy_fallback_striping));
	seq_printf(s, "data_wr_zcopy_fallback_unsafe_sge: %lld\n",
		   atomic64_read(&state->data_wr_zcopy_fallback_unsafe_sge));
	seq_printf(s, "data_wr_copy_error: %lld\n",
		   atomic64_read(&state->data_wr_copy_error));
	seq_printf(s, "data_wr_path_send: %lld\n",
		   atomic64_read(&state->data_wr_path_send));
	seq_printf(s, "data_wr_path_send_error: %lld\n",
		   atomic64_read(&state->data_wr_path_send_error));
	seq_printf(s, "data_wr_retransmit: %lld\n",
		   atomic64_read(&state->data_wr_retransmit));
	seq_printf(s, "data_wr_rnr_retransmit: %lld\n",
		   atomic64_read(&state->data_wr_rnr_retransmit));
	seq_printf(s, "data_wr_retransmit_closing_qp: %lld\n",
		   atomic64_read(&state->data_wr_retransmit_closing_qp));
	seq_printf(s, "data_wr_retransmit_no_live_path: %lld\n",
		   atomic64_read(&state->data_wr_retransmit_no_live_path));
	seq_printf(s, "data_wr_retransmit_teardown_path: %lld\n",
		   atomic64_read(&state->data_wr_retransmit_teardown_path));
	seq_printf(s, "data_wr_retry_enqueue_error: %lld\n",
		   atomic64_read(&state->data_wr_retry_enqueue_error));
	seq_printf(s, "data_wr_retry_exhausted: %lld\n",
		   atomic64_read(&state->data_wr_retry_exhausted));
	seq_printf(s, "data_wr_rnr_retry_exhausted: %lld\n",
		   atomic64_read(&state->data_wr_rnr_retry_exhausted));
	seq_printf(s, "data_wr_timeout: %lld\n",
		   atomic64_read(&state->data_wr_timeout));
	seq_printf(s, "data_wr_send_timeout: %lld\n",
		   atomic64_read(&state->data_wr_send_timeout));
	seq_printf(s, "data_wr_timeout_last_psn: %lld\n",
		   atomic64_read(&state->data_wr_timeout_last_psn));
	seq_printf(s, "apple_sq_queued: %lld\n",
		   atomic64_read(&state->apple_sq_queued));
	seq_printf(s, "apple_sq_dequeued: %lld\n",
		   atomic64_read(&state->apple_sq_dequeued));
	seq_printf(s, "apple_sq_full: %lld\n",
		   atomic64_read(&state->apple_sq_full));
	seq_printf(s, "apple_sq_flushed: %lld\n",
		   atomic64_read(&state->apple_sq_flushed));
	seq_printf(s, "data_tx_accepted: %lld\n",
		   atomic64_read(&state->data_tx_accepted));
	seq_printf(s, "data_tx_posted: %lld\n",
		   atomic64_read(&state->data_tx_posted));
	seq_printf(s, "data_tx_completed: %lld\n",
		   atomic64_read(&state->data_tx_completed));
	seq_printf(s, "data_tx_canceled: %lld\n",
		   atomic64_read(&state->data_tx_canceled));
	seq_printf(s, "data_tx_errors: %lld\n",
		   atomic64_read(&state->data_tx_errors));
	seq_printf(s, "data_tx_credit_stalls: %lld\n",
		   atomic64_read(&state->data_tx_credit_stalls));
	seq_printf(s, "data_tx_credit_received: %lld\n",
		   atomic64_read(&state->data_tx_credit_received));
	seq_printf(s, "data_rx_completed: %lld\n",
		   atomic64_read(&state->data_rx_completed));
	seq_printf(s, "data_rx_canceled: %lld\n",
		   atomic64_read(&state->data_rx_canceled));
	seq_printf(s, "data_rx_credit_sent: %lld\n",
		   atomic64_read(&state->data_rx_credit_sent));
	seq_printf(s, "data_rx_credit_send_error: %lld\n",
		   atomic64_read(&state->data_rx_credit_send_error));
	seq_printf(s, "data_rx_repost_failed: %lld\n",
		   atomic64_read(&state->data_rx_repost_failed));
	seq_printf(s, "native_tx_data: %lld\n",
		   atomic64_read(&state->native_tx_data));
	seq_printf(s, "native_tx_send_ack: %lld\n",
		   atomic64_read(&state->native_tx_send_ack));
	seq_printf(s, "native_tx_recv_credit: %lld\n",
		   atomic64_read(&state->native_tx_recv_credit));
	seq_printf(s, "native_tx_read_ack: %lld\n",
		   atomic64_read(&state->native_tx_read_ack));
	seq_printf(s, "native_tx_read_req: %lld\n",
		   atomic64_read(&state->native_tx_read_req));
	seq_printf(s, "native_tx_read_resp: %lld\n",
		   atomic64_read(&state->native_tx_read_resp));
	seq_printf(s, "native_rx_data: %lld\n",
		   atomic64_read(&state->native_rx_data));
	seq_printf(s, "native_rx_send_ack: %lld\n",
		   atomic64_read(&state->native_rx_send_ack));
	seq_printf(s, "native_rx_recv_credit: %lld\n",
		   atomic64_read(&state->native_rx_recv_credit));
	seq_printf(s, "native_rx_read_ack: %lld\n",
		   atomic64_read(&state->native_rx_read_ack));
	seq_printf(s, "native_rx_read_req: %lld\n",
		   atomic64_read(&state->native_rx_read_req));
	seq_printf(s, "native_rx_read_resp: %lld\n",
		   atomic64_read(&state->native_rx_read_resp));
	seq_printf(s, "data_rx_bad_frame: %lld\n",
		   atomic64_read(&state->data_rx_bad_frame));
	seq_printf(s, "data_rx_bad_header: %lld\n",
		   atomic64_read(&state->data_rx_bad_header));
	seq_printf(s, "data_rx_send: %lld\n",
		   atomic64_read(&state->data_rx_send));
	seq_printf(s, "data_rx_op_send: %lld\n",
		   atomic64_read(&state->data_rx_op_send));
	seq_printf(s, "data_rx_op_send_imm: %lld\n",
		   atomic64_read(&state->data_rx_op_send_imm));
	seq_printf(s, "data_rx_op_write: %lld\n",
		   atomic64_read(&state->data_rx_op_write));
	seq_printf(s, "data_rx_op_write_imm: %lld\n",
		   atomic64_read(&state->data_rx_op_write_imm));
	seq_printf(s, "data_rx_ack: %lld\n",
		   atomic64_read(&state->data_rx_ack));
	seq_printf(s, "data_rx_ack_matched: %lld\n",
		   atomic64_read(&state->data_rx_ack_matched));
	seq_printf(s, "data_rx_ack_match_retried: %lld\n",
		   atomic64_read(&state->data_rx_ack_match_retried));
	seq_printf(s, "data_rx_ack_match_max_ms: %lld\n",
		   atomic64_read(&state->data_rx_ack_match_max_ms));
	seq_printf(s, "data_rx_ack_match_current_max_ms: %lld\n",
		   atomic64_read(&state->data_rx_ack_match_current_max_ms));
	seq_printf(s, "data_rx_ack_match_over_10ms: %lld\n",
		   atomic64_read(&state->data_rx_ack_match_over_10ms));
	seq_printf(s, "data_rx_ack_match_over_64ms: %lld\n",
		   atomic64_read(&state->data_rx_ack_match_over_64ms));
	seq_printf(s, "data_rx_ack_miss: %lld\n",
		   atomic64_read(&state->data_rx_ack_miss));
	seq_printf(s, "data_rx_late_ack: %lld\n",
		   atomic64_read(&state->data_rx_late_ack));
	seq_printf(s, "data_rx_ack_cumulative: %lld\n",
		   atomic64_read(&state->data_rx_ack_cumulative));
	seq_printf(s, "data_tx_ack_ok: %lld\n",
		   atomic64_read(&state->data_tx_ack_ok));
	seq_printf(s, "data_tx_ack_rnr: %lld\n",
		   atomic64_read(&state->data_tx_ack_rnr));
	seq_printf(s, "data_tx_ack_error: %lld\n",
		   atomic64_read(&state->data_tx_ack_error));
	seq_printf(s, "data_tx_ack_send_error: %lld\n",
		   atomic64_read(&state->data_tx_ack_send_error));
	seq_printf(s, "data_tx_ack_drop_checked: %lld\n",
		   atomic64_read(&state->data_tx_ack_drop_checked));
	seq_printf(s, "data_tx_ack_drop_injected: %lld\n",
		   atomic64_read(&state->data_tx_ack_drop_injected));
	seq_printf(s, "data_rx_ack_rnr: %lld\n",
		   atomic64_read(&state->data_rx_ack_rnr));
	seq_printf(s, "data_rx_duplicate_ack: %lld\n",
		   atomic64_read(&state->data_rx_duplicate_ack));
	seq_printf(s, "data_rx_ack_history_miss: %lld\n",
		   atomic64_read(&state->data_rx_ack_history_miss));
	seq_printf(s, "data_rx_no_qp_reack: %lld\n",
		   atomic64_read(&state->data_rx_no_qp_reack));
	seq_printf(s, "data_rx_no_qp_error_ack: %lld\n",
		   atomic64_read(&state->data_rx_no_qp_error_ack));
	seq_printf(s, "data_qp_tombstone_evicted: %lld\n",
		   atomic64_read(&state->data_qp_tombstone_evicted));
	seq_printf(s, "data_tx_read_ack_ok: %lld\n",
		   atomic64_read(&state->data_tx_read_ack_ok));
	seq_printf(s, "data_tx_read_ack_retry: %lld\n",
		   atomic64_read(&state->data_tx_read_ack_retry));
	seq_printf(s, "data_tx_read_ack_error: %lld\n",
		   atomic64_read(&state->data_tx_read_ack_error));
	seq_printf(s, "data_rx_read_ack_ok: %lld\n",
		   atomic64_read(&state->data_rx_read_ack_ok));
	seq_printf(s, "data_rx_read_ack_retry: %lld\n",
		   atomic64_read(&state->data_rx_read_ack_retry));
	seq_printf(s, "data_rx_read_ack_error: %lld\n",
		   atomic64_read(&state->data_rx_read_ack_error));
	seq_printf(s, "data_read_resp_retransmit: %lld\n",
		   atomic64_read(&state->data_read_resp_retransmit));
	seq_printf(s, "data_read_resp_drop: %lld\n",
		   atomic64_read(&state->data_read_resp_drop));
	seq_printf(s, "data_rx_read_resp_duplicate: %lld\n",
		   atomic64_read(&state->data_rx_read_resp_duplicate));
	seq_printf(s, "data_rx_read_resp_gap: %lld\n",
		   atomic64_read(&state->data_rx_read_resp_gap));
	seq_printf(s, "data_rx_read_resp_remote_error: %lld\n",
		   atomic64_read(&state->data_rx_read_resp_remote_error));
	seq_printf(s, "data_rx_read_resp_bad_header: %lld\n",
		   atomic64_read(&state->data_rx_read_resp_bad_header));
	seq_printf(s, "data_rx_read_resp_copy_error: %lld\n",
		   atomic64_read(&state->data_rx_read_resp_copy_error));
	seq_printf(s, "data_rx_read_resp_short: %lld\n",
		   atomic64_read(&state->data_rx_read_resp_short));
	seq_printf(s, "data_rx_read_req_no_access: %lld\n",
		   atomic64_read(&state->data_rx_read_req_no_access));
	seq_printf(s, "data_rx_read_req_no_mr: %lld\n",
		   atomic64_read(&state->data_rx_read_req_no_mr));
	seq_printf(s, "data_rx_read_req_mr_access: %lld\n",
		   atomic64_read(&state->data_rx_read_req_mr_access));
	seq_printf(s, "data_rx_read_req_too_large: %lld\n",
		   atomic64_read(&state->data_rx_read_req_too_large));
	seq_printf(s, "data_rx_read_req_bad_iova: %lld\n",
		   atomic64_read(&state->data_rx_read_req_bad_iova));
	seq_printf(s, "data_rx_read_req_alloc_error: %lld\n",
		   atomic64_read(&state->data_rx_read_req_alloc_error));
	seq_printf(s, "data_rx_read_req_resp_busy: %lld\n",
		   atomic64_read(&state->data_rx_read_req_resp_busy));
	seq_printf(s, "data_rx_read_req_resp_error: %lld\n",
		   atomic64_read(&state->data_rx_read_req_resp_error));
	seq_printf(s, "data_rx_no_qp: %lld\n",
		   atomic64_read(&state->data_rx_no_qp));
	seq_printf(s, "data_rx_bad_peer: %lld\n",
		   atomic64_read(&state->data_rx_bad_peer));
	seq_printf(s, "data_rx_unconnected_qp: %lld\n",
		   atomic64_read(&state->data_rx_unconnected_qp));
	seq_printf(s, "data_rx_qp_error: %lld\n",
		   atomic64_read(&state->data_rx_qp_error));
	seq_printf(s, "data_rx_no_recv: %lld\n",
		   atomic64_read(&state->data_rx_no_recv));
	seq_printf(s, "data_rx_rnr: %lld\n",
		   atomic64_read(&state->data_rx_rnr));
	seq_printf(s, "data_rx_rnr_suppressed: %lld\n",
		   atomic64_read(&state->data_rx_rnr_suppressed));
	seq_printf(s, "data_rx_copy_error: %lld\n",
		   atomic64_read(&state->data_rx_copy_error));
	seq_printf(s, "data_rx_send_len_error: %lld\n",
		   atomic64_read(&state->data_rx_send_len_error));
	seq_printf(s, "data_rx_send_prot_error: %lld\n",
		   atomic64_read(&state->data_rx_send_prot_error));
	seq_printf(s, "data_rx_send_cq_error: %lld\n",
		   atomic64_read(&state->data_rx_send_cq_error));
	seq_printf(s, "data_rx_send_bad_fragment: %lld\n",
		   atomic64_read(&state->data_rx_send_bad_fragment));
	seq_printf(s, "data_rx_send_sequence_error: %lld\n",
		   atomic64_read(&state->data_rx_send_sequence_error));
	seq_printf(s, "data_rx_active_timeout: %lld\n",
		   atomic64_read(&state->data_rx_active_timeout));
	seq_printf(s, "data_rx_reorder_buffered: %lld\n",
		   atomic64_read(&state->data_rx_reorder_buffered));
	seq_printf(s, "data_rx_reorder_delivered: %lld\n",
		   atomic64_read(&state->data_rx_reorder_delivered));
	seq_printf(s, "data_rx_reorder_dropped: %lld\n",
		   atomic64_read(&state->data_rx_reorder_dropped));
	seq_printf(s, "data_rx_reorder_timeout: %lld\n",
		   atomic64_read(&state->data_rx_reorder_timeout));
	seq_printf(s, "data_rx_reorder_window: %lld\n",
		   atomic64_read(&state->data_rx_reorder_window));
	seq_printf(s, "data_rx_pending_discarded: %lld\n",
		   atomic64_read(&state->data_rx_pending_discarded));
	seq_printf(s, "apple_rx_sof: %lld\n",
		   atomic64_read(&state->apple_rx_sof));
	seq_printf(s, "apple_rx_eof3: %lld\n",
		   atomic64_read(&state->apple_rx_eof3));
	seq_printf(s, "apple_rx_eof_other: %lld\n",
		   atomic64_read(&state->apple_rx_eof_other));
	seq_printf(s, "apple_rx_sof_while_active: %lld\n",
		   atomic64_read(&state->apple_rx_sof_while_active));
	seq_printf(s, "apple_rx_no_sof_when_idle: %lld\n",
		   atomic64_read(&state->apple_rx_no_sof_when_idle));
	seq_printf(s, "apple_rx_eof_without_active: %lld\n",
		   atomic64_read(&state->apple_rx_eof_without_active));
	seq_printf(s, "apple_rx_len_overrun: %lld\n",
		   atomic64_read(&state->apple_rx_len_overrun));
	seq_printf(s, "data_cq_overflow: %lld\n",
		   atomic64_read(&state->data_cq_overflow));
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(tbv_debugfs_summary);

static int tbv_debugfs_peers_show(struct seq_file *s, void *unused)
{
	struct tbv_state *state = s->private;
	struct tbv_peer *peer;

	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		seq_printf(s, "peer %u backend=%s rails=%u native_qp_rr_rail_id=%u\n",
			   peer->peer_id, tbv_backend_name(peer->backend),
			   peer->nr_rails, peer->native_qp_rr_rail_id);

		list_for_each_entry(rail, &peer->rails, node) {
			struct tbv_path *path = &rail->path;
			unsigned long flags;
			u32 tx_control_queued;
			u32 tx_data_queued;
			u32 tx_data_reserved;
			u32 tx_remote_data_credits;
			u32 tx_remote_data_credit_max;
			u32 tx_free;
			u32 tx_zcopy_inflight;
			u32 rx_data_credit_pending;
			int tx_inflight;
			bool tx_scheduling;
			bool tx_raw_stream_active;
			bool data_ready;

			spin_lock_irqsave(&path->tx_lock, flags);
			tx_control_queued = path->tx_control_queued;
			tx_data_queued = path->tx_data_queued;
			tx_data_reserved = path->tx_data_reserved;
			tx_remote_data_credits = path->tx_remote_data_credits;
			tx_remote_data_credit_max =
				path->tx_remote_data_credit_max;
			tx_free = tbv_debugfs_list_count(&path->tx_free);
			tx_zcopy_inflight =
				tbv_debugfs_list_count(&path->tx_zcopy_inflight);
			tx_inflight = atomic_read(&path->tx_inflight);
			rx_data_credit_pending = path->rx_data_credit_pending;
			tx_scheduling = path->tx_scheduling;
			tx_raw_stream_active = path->tx_raw_stream_active;
			spin_unlock_irqrestore(&path->tx_lock, flags);

			data_ready = peer->backend == TBV_BACKEND_APPLE ?
				tbv_rail_apple_data_ready(rail) :
				tbv_rail_data_ready(rail);

			seq_printf(s,
				   "  rail=0x%x route=0x%llx local=%u remote=%u path=%u link_speed=%uGb/s link_width=0x%x active=%u data_ready=%u native_qp_bind_count=%d state=%s negotiated=%u ready_sent=%u remote_ready=%u attempts=%u last_error=%d local_out=%d local_tx=%d local_rx=%d remote_rail=0x%x remote_out=%d remote_tx=%d remote_rx=%d\n",
				   rail->rail_id, rail->key.route,
				   rail->key.local_adapter,
				   rail->key.remote_adapter,
				   rail->key.path_id,
				   rail->link_speed,
				   rail->link_width,
				   rail->active,
				   data_ready,
				   atomic_read(&rail->native_qp_bind_count),
				   tbv_path_state_name(rail->path.state),
				   rail->native_negotiated,
				   rail->native_ready_sent,
				   rail->native_remote_ready,
				   rail->native_attempts +
				   rail->native_ready_attempts,
				   rail->native_last_error,
				   rail->path.local_transmit_path,
				   rail->path.local_tx_hop,
				   rail->path.local_rx_hop,
				   rail->remote_rail_id,
				   rail->remote_transmit_path,
				   rail->remote_tx_hop,
				   rail->remote_rx_hop);
			seq_printf(s,
				   "    data_tx_enqueued=%lld data_tx_posted=%lld data_tx_completed=%lld data_tx_credit_stalls=%lld data_tx_credit_received=%lld tx_credits=%u/%u\n",
				   atomic64_read(&path->data_tx_enqueued),
				   atomic64_read(&path->data_tx_posted),
				   atomic64_read(&path->data_tx_completed),
				   atomic64_read(&path->data_tx_credit_stalls),
				   atomic64_read(&path->data_tx_credit_received),
				   tx_remote_data_credits,
				   tx_remote_data_credit_max);
			seq_printf(s,
				   "    path_tx ctrl_q=%u data_q=%u reserved=%u inflight=%d free=%u zcopy_inflight=%u scheduling=%u raw_active=%u\n",
				   tx_control_queued, tx_data_queued,
				   tx_data_reserved, tx_inflight, tx_free,
				   tx_zcopy_inflight, tx_scheduling,
				   tx_raw_stream_active);
			seq_printf(s,
				   "    control_tx_enqueued=%lld control_tx_posted=%lld control_tx_completed=%lld control_tx_queue_max_ms=%lld\n",
				   atomic64_read(&rail->path.control_tx_enqueued),
				   atomic64_read(&rail->path.control_tx_posted),
				   atomic64_read(&rail->path.control_tx_completed),
				   atomic64_read(&rail->path.control_tx_queue_max_ms));
			seq_printf(s,
				   "    path_cfg tx_flags=0x%x rx_flags=0x%x e2e=%u wire_flags=0x%x tx_ring=%u rx_ring=%u sof_mask=0x%x eof_mask=0x%x\n",
				   path->cfg.tx_flags,
				   path->cfg.rx_flags,
				   path->cfg.e2e,
				   tbv_debugfs_wire_path_flags(path),
				   path->cfg.tx_ring_size,
				   path->cfg.rx_ring_size,
				   path->cfg.sof_mask,
				   path->cfg.eof_mask);
			seq_printf(s,
				   "    data_rx_completed=%lld data_rx_canceled=%lld data_rx_credit_sent=%lld data_rx_credit_send_error=%lld data_rx_repost_failed=%lld rx_credit_pending=%u\n",
				   atomic64_read(&path->data_rx_completed),
				   atomic64_read(&path->data_rx_canceled),
				   atomic64_read(&path->data_rx_credit_sent),
				   atomic64_read(&path->data_rx_credit_send_error),
				   atomic64_read(&path->data_rx_repost_failed),
				   rx_data_credit_pending);
			seq_printf(s,
				   "    tx_poll enabled=%u calls=%lld completed=%lld\n",
				   path->tx_poll_enabled,
				   atomic64_read(&path->tx_poll_calls),
				   atomic64_read(&path->tx_poll_completed));
			seq_printf(s,
				   "    rx_supp_poll enabled=%u calls=%lld completed=%lld\n",
				   path->rx_supp_poll_enabled,
				   atomic64_read(&path->rx_supp_poll_calls),
				   atomic64_read(&path->rx_supp_poll_completed));
		}
	}
	mutex_unlock(&state->lock);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(tbv_debugfs_peers);

static int tbv_debugfs_peer_identity_show(struct seq_file *s, void *unused)
{
	struct tbv_state *state = s->private;
	struct tbv_peer *peer;

	seq_puts(s,
		 "# one rail per line; key_link/key_depth are xd->link/xd->depth as stored in tbv_rail_key\n");
	seq_printf(s,
		   "# native_single_peer=%u native_source_aware=%u native_legacy_ambiguous_limited=%lld\n",
		   state->native_single_peer,
		   state->native_control_source_aware,
		   atomic64_read(&state->native_legacy_ambiguous_limited));

	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		const struct tb_xdomain *xd = peer->xd;
		struct tbv_rail *rail;

		seq_printf(s,
			   "peer peer=%u backend=%s refs=%u rails=%u native_qp_rr_rail_id=%u xd_dev=%s xd_route=0x%llx xd_link=%u xd_depth=%u xd_link_speed=%uGb/s xd_link_width=0x%x xd_usb4=%u",
			   peer->peer_id, tbv_backend_name(peer->backend),
			   refcount_read(&peer->refcnt), peer->nr_rails,
			   peer->native_qp_rr_rail_id,
			   xd ? dev_name(&xd->dev) : "<none>",
			   xd ? xd->route : 0, xd ? xd->link : 0,
			   xd ? xd->depth : 0, xd ? xd->link_speed : 0,
			   xd ? xd->link_width : 0, xd ? xd->link_usb4 : 0);
		if (xd && xd->remote_uuid)
			seq_printf(s, " remote_uuid=%pUb\n", xd->remote_uuid);
		else
			seq_puts(s, " remote_uuid=<none>\n");

		list_for_each_entry(rail, &peer->rails, node) {
			struct tbv_path *path = &rail->path;
			struct tb_ring *tx_ring = READ_ONCE(path->tx_ring);
			struct tb_ring *rx_ring = READ_ONCE(path->rx_ring);
			bool data_ready = peer->backend == TBV_BACKEND_APPLE ?
				tbv_rail_apple_data_ready(rail) :
				tbv_rail_data_ready(rail);

			seq_printf(s,
				   "rail peer=%u rail=0x%x key_hash=0x%08x native_lane=%u active=%u removing=%u data_ready=%u state=%s qp_binds=%d key_route=0x%llx key_link=%u key_depth=%u key_path_id=%u link_speed=%uGb/s link_width=0x%x cfg_tx_hop=%d cfg_rx_hop=%d cfg_tx_path=%d cfg_rx_path=%d cfg_tx_flags=0x%x cfg_rx_flags=0x%x cfg_e2e=%u wire_flags=0x%x tx_ring_hop=%d rx_ring_hop=%d tx_ring_flags=0x%x rx_ring_flags=0x%x rx_ring_e2e_tx_hop=%d local_out=%d local_tx=%d local_rx=%d path_remote_out=%d remote_rail=0x%x remote_out=%d remote_tx=%d remote_rx=%d attempts_native=%u attempts_ready=%u attempts_tunnel=%u last_error=%d\n",
				   peer->peer_id, rail->rail_id,
				   tbv_rail_key_hash(&rail->key),
				   rail->native_lane, rail->active,
				   rail->removing, data_ready,
				   tbv_path_state_name(path->state),
				   atomic_read(&rail->native_qp_bind_count),
				   rail->key.route, rail->key.local_adapter,
				   rail->key.remote_adapter, rail->key.path_id,
				   rail->link_speed, rail->link_width,
				   path->cfg.tx_hop, path->cfg.rx_hop,
				   path->cfg.transmit_path,
				   path->cfg.receive_path, path->cfg.tx_flags,
				   path->cfg.rx_flags, path->cfg.e2e,
				   tbv_debugfs_wire_path_flags(path),
				   tbv_debugfs_ring_hop(tx_ring),
				   tbv_debugfs_ring_hop(rx_ring),
				   tbv_debugfs_ring_flags(tx_ring),
				   tbv_debugfs_ring_flags(rx_ring),
				   tbv_debugfs_ring_e2e_tx_hop(rx_ring),
				   path->local_transmit_path,
				   path->local_tx_hop, path->local_rx_hop,
				   path->remote_transmit_path,
				   rail->remote_rail_id,
				   rail->remote_transmit_path,
				   rail->remote_tx_hop, rail->remote_rx_hop,
				   rail->native_attempts,
				   rail->native_ready_attempts,
				   rail->native_tunnel_attempts,
				   rail->native_last_error);
		}
	}
	mutex_unlock(&state->lock);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(tbv_debugfs_peer_identity);

static int tbv_debugfs_configured_links_show(struct seq_file *s, void *unused)
{
	struct tbv_state *state = s->private;

	tbv_link_debugfs_show(s, state);
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(tbv_debugfs_configured_links);

int tbv_debugfs_init(struct tbv_state *state)
{
	state->debugfs_dir = debugfs_create_dir(TBV_DRV_NAME, NULL);
	if (IS_ERR(state->debugfs_dir)) {
		state->debugfs_dir = NULL;
		return 0;
	}

	debugfs_create_file("summary", 0444, state->debugfs_dir, state,
			    &tbv_debugfs_summary_fops);
	debugfs_create_file("peers", 0444, state->debugfs_dir, state,
			    &tbv_debugfs_peers_fops);
	debugfs_create_file("peer_identity", 0444, state->debugfs_dir, state,
			    &tbv_debugfs_peer_identity_fops);
	debugfs_create_file("configured_links", 0444, state->debugfs_dir,
			    state, &tbv_debugfs_configured_links_fops);
	return 0;
}

void tbv_debugfs_exit(struct tbv_state *state)
{
	debugfs_remove_recursive(state->debugfs_dir);
	state->debugfs_dir = NULL;
}
