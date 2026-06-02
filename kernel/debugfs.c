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
		   state->native_control_source_aware ? "enabled" :
						       "limited");
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
	seq_printf(s, "data_wr_timeout: %lld\n",
		   atomic64_read(&state->data_wr_timeout));
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
	seq_printf(s, "data_rx_credit_sent: %lld\n",
		   atomic64_read(&state->data_rx_credit_sent));
	seq_printf(s, "data_rx_credit_send_error: %lld\n",
		   atomic64_read(&state->data_rx_credit_send_error));
	seq_printf(s, "data_rx_repost_failed: %lld\n",
		   atomic64_read(&state->data_rx_repost_failed));
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
	seq_printf(s, "data_rx_copy_error: %lld\n",
		   atomic64_read(&state->data_rx_copy_error));
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

		seq_printf(s, "peer %u backend=%s rails=%u\n",
			   peer->peer_id, tbv_backend_name(peer->backend),
			   peer->nr_rails);

		list_for_each_entry(rail, &peer->rails, node) {
			bool data_ready;

			data_ready = peer->backend == TBV_BACKEND_APPLE ?
				tbv_rail_apple_data_ready(rail) :
				tbv_rail_data_ready(rail);

			seq_printf(s,
				   "  rail=0x%x route=0x%llx local=%u remote=%u path=%u link_speed=%uGb/s link_width=0x%x active=%u data_ready=%u state=%s negotiated=%u ready_sent=%u remote_ready=%u attempts=%u last_error=%d local_out=%d local_tx=%d local_rx=%d remote_rail=0x%x remote_out=%d remote_tx=%d remote_rx=%d\n",
				   rail->rail_id, rail->key.route,
				   rail->key.local_adapter,
				   rail->key.remote_adapter,
				   rail->key.path_id,
				   rail->link_speed,
				   rail->link_width,
				   rail->active,
				   data_ready,
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
				   atomic64_read(&rail->path.data_tx_enqueued),
				   atomic64_read(&rail->path.data_tx_posted),
				   atomic64_read(&rail->path.data_tx_completed),
				   atomic64_read(&rail->path.data_tx_credit_stalls),
				   atomic64_read(&rail->path.data_tx_credit_received),
				   rail->path.tx_remote_data_credits,
				   rail->path.tx_remote_data_credit_max);
			seq_printf(s,
				   "    path_cfg tx_flags=0x%x rx_flags=0x%x e2e=%u wire_flags=0x%x tx_ring=%u rx_ring=%u sof_mask=0x%x eof_mask=0x%x\n",
				   rail->path.cfg.tx_flags,
				   rail->path.cfg.rx_flags,
				   rail->path.cfg.e2e,
				   tbv_debugfs_wire_path_flags(&rail->path),
				   rail->path.cfg.tx_ring_size,
				   rail->path.cfg.rx_ring_size,
				   rail->path.cfg.sof_mask,
				   rail->path.cfg.eof_mask);
			seq_printf(s,
				   "    data_rx_completed=%lld data_rx_credit_sent=%lld data_rx_credit_send_error=%lld data_rx_repost_failed=%lld rx_credit_pending=%u\n",
				   atomic64_read(&rail->path.data_rx_completed),
				   atomic64_read(&rail->path.data_rx_credit_sent),
				   atomic64_read(&rail->path.data_rx_credit_send_error),
				   atomic64_read(&rail->path.data_rx_repost_failed),
				   rail->path.rx_data_credit_pending);
		}
	}
	mutex_unlock(&state->lock);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(tbv_debugfs_peers);

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
	debugfs_create_file("configured_links", 0444, state->debugfs_dir,
			    state, &tbv_debugfs_configured_links_fops);
	return 0;
}

void tbv_debugfs_exit(struct tbv_state *state)
{
	debugfs_remove_recursive(state->debugfs_dir);
	state->debugfs_dir = NULL;
}
