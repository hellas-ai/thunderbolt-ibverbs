// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/thunderbolt.h>
#include <linux/uuid.h>

#include "tbv.h"

const uuid_t tbv_native_service_uuid =
	UUID_INIT(0x7c2c8f1e, 0x5b4d, 0x4a01, 0x9f, 0x3a,
		  0x2b, 0x8e, 0x6d, 0x4c, 0x1a, 0x07);

static const uuid_t tbv_apple_service_uuid =
	UUID_INIT(0x49bf223e, 0xd4aa, 0x44d7, 0x87, 0x91,
		  0x50, 0x44, 0x5a, 0xc5, 0x2d, 0x5e);

static const char tbv_apple_ca_key[TB_PROPERTY_KEY_SIZE + 1] = {
	(char)0xff, (char)0xff, (char)0xff, (char)0xff,
	(char)0xff, (char)0xff, 'C', 'A', '\0',
};

/*
 * Apple's RDMA service key is not printable. macOS exposes Linux services
 * advertised under "rdma" in IORegistry, but AppleThunderboltRDMA does not
 * bind them as RDMA peers. Use the same AD key Apple advertises.
 */
static const char tbv_apple_ad_key[TB_PROPERTY_KEY_SIZE + 1] = {
	(char)0xff, (char)0xff, (char)0xff, (char)0xff,
	(char)0xff, (char)0xff, 'A', 'D', '\0',
};

static struct tbv_state *tbv_service_state;

static const char * const tbv_native_protocol_keys[TBV_NATIVE_MAX_LANES] = {
	TBV_NATIVE_PROTOCOL_KEY,
	"tbverb1",
	"tbverb2",
	"tbverb3",
};

#define TBV_APPLE_RAIL_RETRY_MS 250

struct tbv_service_binding {
	struct tbv_peer *peer;
	struct tbv_rail *rail;
};

static const char *tbv_native_protocol_key(u32 lane)
{
	if (lane >= ARRAY_SIZE(tbv_native_protocol_keys))
		return NULL;

	return tbv_native_protocol_keys[lane];
}

static struct tb_property_dir *
tbv_service_create_dir_common(const uuid_t *uuid, u32 prtcid, u32 prtcvers,
			      u32 prtcrevs, u32 prtcstns)
{
	struct tb_property_dir *dir;
	int ret;

	dir = tb_property_create_dir(uuid);
	if (!dir)
		return ERR_PTR(-ENOMEM);

	ret = tb_property_add_immediate(dir, "prtcid", prtcid);
	ret = ret ?: tb_property_add_immediate(dir, "prtcvers", prtcvers);
	ret = ret ?: tb_property_add_immediate(dir, "prtcrevs", prtcrevs);
	ret = ret ?: tb_property_add_immediate(dir, "prtcstns", prtcstns);
	if (ret) {
		tb_property_free_dir(dir);
		return ERR_PTR(ret);
	}

	return dir;
}

struct tb_property_dir *tbv_service_create_native_dir(void)
{
	return tbv_service_create_dir_common(&tbv_native_service_uuid,
					     TBV_NATIVE_PRTCID,
					     TBV_NATIVE_PRTCVERS,
					     TBV_NATIVE_PRTCREVS, 0);
}

struct tb_property_dir *tbv_service_create_apple_dir(u32 prtcstns)
{
	struct tb_property_dir *dir;
	int ret;

	dir = tbv_service_create_dir_common(&tbv_apple_service_uuid,
					    TBV_APPLE_PRTCID,
					    TBV_APPLE_PRTCVERS,
					    TBV_APPLE_PRTCREVS,
					    prtcstns);
	if (IS_ERR(dir))
		return dir;

	ret = tb_property_add_immediate(dir, tbv_apple_ca_key, 1);
	if (ret) {
		tb_property_free_dir(dir);
		return ERR_PTR(ret);
	}

	return dir;
}

static enum tbv_backend_type
tbv_service_backend_from_id(const struct tb_service_id *id)
{
	if (id->protocol_id == TBV_APPLE_PRTCID)
		return TBV_BACKEND_APPLE;
	return TBV_BACKEND_NATIVE;
}

static bool tbv_service_backend_enabled(const struct tbv_state *state,
					enum tbv_backend_type backend)
{
	if (backend == TBV_BACKEND_APPLE)
		return state->cfg.apple_enabled && state->apple_data;
	return state->cfg.native_enabled && state->native_data;
}

