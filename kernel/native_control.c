// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/thunderbolt.h>

#include "../proto/native_wire.h"
#include "tbv.h"

#define TBV_NATIVE_HELLO_RETRIES 5
#define TBV_NATIVE_HELLO_TIMEOUT_MS 1000
#define TBV_NATIVE_HELLO_RETRY_DELAY_MS 200

static struct tb_protocol_handler tbv_native_handler;
static bool tbv_native_handler_registered;

static u32 tbv_native_control_caps(const struct tbv_state *state,
				   const struct tbv_peer *peer)
{
	u32 caps = 0;

	if (state->cfg.uc_supported)
		caps |= TBV_NATIVE_WIRE_CAP_UC;
	if (state->cfg.rc_supported)
		caps |= TBV_NATIVE_WIRE_CAP_RC;
	if (peer->nr_rails > 1)
		caps |= TBV_NATIVE_WIRE_CAP_MULTI_RAIL;

	return caps;
}

static u32 tbv_native_control_path_flags(const struct tbv_path *path)
{
	u32 flags = 0;

	if (path->cfg.tx_flags & RING_FLAG_FRAME)
		flags |= TBV_NATIVE_WIRE_PATH_FRAME;
	if (path->cfg.e2e)
		flags |= TBV_NATIVE_WIRE_PATH_E2E;

	return flags;
}

static void tbv_native_control_fill_hello(const struct tbv_state *state,
					  const struct tbv_peer *peer,
					  const struct tbv_rail *rail,
					  struct tbv_native_wire_hello *hello)
{
	memset(hello, 0, sizeof(*hello));
	hello->capabilities = tbv_native_control_caps(state, peer);
	hello->rail_id = rail->rail_id;
	hello->route = rail->key.route;
	hello->tx_hop = rail->path.tx_ring ?
			rail->path.tx_ring->hop : U32_MAX;
	hello->rx_hop = rail->path.rx_ring ?
			rail->path.rx_ring->hop : U32_MAX;
	hello->transmit_path = rail->path.local_transmit_path >= 0 ?
			rail->path.local_transmit_path : U32_MAX;
	hello->tx_ring_size = rail->path.cfg.tx_ring_size;
	hello->rx_ring_size = rail->path.cfg.rx_ring_size;
	hello->path_flags = tbv_native_control_path_flags(&rail->path);
}

static int tbv_native_control_snapshot(struct tbv_state *state,
				       const struct tbv_native_wire_info *info,
				       struct tbv_native_wire_hello *hello,
				       struct tb_xdomain **xd)
{
	struct tbv_peer *peer;
	int ret = -ENOENT;

	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		if (peer->backend != TBV_BACKEND_NATIVE)
			continue;

		list_for_each_entry(rail, &peer->rails, node) {
			if (rail->key.route != info->route)
				continue;

			tbv_native_control_fill_hello(state, peer, rail,
						      hello);
			*xd = tb_xdomain_get(peer->xd);
			ret = 0;
			goto out;
		}
	}

out:
	mutex_unlock(&state->lock);
	return ret;
}

static int tbv_native_control_apply_remote(struct tbv_state *state,
					   const struct tbv_native_wire_info *info,
					   const struct tbv_native_wire_hello *remote,
					   bool require_matching_rail)
{
	struct tbv_peer *peer;
	int ret = -ENOENT;

	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		if (peer->backend != TBV_BACKEND_NATIVE)
			continue;

		list_for_each_entry(rail, &peer->rails, node) {
			if (rail->key.route != info->route)
				continue;
			if (require_matching_rail &&
			    rail->rail_id != remote->rail_id)
				continue;

			rail->native_negotiated = true;
			rail->remote_rail_id = remote->rail_id;
			rail->remote_transmit_path = remote->transmit_path;
			rail->remote_tx_hop = remote->tx_hop;
			rail->remote_rx_hop = remote->rx_hop;
			ret = 0;
			goto out;
		}
	}

out:
	mutex_unlock(&state->lock);
	return ret;
}

static int tbv_native_control_apply_ack(struct tbv_state *state,
					const struct tbv_native_wire_info *info,
					const struct tbv_native_wire_hello *remote)
{
	return tbv_native_control_apply_remote(state, info, remote, true);
}

