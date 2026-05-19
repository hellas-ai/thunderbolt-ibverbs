// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/err.h>
#include <linux/errno.h>
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

static const char tbv_apple_protocol_key[TB_PROPERTY_KEY_SIZE + 1] = {
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

static bool tbv_service_apple_xdomain_allowed(const struct tbv_state *state,
					      const struct tb_xdomain *xd)
{
	const char *vendor = xd && xd->vendor_name ? xd->vendor_name : NULL;

	if (state->cfg.profile != TBV_PROFILE_MIXED)
		return true;

	if (vendor && !strcmp(vendor, "Apple Inc."))
		return true;

	pr_info("skipping Apple AD/FA57 service from non-Apple peer vendor='%s' in mixed profile\n",
		vendor ?: "<unknown>");
	return false;
}

static u32 tbv_service_native_lane(const struct tb_service_id *id)
{
	return (u32)id->driver_data;
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
	    !tbv_service_apple_xdomain_allowed(tbv_service_state, xd))
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
	rail = tbv_peer_add_rail(peer, &key);
	if (IS_ERR(rail)) {
		ret = PTR_ERR(rail);
		goto err_put_peer;
	}

	rail->link_speed = xd->link_speed;
	rail->link_width = xd->link_width;

	if (tbv_service_state->allocate_rings &&
	    tbv_service_backend_data_enabled(tbv_service_state, backend)) {
		ret = tbv_path_alloc_rings(&rail->path, xd, -1);
		if (ret) {
			goto err_remove_rail;
		}
		tbv_state_set_verbs_parent(tbv_service_state, backend,
					   tb_ring_dma_device(rail->path.tx_ring));
		pr_info("allocated rings service id=%d native_lane=%u tx_hop=%d rx_hop=%d out_hop=%d\n",
			svc->id, backend == TBV_BACKEND_NATIVE ? native_lane : 0,
			rail->path.tx_ring->hop,
			rail->path.rx_ring->hop,
			rail->path.local_transmit_path);

		if (tbv_service_state->start_rings) {
			ret = tbv_path_start_rings(&rail->path);
			if (ret) {
				goto err_remove_rail;
			}
			pr_info("started rings service id=%d native_lane=%u tx_hop=%d rx_hop=%d\n",
				svc->id,
				backend == TBV_BACKEND_NATIVE ? native_lane : 0,
				rail->path.tx_ring->hop,
				rail->path.rx_ring->hop);

			if (backend == TBV_BACKEND_NATIVE &&
			    tbv_service_state->negotiate_native)
				tbv_native_control_queue_rail(tbv_service_state,
							      rail);
			else if (backend == TBV_BACKEND_APPLE &&
				 tbv_service_state->enable_tunnels) {
				ret = tbv_path_enable_tunnel(&rail->path, xd,
							     rail->path.cfg.receive_path);
				if (ret)
					goto err_remove_rail;
				pr_info("enabled Apple data path service id=%d tx_path=%d rx_path=%d tx_hop=%d rx_hop=%d\n",
					svc->id, rail->path.local_transmit_path,
					rail->path.remote_transmit_path,
					rail->path.local_tx_hop,
					rail->path.local_rx_hop);
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
		tbv_peer_remove_rail(binding->rail);
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

	if (state->cfg.requested.lanes_auto)
		count = 1;
	else
		count = state->cfg.requested.lanes_max;

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

	ret = tb_register_property_dir(tbv_apple_protocol_key,
				       state->apple_dir);
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
		goto err_clear;

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
	return 0;

err_unregister_apple:
	if (state->apple_dir) {
		tb_unregister_property_dir(tbv_apple_protocol_key,
					   state->apple_dir);
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

	tbv_tbnet_minimal_stop(&state->tbnet_identity);
	tbv_native_control_stop(state);

	if (state->apple_dir) {
		tb_unregister_property_dir(tbv_apple_protocol_key,
					   state->apple_dir);
		tb_property_free_dir(state->apple_dir);
		state->apple_dir = NULL;
	}

	tbv_unregister_native_dirs(state);

	tbv_service_state = NULL;
}