static bool tbv_service_backend_data_enabled(const struct tbv_state *state,
					     enum tbv_backend_type backend)
{
	if (backend == TBV_BACKEND_APPLE)
		return state->apple_data;
	return state->native_data;
}

static bool tbv_service_tbnet_path_ready(struct tbv_state *state,
					 const struct tb_xdomain *xd)
{
	if (state->cfg.tbnet_identity != TBV_TBNET_ID_MINIMAL_PACKET)
		return true;

	return tbv_tbnet_minimal_path_ready(&state->tbnet_identity,
					    xd ? xd->remote_uuid : NULL);
}

static bool tbv_service_should_defer_apple_rail(struct tbv_state *state,
						const struct tb_xdomain *xd)
{
	return state->apple_rails_wait_tbnet &&
	       !tbv_service_tbnet_path_ready(state, xd);
}

static bool tbv_service_has_pending_apple_rail_locked(struct tbv_state *state)
{
	struct tbv_peer *peer;

	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		if (peer->backend != TBV_BACKEND_APPLE)
			continue;

		list_for_each_entry(rail, &peer->rails, node)
			if (!rail->removing &&
			    !READ_ONCE(rail->ibdev) &&
			    !READ_ONCE(rail->ibdev_register_failed) &&
			    !READ_ONCE(rail->ibdev_register_deferred) &&
			    tbv_rail_apple_service_ready(rail))
				return true;
	}

	return false;
}

static void tbv_service_refresh_apple_rails_pending(struct tbv_state *state)
{
	mutex_lock(&state->lock);
	WRITE_ONCE(state->apple_rails_pending,
		   tbv_service_has_pending_apple_rail_locked(state));
	mutex_unlock(&state->lock);
}

static void tbv_service_queue_apple_rail_work(struct tbv_state *state,
					      unsigned long delay)
{
	if (!state->enable_tunnels)
		return;

	queue_delayed_work(system_long_wq, &state->apple_rail_work, delay);
}

static int tbv_service_enable_apple_rail_tunnel(struct tbv_rail *rail)
{
	struct tbv_peer *peer = rail->peer;
	struct tbv_state *state = peer ? peer->state : NULL;
	bool enable = false;
	int ret = 0;

	if (!peer || !state || !peer->xd)
		return -ENODEV;

	mutex_lock(&peer->control_lock);
	mutex_lock(&state->lock);
	if (rail->removing) {
		ret = -ENODEV;
	} else if (rail->path.state == TBV_PATH_TUNNEL_ENABLED) {
		ret = 0;
	} else if (rail->path.state == TBV_PATH_RING_STARTED) {
		enable = true;
	} else {
		ret = -ENOTCONN;
	}
	mutex_unlock(&state->lock);

	if (enable) {
		ret = tbv_path_enable_tunnel(&rail->path, peer->xd,
					     rail->path.cfg.receive_path);
		if (!ret)
			pr_info("enabled Apple data path service route=0x%llx tx_path=%d rx_path=%d tx_hop=%d rx_hop=%d\n",
				peer->xd->route, rail->path.local_transmit_path,
				rail->path.remote_transmit_path,
				rail->path.local_tx_hop, rail->path.local_rx_hop);
	}
	mutex_unlock(&peer->control_lock);

	return ret;
}

static int tbv_service_publish_apple_rail(struct tbv_rail *rail)
{
	struct tbv_peer *peer = rail->peer;
	int ret;

	ret = tbv_service_enable_apple_rail_tunnel(rail);
	if (ret)
		return ret;

	pr_info("publishing Apple data service route=0x%llx tx_path=%d rx_path=%d tx_hop=%d rx_hop=%d state=%s\n",
		peer->xd->route, rail->path.local_transmit_path,
		rail->path.remote_transmit_path, rail->path.local_tx_hop,
		rail->path.local_rx_hop, tbv_path_state_name(rail->path.state));
	return tbv_ibdev_rail_event(peer->state, rail, true);
}

