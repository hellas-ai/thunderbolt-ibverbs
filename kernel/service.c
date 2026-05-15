// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/module.h>
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
		return state->cfg.apple_enabled;
	return state->cfg.native_enabled;
}

static int tbv_service_probe(struct tb_service *svc,
			     const struct tb_service_id *id)
{
	enum tbv_backend_type backend = tbv_service_backend_from_id(id);
	struct tb_xdomain *xd = tb_service_parent(svc);
	struct tbv_rail_key key;
	struct tbv_peer *peer;
	int ret;

	if (!tbv_service_state)
		return -ENODEV;

	if (!tbv_service_backend_enabled(tbv_service_state, backend))
		return -ENODEV;

	peer = tbv_peer_create(tbv_service_state, backend, xd);
	if (IS_ERR(peer))
		return PTR_ERR(peer);

	tbv_rail_key_init(&key, xd->route, xd->link, xd->depth, svc->prtcid);
	ret = tbv_peer_add_rail(peer, &key);
	if (ret) {
		tbv_peer_destroy(tbv_service_state, peer);
		return ret;
	}

	if (tbv_service_state->allocate_rings) {
		struct tbv_rail *rail = list_first_entry(&peer->rails,
							 struct tbv_rail,
							 node);

		ret = tbv_path_alloc_rings(&rail->path, xd, -1);
		if (ret) {
			tbv_peer_destroy(tbv_service_state, peer);
			return ret;
		}
		pr_info("allocated rings service id=%d tx_hop=%d rx_hop=%d out_hop=%d\n",
			svc->id, rail->path.tx_ring->hop,
			rail->path.rx_ring->hop,
			rail->path.local_transmit_path);

		if (tbv_service_state->start_rings) {
			ret = tbv_path_start_rings(&rail->path);
			if (ret) {
				tbv_peer_destroy(tbv_service_state, peer);
				return ret;
			}
			pr_info("started rings service id=%d tx_hop=%d rx_hop=%d\n",
				svc->id, rail->path.tx_ring->hop,
				rail->path.rx_ring->hop);

			if (tbv_service_state->negotiate_native)
				tbv_native_control_queue_rail(tbv_service_state,
							      rail);
		}
	}

	tb_service_set_drvdata(svc, peer);
	pr_info("bound %s service id=%d route=0x%llx link_speed=%uGb/s width=0x%x rail=0x%x\n",
		tbv_backend_name(backend), svc->id, xd->route, xd->link_speed,
		xd->link_width, tbv_rail_key_hash(&key));
	return 0;
}

static void tbv_service_remove(struct tb_service *svc)
{
	struct tbv_peer *peer = tb_service_get_drvdata(svc);

	if (tbv_service_state)
		tbv_peer_destroy(tbv_service_state, peer);
	tb_service_set_drvdata(svc, NULL);
}

static const struct tb_service_id tbv_service_ids[] = {
	{ TB_SERVICE(TBV_NATIVE_PROTOCOL_KEY, TBV_NATIVE_PRTCID) },
	{
		.match_flags = TBSVC_MATCH_PROTOCOL_KEY |
			       TBSVC_MATCH_PROTOCOL_ID |
			       TBSVC_MATCH_PROTOCOL_VERSION |
			       TBSVC_MATCH_PROTOCOL_REVISION,
		.protocol_key = {
			(char)0xff, (char)0xff, (char)0xff, (char)0xff,
			(char)0xff, (char)0xff, 'A', 'D', '\0',
		},
		.protocol_id = TBV_APPLE_PRTCID,
		.protocol_version = TBV_APPLE_PRTCVERS,
		.protocol_revision = TBV_APPLE_PRTCREVS,
	},
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

static int tbv_register_native_dir(struct tbv_state *state, u32 prtcstns)
{
	int ret;

	state->native_dir = tbv_service_create_dir_common(&tbv_native_service_uuid,
							  TBV_NATIVE_PRTCID,
							  TBV_NATIVE_PRTCVERS,
							  TBV_NATIVE_PRTCREVS,
							  prtcstns);
	if (IS_ERR(state->native_dir)) {
		ret = PTR_ERR(state->native_dir);
		state->native_dir = NULL;
		return ret;
	}

	ret = tb_register_property_dir(TBV_NATIVE_PROTOCOL_KEY,
				       state->native_dir);
	if (ret) {
		tb_property_free_dir(state->native_dir);
		state->native_dir = NULL;
	}

	return ret;
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

	ret = tbv_native_control_start(state);
	if (ret)
		goto err_clear;

	if (state->cfg.native_enabled) {
		ret = tbv_register_native_dir(state,
					      service_cfg->native_prtcstns);
		if (ret)
			goto err_clear;
	}

	if (state->cfg.apple_enabled) {
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
	if (state->native_dir) {
		tb_unregister_property_dir(TBV_NATIVE_PROTOCOL_KEY,
					   state->native_dir);
		tb_property_free_dir(state->native_dir);
		state->native_dir = NULL;
	}
err_clear:
	tbv_native_control_stop();
	tbv_service_state = NULL;
	return ret;
}

void tbv_services_stop(struct tbv_state *state)
{
	if (state->services_registered) {
		tb_unregister_service_driver(&tbv_service_driver);
		state->services_registered = false;
	}

	tbv_native_control_stop();

	if (state->apple_dir) {
		tb_unregister_property_dir(tbv_apple_protocol_key,
					   state->apple_dir);
		tb_property_free_dir(state->apple_dir);
		state->apple_dir = NULL;
	}

	if (state->native_dir) {
		tb_unregister_property_dir(TBV_NATIVE_PROTOCOL_KEY,
					   state->native_dir);
		tb_property_free_dir(state->native_dir);
		state->native_dir = NULL;
	}

	tbv_service_state = NULL;
}
