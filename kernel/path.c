// SPDX-License-Identifier: GPL-2.0

#include <linux/errno.h>
#include <linux/string.h>
#include <linux/thunderbolt.h>

#include "tbv.h"

#define TBV_NATIVE_RING_SIZE 1024
#define TBV_APPLE_RING_SIZE 256

void tbv_path_default_config(enum tbv_backend_type backend,
			     struct tbv_path_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));

	switch (backend) {
	case TBV_BACKEND_APPLE:
		cfg->tx_ring_size = TBV_APPLE_RING_SIZE;
		cfg->rx_ring_size = TBV_APPLE_RING_SIZE;
		cfg->tx_flags = RING_FLAG_FRAME;
		cfg->rx_flags = RING_FLAG_FRAME | RING_FLAG_E2E;
		cfg->sof_mask = BIT(1);
		cfg->eof_mask = BIT(2) | BIT(3);
		cfg->e2e = true;
		break;

	case TBV_BACKEND_NATIVE:
	default:
		cfg->tx_ring_size = TBV_NATIVE_RING_SIZE;
		cfg->rx_ring_size = TBV_NATIVE_RING_SIZE;
		cfg->tx_flags = RING_FLAG_FRAME;
		cfg->rx_flags = RING_FLAG_FRAME;
		cfg->sof_mask = BIT(1);
		cfg->eof_mask = BIT(2) | BIT(3);
		cfg->e2e = false;
		break;
	}
}

void tbv_path_init(struct tbv_path *path,
		   const struct tbv_path_config *cfg)
{
	memset(path, 0, sizeof(*path));
	path->state = TBV_PATH_NEW;
	path->cfg = *cfg;
	path->local_transmit_path = -1;
	path->remote_transmit_path = -1;
}

void tbv_path_reset(struct tbv_path *path)
{
	path->tx_ring = NULL;
	path->rx_ring = NULL;
	memset(path, 0, sizeof(*path));
	path->state = TBV_PATH_STOPPED;
	path->local_transmit_path = -1;
	path->remote_transmit_path = -1;
}

const char *tbv_path_state_name(enum tbv_path_state state)
{
	switch (state) {
	case TBV_PATH_NEW:
		return "new";
	case TBV_PATH_RING_ALLOCATED:
		return "ring_allocated";
	case TBV_PATH_RING_STARTED:
		return "ring_started";
	case TBV_PATH_TUNNEL_ENABLED:
		return "tunnel_enabled";
	case TBV_PATH_STOPPED:
		return "stopped";
	default:
		return "unknown";
	}
}

int tbv_path_alloc_rings(struct tbv_path *path, struct tb_xdomain *xd,
			 int requested_transmit_path)
{
	int e2e_tx_hop = 0;
	int transmit_path;

	if (path->state != TBV_PATH_NEW && path->state != TBV_PATH_STOPPED)
		return -EBUSY;

	path->tx_ring = tb_ring_alloc_tx(xd->tb->nhi, -1,
					 path->cfg.tx_ring_size,
					 path->cfg.tx_flags);
	if (!path->tx_ring)
		return -ENOMEM;

	transmit_path = tb_xdomain_alloc_out_hopid(xd,
						   requested_transmit_path);
	if (transmit_path < 0) {
		tb_ring_free(path->tx_ring);
		path->tx_ring = NULL;
		return transmit_path;
	}
	path->local_transmit_path = transmit_path;

	if (path->cfg.e2e)
		e2e_tx_hop = path->tx_ring->hop;

	path->rx_ring = tb_ring_alloc_rx(xd->tb->nhi, -1,
					 path->cfg.rx_ring_size,
					 path->cfg.rx_flags, e2e_tx_hop,
					 path->cfg.sof_mask,
					 path->cfg.eof_mask, NULL, NULL);
	if (!path->rx_ring) {
		tb_xdomain_release_out_hopid(xd, path->local_transmit_path);
		path->local_transmit_path = -1;
		tb_ring_free(path->tx_ring);
		path->tx_ring = NULL;
		return -ENOMEM;
	}

	path->state = TBV_PATH_RING_ALLOCATED;
	return 0;
}

int tbv_path_start_rings(struct tbv_path *path)
{
	if (path->state != TBV_PATH_RING_ALLOCATED)
		return -EINVAL;

	tb_ring_start(path->tx_ring);
	tb_ring_start(path->rx_ring);
	path->state = TBV_PATH_RING_STARTED;
	return 0;
}

int tbv_path_enable_tunnel(struct tbv_path *path, struct tb_xdomain *xd,
			   int remote_transmit_path)
{
	int ret;

	if (path->state != TBV_PATH_RING_STARTED)
		return -EINVAL;

	ret = tb_xdomain_alloc_in_hopid(xd, remote_transmit_path);
	if (ret != remote_transmit_path)
		return ret < 0 ? ret : -EBUSY;

	ret = tb_xdomain_enable_paths(xd, path->local_transmit_path,
				      path->tx_ring->hop,
				      remote_transmit_path,
				      path->rx_ring->hop);
	if (ret) {
		tb_xdomain_release_in_hopid(xd, remote_transmit_path);
		return ret;
	}

	path->remote_transmit_path = remote_transmit_path;
	path->state = TBV_PATH_TUNNEL_ENABLED;
	return 0;
}

void tbv_path_destroy(struct tbv_path *path, struct tb_xdomain *xd)
{
	if (path->state == TBV_PATH_TUNNEL_ENABLED) {
		tb_xdomain_disable_paths(xd, path->local_transmit_path,
					 path->tx_ring ? path->tx_ring->hop : -1,
					 path->remote_transmit_path,
					 path->rx_ring ? path->rx_ring->hop : -1);
		tb_xdomain_release_in_hopid(xd, path->remote_transmit_path);
		path->remote_transmit_path = -1;
		path->state = TBV_PATH_RING_STARTED;
	}

	if (path->state == TBV_PATH_RING_STARTED) {
		if (path->rx_ring)
			tb_ring_stop(path->rx_ring);
		if (path->tx_ring)
			tb_ring_stop(path->tx_ring);
		path->state = TBV_PATH_RING_ALLOCATED;
	}

	if (path->rx_ring) {
		tb_ring_free(path->rx_ring);
		path->rx_ring = NULL;
	}

	if (path->local_transmit_path >= 0) {
		tb_xdomain_release_out_hopid(xd, path->local_transmit_path);
		path->local_transmit_path = -1;
	}

	if (path->tx_ring) {
		tb_ring_free(path->tx_ring);
		path->tx_ring = NULL;
	}

	path->state = TBV_PATH_STOPPED;
}