static int tbv_native_control_handle(const void *buf, size_t size, void *data)
{
	struct tbv_state *state = data;
	struct tbv_native_wire_hello remote;
	struct tbv_native_wire_hello local;
	struct tbv_native_wire_info info;
	u8 reply[TBV_NATIVE_WIRE_HELLO_MSG_SIZE];
	struct tb_xdomain *xd = NULL;
	int ret;

	ret = tbv_native_wire_parse_hello(buf, size, &remote, &info);
	if (ret)
		return 0;

	if (info.op == TBV_NATIVE_WIRE_OP_HELLO_ACK) {
		ret = tbv_native_control_apply_ack(state, &info, &remote);
		if (!ret)
			pr_info("native HELLO_ACK received route=0x%llx rail=0x%x remote_out=%u remote_tx=%u remote_rx=%u\n",
				info.route, remote.rail_id,
				remote.transmit_path, remote.tx_hop,
				remote.rx_hop);
		/*
		 * Let tb_xdomain_request() match the ACK after observing it.
		 * XDomain dispatch calls protocol handlers before request
		 * matching, so consuming the ACK here would make the sender
		 * time out even though the peer replied correctly.
		 */
		return 0;
	}

	if (info.op != TBV_NATIVE_WIRE_OP_HELLO)
		return 0;

	memset(&local, 0, sizeof(local));
	ret = tbv_native_control_snapshot(state, &info, &local, &xd);
	if (ret) {
		pr_warn("native HELLO route=0x%llx has no matching peer\n",
			info.route);
		return 1;
	}

	ret = tbv_native_control_apply_remote(state, &info, &remote, false);
	if (!ret)
		pr_info("native HELLO received route=0x%llx rail=0x%x remote_out=%u remote_tx=%u remote_rx=%u\n",
			info.route, remote.rail_id, remote.transmit_path,
			remote.tx_hop, remote.rx_hop);

	ret = tbv_native_wire_build_hello(reply, sizeof(reply), &local,
					  TBV_NATIVE_WIRE_OP_HELLO_ACK,
					  0, info.seq, local.route,
					  info.xdomain_sequence);
	if (ret >= 0)
		ret = tb_xdomain_response(xd, reply, sizeof(reply),
					  TB_CFG_PKG_XDOMAIN_RESP);

	if (ret < 0)
		pr_warn("native HELLO_ACK route=0x%llx failed: %d\n",
			info.route, ret);
	else
		pr_info("native HELLO_ACK route=0x%llx rail=0x%x tx_hop=%u rx_hop=%u out_hop=%u\n",
			info.route, local.rail_id, local.tx_hop,
			local.rx_hop, local.transmit_path);

	tb_xdomain_put(xd);
	return 1;
}

int tbv_native_control_exchange(struct tbv_state *state, struct tbv_peer *peer,
				struct tbv_rail *rail)
{
	struct tbv_native_wire_hello local;
	struct tbv_native_wire_hello remote;
	u8 response[TBV_NATIVE_WIRE_HELLO_MSG_SIZE];
	u8 request[TBV_NATIVE_WIRE_HELLO_MSG_SIZE];
	struct tbv_native_wire_info info;
	int attempt;
	int ret;

	if (peer->backend != TBV_BACKEND_NATIVE)
		return 0;

	if (rail->path.state != TBV_PATH_RING_STARTED)
		return -EINVAL;

	tbv_native_control_fill_hello(state, peer, rail, &local);
	ret = tbv_native_wire_build_hello(request, sizeof(request), &local,
					  TBV_NATIVE_WIRE_OP_HELLO, 0,
					  rail->rail_id, local.route, 0);
	if (ret < 0)
		return ret;

	for (attempt = 0; attempt < TBV_NATIVE_HELLO_RETRIES; attempt++) {
		memset(response, 0, sizeof(response));
		ret = tb_xdomain_request(peer->xd, request, sizeof(request),
					 TB_CFG_PKG_XDOMAIN_REQ, response,
					 sizeof(response),
					 TB_CFG_PKG_XDOMAIN_RESP,
					 TBV_NATIVE_HELLO_TIMEOUT_MS);
		if (!ret) {
			ret = tbv_native_wire_parse_hello(response,
							 sizeof(response),
							 &remote, &info);
			if (ret)
				return ret;
			if (info.op != TBV_NATIVE_WIRE_OP_HELLO_ACK)
				return -EPROTO;
			ret = tbv_native_control_apply_ack(state, &info,
							  &remote);
			if (ret)
				return ret;
			pr_info("native HELLO negotiated route=0x%llx rail=0x%x remote_out=%u remote_tx=%u remote_rx=%u attempt=%d\n",
				info.route, remote.rail_id,
				remote.transmit_path, remote.tx_hop,
				remote.rx_hop, attempt + 1);
			return 0;
		}
		if (ret != -ETIMEDOUT)
			return ret;
		if (attempt + 1 < TBV_NATIVE_HELLO_RETRIES)
			msleep(TBV_NATIVE_HELLO_RETRY_DELAY_MS);
	}

	pr_warn("native HELLO route=0x%llx rail=0x%x timed out after %d attempts\n",
		rail->key.route, rail->rail_id, TBV_NATIVE_HELLO_RETRIES);
	return -ETIMEDOUT;
}

int tbv_native_control_start(struct tbv_state *state)
{
	int ret;

	if (!state->cfg.native_enabled)
		return 0;

	memset(&tbv_native_handler, 0, sizeof(tbv_native_handler));
	tbv_native_handler.uuid = &tbv_native_service_uuid;
	tbv_native_handler.callback = tbv_native_control_handle;
	tbv_native_handler.data = state;

	ret = tb_register_protocol_handler(&tbv_native_handler);
	if (ret)
		return ret;

	tbv_native_handler_registered = true;
	return 0;
}

void tbv_native_control_stop(void)
{
	if (!tbv_native_handler_registered)
		return;

	tb_unregister_protocol_handler(&tbv_native_handler);
	tbv_native_handler_registered = false;
}