static struct tbv_rail *
tbv_service_next_pending_apple_rail(struct tbv_state *state)
{
	struct tbv_peer *peer;
	bool pending = false;

	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		if (peer->backend != TBV_BACKEND_APPLE)
			continue;

		list_for_each_entry(rail, &peer->rails, node) {
			if (rail->removing ||
			    READ_ONCE(rail->ibdev) ||
			    READ_ONCE(rail->ibdev_register_failed) ||
			    READ_ONCE(rail->ibdev_register_deferred) ||
			    !tbv_rail_apple_service_ready(rail))
				continue;
			pending = true;
			if (!tbv_service_tbnet_path_ready(state, peer->xd))
				continue;
			refcount_inc(&rail->refcnt);
			mutex_unlock(&state->lock);
			return rail;
		}
	}
	WRITE_ONCE(state->apple_rails_pending, pending);
	mutex_unlock(&state->lock);
	return NULL;
}

static void tbv_service_apple_rail_work(struct work_struct *work)
{
	struct tbv_state *state =
		container_of(to_delayed_work(work), struct tbv_state,
			     apple_rail_work);

	if (!state->services_registered || !state->enable_tunnels)
		return;
	if (!tbv_service_tbnet_path_ready(state, NULL))
		return;

	for (;;) {
		struct tbv_rail *rail;
		int ret;

		rail = tbv_service_next_pending_apple_rail(state);
		if (!rail)
			return;

		ret = tbv_service_publish_apple_rail(rail);
		if (ret) {
			struct tbv_peer *peer = rail->peer;

			tbv_service_refresh_apple_rails_pending(state);
			pr_warn_ratelimited("failed to publish Apple data service peer=%u route=0x%llx rail=0x%x state=%s ret=%d; retrying\n",
					    peer->peer_id, peer->xd->route,
					    rail->rail_id,
					    tbv_path_state_name(rail->path.state),
					    ret);
			tbv_rail_put(rail);
			if (READ_ONCE(state->apple_rails_pending))
				tbv_service_queue_apple_rail_work(
					state,
					msecs_to_jiffies(TBV_APPLE_RAIL_RETRY_MS));
			return;
		}

		tbv_rail_put(rail);
	}
}

void tbv_services_tbnet_identity_ready(struct tbv_tbnet_identity *identity)
{
	struct tbv_state *state = tbv_service_state;

	if (!state || identity != &state->tbnet_identity)
		return;
	if (!state->services_registered || !state->apple_rails_wait_tbnet)
		return;
	if (!tbv_service_tbnet_path_ready(state, NULL))
		return;

	tbv_service_queue_apple_rail_work(state, 0);
}

static bool tbv_service_apple_xdomain_allowed(const struct tb_xdomain *xd)
{
	const char *vendor = xd && xd->vendor_name ? xd->vendor_name : NULL;

	/*
	 * FA57 is an Apple interop backend, not the Linux peer protocol. Linux
	 * peers advertise and negotiate the native services; binding their AD key
	 * here creates a second, weaker transport path to the same machine.
	 */
	if (vendor && !strcmp(vendor, "Apple Inc."))
		return true;

	if (!vendor) {
		pr_warn("allowing Apple AD/FA57 service route=0x%llx with unknown peer vendor\n",
			xd ? xd->route : 0);
		return true;
	}

	pr_info("skipping Apple AD/FA57 service from non-Apple peer vendor='%s'\n",
		vendor ?: "<unknown>");
	return false;
}

static u32 tbv_service_native_lane(const struct tb_service_id *id)
{
	return (u32)id->driver_data;
}

static u32 tbv_config_native_lane_count(const struct tbv_state *state)
{
	if (state->cfg.requested.lanes_auto)
		return min_t(u32, 2, TBV_NATIVE_MAX_LANES);

	return state->cfg.requested.lanes_max;
}

static u32 tbv_service_path_id(const struct tb_service *svc,
			       const struct tb_service_id *id,
			       enum tbv_backend_type backend)
{
	u32 lane;

	if (backend != TBV_BACKEND_NATIVE)
		return svc->prtcid;

	lane = tbv_service_native_lane(id);
	if (!lane)
		return svc->prtcid;

	return (svc->prtcid << 8) | lane;
}

