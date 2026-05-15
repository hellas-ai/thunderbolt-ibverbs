// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/thunderbolt.h>

#include "tbv.h"

static void tbv_peer_free_rails(struct tbv_peer *peer)
{
	struct tbv_rail *rail;
	struct tbv_rail *tmp;

	list_for_each_entry_safe(rail, tmp, &peer->rails, node) {
		tbv_native_control_cancel_rail(rail);
		tbv_path_destroy(&rail->path, peer->xd);
		list_del(&rail->node);
		kfree(rail);
	}
	peer->nr_rails = 0;
}

struct tbv_peer *tbv_peer_create(struct tbv_state *state,
				 enum tbv_backend_type backend,
				 struct tb_xdomain *xd)
{
	struct tbv_peer *peer;

	if (!tbv_backend_get(backend))
		return ERR_PTR(-EINVAL);

	peer = kzalloc(sizeof(*peer), GFP_KERNEL);
	if (!peer)
		return ERR_PTR(-ENOMEM);

	refcount_set(&peer->refcnt, 1);
	peer->state = state;
	peer->backend = backend;
	peer->xd = tb_xdomain_get(xd);
	INIT_LIST_HEAD(&peer->rails);

	mutex_lock(&state->lock);
	peer->peer_id = state->next_peer_id++;
	list_add_tail(&peer->node, &state->peers);
	mutex_unlock(&state->lock);

	pr_info("peer %u created backend=%s\n", peer->peer_id,
		tbv_backend_name(backend));
	return peer;
}

void tbv_peer_destroy(struct tbv_state *state, struct tbv_peer *peer)
{
	if (!peer)
		return;

	mutex_lock(&state->lock);
	list_del_init(&peer->node);
	mutex_unlock(&state->lock);

	tbv_peer_free_rails(peer);
	pr_info("peer %u destroyed\n", peer->peer_id);
	tb_xdomain_put(peer->xd);
	kfree(peer);
}

int tbv_peer_add_rail(struct tbv_peer *peer, const struct tbv_rail_key *key)
{
	struct tbv_path_config path_cfg;
	struct tbv_rail *rail;
	struct tbv_rail *pos;

	rail = kzalloc(sizeof(*rail), GFP_KERNEL);
	if (!rail)
		return -ENOMEM;

	rail->key = *key;
	rail->rail_id = tbv_rail_key_hash(key);
	rail->active = true;
	rail->remote_transmit_path = -1;
	rail->remote_tx_hop = -1;
	rail->remote_rx_hop = -1;
	rail->native_last_error = 0;
	tbv_native_control_init_rail(rail, peer);
	tbv_path_default_config(peer->backend, &path_cfg);
	if (peer->backend == TBV_BACKEND_NATIVE &&
	    peer->state->cfg.profile == TBV_PROFILE_LINUX_PERF) {
		path_cfg.rx_flags |= RING_FLAG_E2E;
		path_cfg.e2e = true;
	}
	tbv_path_init(&rail->path, &path_cfg, rail);

	list_for_each_entry(pos, &peer->rails, node) {
		int cmp = tbv_rail_key_cmp(key, &pos->key);

		if (!cmp) {
			kfree(rail);
			return -EEXIST;
		}
		if (cmp < 0) {
			list_add_tail(&rail->node, &pos->node);
			peer->nr_rails++;
			return 0;
		}
	}

	list_add_tail(&rail->node, &peer->rails);
	peer->nr_rails++;
	return 0;
}
