// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/thunderbolt.h>

#include "tbv.h"

static bool tbv_peer_matches(const struct tbv_peer *peer,
			     enum tbv_backend_type backend,
			     const struct tb_xdomain *xd)
{
	return peer->backend == backend && peer->xd == xd;
}

static bool tbv_xdomain_same_remote_host(const struct tb_xdomain *a,
					 const struct tb_xdomain *b)
{
	if (a == b)
		return true;
	if (!a || !b)
		return false;

	if (a->remote_uuid && b->remote_uuid)
		return uuid_equal(a->remote_uuid, b->remote_uuid);

	return a->route == b->route && a->link == b->link &&
	       a->depth == b->depth;
}

static bool tbv_native_legacy_xdomain_allowed_locked(struct tbv_state *state,
						     struct tb_xdomain *xd,
						     u32 *existing_peer_id)
{
	struct tbv_peer *pos;

	list_for_each_entry(pos, &state->peers, node) {
		if (pos->backend != TBV_BACKEND_NATIVE)
			continue;
		if (pos->xd == xd)
			return true;
		if (!tbv_xdomain_same_remote_host(pos->xd, xd))
			continue;

		if (existing_peer_id)
			*existing_peer_id = pos->peer_id;
		return false;
	}

	return true;
}

static bool tbv_native_legacy_rail_key_allowed_locked(struct tbv_peer *peer,
						      const struct tbv_rail_key *key,
						      u32 *existing_peer_id)
{
	struct tbv_state *state = peer->state;
	struct tbv_peer *pos_peer;

	list_for_each_entry(pos_peer, &state->peers, node) {
		struct tbv_rail *pos_rail;

		if (pos_peer->backend != TBV_BACKEND_NATIVE)
			continue;

		list_for_each_entry(pos_rail, &pos_peer->rails, node) {
			if (pos_peer == peer)
				continue;
			if (tbv_rail_key_cmp(key, &pos_rail->key))
				continue;

			if (existing_peer_id)
				*existing_peer_id = pos_peer->peer_id;
			return false;
		}
	}

	return true;
}

struct tbv_peer *tbv_peer_get_or_create(struct tbv_state *state,
					enum tbv_backend_type backend,
					struct tb_xdomain *xd)
{
	struct tbv_peer *peer;
	struct tbv_peer *pos;
	u32 existing_peer_id = 0;

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
	mutex_init(&peer->control_lock);

	mutex_lock(&state->lock);
	list_for_each_entry(pos, &state->peers, node) {
		if (!tbv_peer_matches(pos, backend, xd))
			continue;

		refcount_inc(&pos->refcnt);
		mutex_unlock(&state->lock);
		tb_xdomain_put(peer->xd);
		kfree(peer);
		pr_info("peer %u reused backend=%s refs=%u\n", pos->peer_id,
			tbv_backend_name(backend), refcount_read(&pos->refcnt));
		return pos;
	}

	if (backend == TBV_BACKEND_NATIVE &&
	    !state->native_control_source_aware &&
	    !tbv_native_legacy_xdomain_allowed_locked(state, xd,
						      &existing_peer_id)) {
		atomic64_inc(&state->native_legacy_ambiguous_limited);
		if (!state->native_legacy_multicable_warned) {
			state->native_legacy_multicable_warned = true;
			pr_warn("legacy source-blind native control: limiting remote host to peer %u; apply callback_xd kernel support for multi-cable native rails\n",
				existing_peer_id);
		}
		mutex_unlock(&state->lock);
		tb_xdomain_put(peer->xd);
		kfree(peer);
		return ERR_PTR(-EBUSY);
	}

	ida_init(&peer->rail_ids);
	peer->peer_id = state->next_peer_id++;
	list_add_tail(&peer->node, &state->peers);
	mutex_unlock(&state->lock);

	pr_info("peer %u created backend=%s\n", peer->peer_id,
		tbv_backend_name(backend));
	return peer;
}

void tbv_peer_put(struct tbv_state *state, struct tbv_peer *peer)
{
	bool free_peer;

	if (!peer)
		return;

	mutex_lock(&state->lock);
	free_peer = refcount_dec_and_test(&peer->refcnt);
	if (free_peer)
		list_del_init(&peer->node);
	mutex_unlock(&state->lock);

	if (!free_peer)
		return;

	if (peer->nr_rails)
		pr_warn("peer %u destroyed with %u live rails\n",
			peer->peer_id, peer->nr_rails);

	pr_info("peer %u destroyed backend=%s\n", peer->peer_id,
		tbv_backend_name(peer->backend));
	ida_destroy(&peer->rail_ids);
	tb_xdomain_put(peer->xd);
	kfree(peer);
}