static int tbv_service_probe(struct tb_service *svc,
			     const struct tb_service_id *id)
{
	enum tbv_backend_type backend = tbv_service_backend_from_id(id);
	struct tb_xdomain *xd = tb_service_parent(svc);
	struct tbv_service_binding *binding;
	struct tbv_rail_key key;
	struct tbv_peer *peer;
	struct tbv_rail *rail;
	u32 native_lane = tbv_service_native_lane(id);
	u32 path_id;
	int ret;

	if (!tbv_service_state)
		return -ENODEV;

	if (!tbv_service_backend_enabled(tbv_service_state, backend))
		return -ENODEV;

	if (backend == TBV_BACKEND_APPLE &&
	    !tbv_service_apple_xdomain_allowed(xd))
		return -ENODEV;

	if (backend == TBV_BACKEND_NATIVE &&
	    native_lane >= tbv_config_native_lane_count(tbv_service_state))
		return -ENODEV;

	binding = kzalloc(sizeof(*binding), GFP_KERNEL);
	if (!binding)
		return -ENOMEM;

	peer = tbv_peer_get_or_create(tbv_service_state, backend, xd);
	if (IS_ERR(peer)) {
		ret = PTR_ERR(peer);
		goto err_free_binding;
	}

	path_id = tbv_service_path_id(svc, id, backend);
	tbv_rail_key_init(&key, xd->route, xd->link, xd->depth, path_id);
	rail = tbv_peer_add_rail(peer, &key, native_lane);
	if (IS_ERR(rail)) {
		ret = PTR_ERR(rail);
		goto err_put_peer;
	}

	rail->link_speed = xd->link_speed;
	rail->link_width = xd->link_width;

	if (tbv_service_state->allocate_rings &&
	    tbv_service_backend_data_enabled(tbv_service_state, backend)) {
		ret = tbv_path_alloc_rings(&rail->path, xd, -1);
		if (ret)
			goto err_remove_rail;

		if (!rail->path.tx_ring || !rail->path.rx_ring) {
			ret = -EIO;
			goto err_remove_rail;
		}

		pr_info("allocated rings service id=%d native_lane=%u tx_hop=%d rx_hop=%d out_hop=%d\n",
			svc->id, backend == TBV_BACKEND_NATIVE ? native_lane : 0,
			rail->path.tx_ring->hop,
			rail->path.rx_ring->hop,
			rail->path.local_transmit_path);

		if (tbv_service_state->start_rings) {
			ret = tbv_path_start_rings(&rail->path);
			if (ret)
				goto err_remove_rail;

			pr_info("started rings service id=%d native_lane=%u tx_hop=%d rx_hop=%d\n",
				svc->id,
				backend == TBV_BACKEND_NATIVE ? native_lane : 0,
				rail->path.tx_ring->hop,
				rail->path.rx_ring->hop);

			if (backend == TBV_BACKEND_NATIVE &&
			    tbv_service_state->negotiate_native) {
				tbv_native_control_queue_rail(tbv_service_state,
							      rail);
			} else if (backend == TBV_BACKEND_APPLE &&
				   tbv_service_state->enable_tunnels) {
				/*
				 * Apple rail publication is worker-owned. The
				 * worker gates on TBnet readiness, enables the
				 * FA57 data tunnel, publishes verbs, and retries
				 * transient tunnel setup failures.
				 */
				WRITE_ONCE(tbv_service_state->apple_rails_pending,
					   true);
				if (tbv_service_should_defer_apple_rail(tbv_service_state, xd)) {
					pr_info("deferring Apple data service id=%d route=0x%llx until ThunderboltIP packet path is active\n",
						svc->id, xd->route);
				} else {
					pr_info("queueing Apple data service id=%d route=0x%llx for publication\n",
						svc->id, xd->route);
					tbv_service_queue_apple_rail_work(tbv_service_state, 0);
				}
			}
		}
	}

	binding->peer = peer;
	binding->rail = rail;
	tb_service_set_drvdata(svc, binding);
	pr_info("bound %s service id=%d key=%s native_lane=%u route=0x%llx link_speed=%uGb/s width=0x%x rail=0x%x path_id=%u\n",
		tbv_backend_name(backend), svc->id, svc->key,
		backend == TBV_BACKEND_NATIVE ? native_lane : 0,
		xd->route, xd->link_speed, xd->link_width,
		tbv_rail_key_hash(&key), path_id);
	return 0;

err_remove_rail:
	tbv_peer_remove_rail(rail);
	if (backend == TBV_BACKEND_APPLE)
		tbv_service_refresh_apple_rails_pending(tbv_service_state);
err_put_peer:
	tbv_peer_put(tbv_service_state, peer);
err_free_binding:
	kfree(binding);
	return ret;
}

