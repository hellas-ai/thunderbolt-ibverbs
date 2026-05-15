// SPDX-License-Identifier: GPL-2.0

#include <linux/debugfs.h>
#include <linux/seq_file.h>

#include "tbv.h"

static int tbv_debugfs_summary_show(struct seq_file *s, void *unused)
{
	struct tbv_state *state = s->private;

	seq_printf(s, "profile: %s\n", tbv_profile_name(state->cfg.profile));
	seq_printf(s, "native_enabled: %u\n", state->cfg.native_enabled);
	seq_printf(s, "apple_enabled: %u\n", state->cfg.apple_enabled);
	seq_printf(s, "rc_supported: %u\n", state->cfg.rc_supported);
	seq_printf(s, "uc_supported: %u\n", state->cfg.uc_supported);
	seq_printf(s, "tbnet_identity: %s\n",
		   tbv_tbnet_identity_name(state->cfg.tbnet_identity));
	seq_printf(s, "services_registered: %u\n",
		   state->services_registered);
	seq_printf(s, "allocate_rings: %u\n", state->allocate_rings);
	seq_printf(s, "start_rings: %u\n", state->start_rings);
	seq_printf(s, "negotiate_native: %u\n", state->negotiate_native);
	seq_printf(s, "enable_tunnels: %u\n", state->enable_tunnels);
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
			seq_printf(s,
				   "  rail=0x%x route=0x%llx local=%u remote=%u path=%u link_speed=%uGb/s link_width=0x%x active=%u state=%s negotiated=%u attempts=%u last_error=%d remote_rail=0x%x remote_out=%d remote_tx=%d remote_rx=%d\n",
				   rail->rail_id, rail->key.route,
				   rail->key.local_adapter,
				   rail->key.remote_adapter,
				   rail->key.path_id,
				   rail->link_speed,
				   rail->link_width,
				   rail->active,
				   tbv_path_state_name(rail->path.state),
				   rail->native_negotiated,
				   rail->native_attempts,
				   rail->native_last_error,
				   rail->remote_rail_id,
				   rail->remote_transmit_path,
				   rail->remote_tx_hop,
				   rail->remote_rx_hop);
		}
	}
	mutex_unlock(&state->lock);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(tbv_debugfs_peers);

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
	return 0;
}

void tbv_debugfs_exit(struct tbv_state *state)
{
	debugfs_remove_recursive(state->debugfs_dir);
	state->debugfs_dir = NULL;
}