struct tbv_rail *tbv_peer_add_rail(struct tbv_peer *peer,
				   const struct tbv_rail_key *key,
				   u32 native_lane)
{
	struct tbv_path_config path_cfg;
	struct tbv_rail *rail;
	struct tbv_rail *pos;
	u32 existing_peer_id = 0;
	int rail_id;

	rail = kzalloc(sizeof(*rail), GFP_KERNEL);
	if (!rail)
		return ERR_PTR(-ENOMEM);

	rail_id = ida_alloc(&peer->rail_ids, GFP_KERNEL);
	if (rail_id < 0) {
		kfree(rail);
		return ERR_PTR(rail_id);
	}

	rail->key = *key;
	rail->rail_id = rail_id;
	rail->native_lane = native_lane;
	atomic_set(&rail->native_qp_bind_count, 0);
	refcount_set(&rail->refcnt, 1);
	init_completion(&rail->refs_zero);
	rail->active = true;
	rail->remote_transmit_path = -1;
	rail->remote_tx_hop = -1;
	rail->remote_rx_hop = -1;
	rail->native_last_error = 0;
	rail->native_tunnel_attempts = 0;
	rail->native_ready_sent = false;
	rail->native_remote_ready = false;
	tbv_native_control_init_rail(rail, peer);
	tbv_path_default_config(peer->backend, &path_cfg);
	if (peer->backend == TBV_BACKEND_NATIVE) {
		/*
		 * Native rails only bind to Linux peers.  Even in mixed mode the
		 * Mac-facing wire format is handled by the separate Apple
		 * backend, so native can keep the hardware E2E delivery contract
		 * needed for RC semantics.
		 */
		path_cfg.tx_flags |= RING_FLAG_E2E;
		path_cfg.rx_flags |= RING_FLAG_E2E;
		path_cfg.e2e = true;
	}
	tbv_path_init(&rail->path, &path_cfg, rail);

	mutex_lock(&peer->state->lock);
	if (peer->backend == TBV_BACKEND_NATIVE &&
	    !peer->state->native_control_source_aware &&
	    !tbv_native_legacy_rail_key_allowed_locked(peer, key,
						       &existing_peer_id)) {
		atomic64_inc(&peer->state->native_legacy_ambiguous_limited);
		if (!peer->state->native_legacy_multicable_warned) {
			peer->state->native_legacy_multicable_warned = true;
			pr_warn("legacy source-blind native control: rejecting duplicate native rail key from peer %u; apply callback_xd kernel support for multi-cable native rails\n",
				existing_peer_id);
		}
		mutex_unlock(&peer->state->lock);
		ida_free(&peer->rail_ids, rail->rail_id);
		kfree(rail);
		return ERR_PTR(-EBUSY);
	}

	list_for_each_entry(pos, &peer->rails, node) {
		int cmp = tbv_rail_key_cmp(key, &pos->key);

		if (!cmp) {
			mutex_unlock(&peer->state->lock);
			ida_free(&peer->rail_ids, rail->rail_id);
			kfree(rail);
			return ERR_PTR(-EEXIST);
		}
		if (cmp < 0) {
			list_add_tail(&rail->node, &pos->node);
			peer->nr_rails++;
			mutex_unlock(&peer->state->lock);
			return rail;
		}
	}

	list_add_tail(&rail->node, &peer->rails);
	peer->nr_rails++;
	mutex_unlock(&peer->state->lock);
	return rail;
}

void tbv_peer_remove_rail(struct tbv_rail *rail)
{
	struct tbv_peer *peer;

	if (!rail)
		return;

	peer = rail->peer;

	/*
	 * Mark the rail as removing but leave it on peer->rails. Selectors
	 * (tbv_select_native_data_path_for_qp_locked et al.) all honor
	 * removing=true so no new TX targets pick this rail. Keeping the
	 * rail visible while ib_unregister_device() drains in-flight verbs
	 * callbacks means debugfs, native control, and any other observer
	 * sees a consistent topology snapshot until teardown actually
	 * completes -- there is never a window where a rail "exists but
	 * isn't reachable through peer->rails".
	 */
	mutex_lock(&peer->state->lock);
	rail->removing = true;
	mutex_unlock(&peer->state->lock);

	/*
	 * Tear down the per-rail ib_device (if any) before the path. Any QPs
	 * pinned to this rail hold a rail refcount, so ib_unregister_device's
	 * destroy_qp callbacks must complete before wait_for_completion
	 * (refs_zero) can return. This serializes data-path cleanup with
	 * verbs lifecycle and removes any chance of post_send racing
	 * tbv_path_destroy.
	 */
	tbv_ibdev_rail_event(peer->state, rail, false);

	tbv_native_control_cancel_rail(rail);
	tbv_rail_put(rail);
	wait_for_completion(&rail->refs_zero);

	/*
	 * All QPs that held a ref on this rail have now been destroyed
	 * (their refs were dropped in tbv_destroy_qp); it is safe to
	 * unlink the rail from peer->rails and free its path.
	 */
	mutex_lock(&peer->state->lock);
	if (!list_empty(&rail->node)) {
		list_del_init(&rail->node);
		if (peer->nr_rails)
			peer->nr_rails--;
	}
	mutex_unlock(&peer->state->lock);

	tbv_path_destroy(&rail->path, peer->xd);
	ida_free(&peer->rail_ids, rail->rail_id);
	kfree(rail);
}

void tbv_rail_put(struct tbv_rail *rail)
{
	if (refcount_dec_and_test(&rail->refcnt))
		complete(&rail->refs_zero);
}