static void tbv_service_remove(struct tb_service *svc)
{
	struct tbv_service_binding *binding = tb_service_get_drvdata(svc);

	if (tbv_service_state && binding) {
		enum tbv_backend_type backend = binding->peer->backend;

		tbv_peer_remove_rail(binding->rail);
		if (backend == TBV_BACKEND_APPLE)
			tbv_service_refresh_apple_rails_pending(tbv_service_state);
		tbv_peer_put(tbv_service_state, binding->peer);
	}
	tb_service_set_drvdata(svc, NULL);
	kfree(binding);
}

static const struct tb_service_id tbv_service_ids[] = {
	{ TB_SERVICE(TBV_NATIVE_PROTOCOL_KEY, TBV_NATIVE_PRTCID),
	  .driver_data = 0 },
	{ TB_SERVICE("tbverb1", TBV_NATIVE_PRTCID), .driver_data = 1 },
	{ TB_SERVICE("tbverb2", TBV_NATIVE_PRTCID), .driver_data = 2 },
	{ TB_SERVICE("tbverb3", TBV_NATIVE_PRTCID), .driver_data = 3 },
#ifndef TBV_LINUX_PERF_ONLY
	{
		/*
		 * Apple's AD service key contains leading 0xff bytes. The
		 * kernel's tbsvc modalias generator formats protocol_key with
		 * "%s", which produces a non-printable alias that some depmod
		 * versions cannot index safely. Match the Apple path by its
		 * FA57 protocol tuple instead; the advertised service directory
		 * still uses the real AD key above.
		 */
		.match_flags = TBSVC_MATCH_PROTOCOL_ID |
			       TBSVC_MATCH_PROTOCOL_VERSION |
			       TBSVC_MATCH_PROTOCOL_REVISION,
		.protocol_id = TBV_APPLE_PRTCID,
		.protocol_version = TBV_APPLE_PRTCVERS,
		.protocol_revision = TBV_APPLE_PRTCREVS,
	},
#endif
	{ },
};
MODULE_DEVICE_TABLE(tbsvc, tbv_service_ids);

static struct tb_service_driver tbv_service_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = TBV_DRV_NAME,
	},
	.probe = tbv_service_probe,
	.remove = tbv_service_remove,
	.id_table = tbv_service_ids,
};

static int tbv_register_native_dir(struct tbv_state *state, u32 lane,
				   u32 prtcstns)
{
	const char *key = tbv_native_protocol_key(lane);
	int ret;

	if (!key)
		return -EINVAL;

	state->native_dirs[lane] =
		tbv_service_create_dir_common(&tbv_native_service_uuid,
					      TBV_NATIVE_PRTCID,
					      TBV_NATIVE_PRTCVERS,
					      TBV_NATIVE_PRTCREVS,
					      prtcstns);
	if (IS_ERR(state->native_dirs[lane])) {
		ret = PTR_ERR(state->native_dirs[lane]);
		state->native_dirs[lane] = NULL;
		return ret;
	}

	ret = tb_register_property_dir(key, state->native_dirs[lane]);
	if (ret) {
		tb_property_free_dir(state->native_dirs[lane]);
		state->native_dirs[lane] = NULL;
	}

	return ret;
}

static void tbv_unregister_native_dirs(struct tbv_state *state)
{
	u32 lane;

	for (lane = 0; lane < TBV_NATIVE_MAX_LANES; lane++) {
		const char *key = tbv_native_protocol_key(lane);

		if (!state->native_dirs[lane])
			continue;

		tb_unregister_property_dir(key, state->native_dirs[lane]);
		tb_property_free_dir(state->native_dirs[lane]);
		state->native_dirs[lane] = NULL;
	}
	state->native_dir_count = 0;
}

static int tbv_register_native_dirs(struct tbv_state *state, u32 prtcstns)
{
	u32 count;
	u32 lane;
	int ret;

	if (!state->cfg.native_enabled || !state->native_data)
		return 0;

	count = tbv_config_native_lane_count(state);

	if (!count || count > TBV_NATIVE_MAX_LANES) {
		pr_err("native lanes request %u exceeds supported maximum %u\n",
		       count, TBV_NATIVE_MAX_LANES);
		return -EINVAL;
	}

	for (lane = 0; lane < count; lane++) {
		ret = tbv_register_native_dir(state, lane, prtcstns);
		if (ret) {
			tbv_unregister_native_dirs(state);
			return ret;
		}
		state->native_dir_count = lane + 1;
	}

	pr_info("advertised %u native service%s\n", count,
		count == 1 ? "" : "s");
	return 0;
}

static int tbv_register_apple_dir(struct tbv_state *state, u32 prtcstns)
{
	int ret;

	state->apple_dir = tbv_service_create_apple_dir(prtcstns);
	if (IS_ERR(state->apple_dir)) {
		ret = PTR_ERR(state->apple_dir);
		state->apple_dir = NULL;
		return ret;
	}

	ret = tb_register_property_dir(tbv_apple_ad_key, state->apple_dir);
	if (ret) {
		tb_property_free_dir(state->apple_dir);
		state->apple_dir = NULL;
	}

	return ret;
}

int tbv_services_start(struct tbv_state *state, bool bind_services,
		       const struct tbv_service_config *service_cfg)
{
	int ret;

	state->allocate_rings = service_cfg->allocate_rings;
	state->start_rings = service_cfg->start_rings;
	state->negotiate_native = service_cfg->negotiate_native;
	state->enable_tunnels = service_cfg->enable_tunnels;
	/*
	 * Apple's RDMA interface is layered on an AppleThunderboltIP-backed
	 * IOEthernetInterface. In minimal-packet mode, publish the Apple verbs rail
	 * only after reciprocal LOGIN/LOGIN_RESPONSE and ThunderboltIP path
	 * programming. FA57 DMA tunnel setup is part of rail publication, so
	 * userspace cannot create a QP on an unprogrammed Apple data path.
	 */
	state->apple_rails_wait_tbnet =
		state->cfg.tbnet_identity == TBV_TBNET_ID_MINIMAL_PACKET;
	WRITE_ONCE(state->apple_rails_pending, false);
	INIT_DELAYED_WORK(&state->apple_rail_work, tbv_service_apple_rail_work);

	if (!bind_services) {
		pr_info("Thunderbolt service binding disabled\n");
		return 0;
	}

	tbv_service_state = state;

	if (state->cfg.tbnet_identity == TBV_TBNET_ID_MINIMAL_PACKET) {
		ret = tbv_tbnet_minimal_start(&state->tbnet_identity);
		if (ret)
			goto err_clear;
	}

	ret = tbv_native_control_start(state);
	if (ret)
		goto err_stop_minimal;

	ret = tbv_register_native_dirs(state, service_cfg->native_prtcstns);
	if (ret)
		goto err_stop_minimal;

	if (state->cfg.apple_enabled && state->apple_data) {
		ret = tbv_register_apple_dir(state,
					     service_cfg->apple_prtcstns);
		if (ret)
			goto err_unregister_native;
	}

	ret = tb_register_service_driver(&tbv_service_driver);
	if (ret)
		goto err_unregister_apple;

	state->services_registered = true;
	pr_info("Thunderbolt service binding enabled\n");
	if (READ_ONCE(state->apple_rails_pending))
		tbv_service_queue_apple_rail_work(state, 0);
	tbv_services_tbnet_identity_ready(&state->tbnet_identity);
	return 0;

err_unregister_apple:
	if (state->apple_dir) {
		tb_unregister_property_dir(tbv_apple_ad_key, state->apple_dir);
		tb_property_free_dir(state->apple_dir);
		state->apple_dir = NULL;
	}
err_unregister_native:
	tbv_unregister_native_dirs(state);
err_stop_minimal:
	tbv_tbnet_minimal_stop(&state->tbnet_identity);
err_clear:
	tbv_native_control_stop(state);
	tbv_service_state = NULL;
	return ret;
}

void tbv_services_stop(struct tbv_state *state)
{
	if (state->services_registered) {
		tb_unregister_service_driver(&tbv_service_driver);
		state->services_registered = false;
	}
	cancel_delayed_work_sync(&state->apple_rail_work);
	WRITE_ONCE(state->apple_rails_pending, false);

	tbv_tbnet_minimal_stop(&state->tbnet_identity);
	tbv_native_control_stop(state);

	if (state->apple_dir) {
		tb_unregister_property_dir(tbv_apple_ad_key, state->apple_dir);
		tb_property_free_dir(state->apple_dir);
		state->apple_dir = NULL;
	}

	tbv_unregister_native_dirs(state);

	tbv_service_state = NULL;
}
