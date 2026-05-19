// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/thunderbolt.h>
#include <linux/workqueue.h>

#include "../proto/native_wire.h"
#include "tbv.h"

#define TBV_NATIVE_HELLO_RETRIES 5
#define TBV_NATIVE_TUNNEL_RETRIES 20
#define TBV_NATIVE_READY_RETRIES 10
#define TBV_NATIVE_HELLO_TIMEOUT_MS 1000
#define TBV_NATIVE_HELLO_RETRY_DELAY_MS 200
#define TBV_NATIVE_HELLO_INITIAL_DELAY_MS 100

static void tbv_native_control_work(struct work_struct *work);

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

static bool tbv_native_control_peer_matches_source(
	const struct tbv_peer *peer, const struct tb_xdomain *source_xd)
{
	return !source_xd || peer->xd == source_xd;
}

static u8 tbv_native_control_sequence(const struct tbv_rail *rail)
{
	if (rail->path.local_transmit_path >= 0)
		return rail->path.local_transmit_path & 0x3;

	return rail->rail_id & 0x3;
}

void tbv_native_control_init_rail(struct tbv_rail *rail,
				  struct tbv_peer *peer)
{
	rail->peer = peer;
	INIT_DELAYED_WORK(&rail->native_work, tbv_native_control_work);
}

void tbv_native_control_queue_rail(struct tbv_state *state,
				   struct tbv_rail *rail)
{
	rail->native_work_state = state;
	WRITE_ONCE(rail->native_work_stop, false);
	rail->native_attempts = 0;
	rail->native_tunnel_attempts = 0;
	rail->native_ready_attempts = 0;
	rail->native_last_error = 0;
	rail->native_ready_sent = false;
	rail->native_remote_ready = false;
	schedule_delayed_work(&rail->native_work,
			      msecs_to_jiffies(TBV_NATIVE_HELLO_INITIAL_DELAY_MS));
}

void tbv_native_control_cancel_rail(struct tbv_rail *rail)
{
	struct tbv_state *state = rail->native_work_state;

	if (state) {
		mutex_lock(&state->lock);
		WRITE_ONCE(rail->native_work_stop, true);
		mutex_unlock(&state->lock);
	} else {
		WRITE_ONCE(rail->native_work_stop, true);
	}
	cancel_delayed_work_sync(&rail->native_work);
	rail->native_work_state = NULL;
}

static int tbv_native_control_snapshot(struct tbv_state *state,
				       const struct tb_xdomain *source_xd,
				       const struct tbv_native_wire_info *info,
				       u32 rail_id,
				       bool require_matching_rail,
				       bool require_tunnel_enabled,
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
		if (!tbv_native_control_peer_matches_source(peer, source_xd))
			continue;

		list_for_each_entry(rail, &peer->rails, node) {
			if (rail->key.route != info->route)
				continue;
			if (require_matching_rail && rail->rail_id != rail_id)
				continue;
			if (require_tunnel_enabled &&
			    rail->path.state != TBV_PATH_TUNNEL_ENABLED)
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
					   const struct tb_xdomain *source_xd,
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
		if (!tbv_native_control_peer_matches_source(peer, source_xd))
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
					const struct tb_xdomain *source_xd,
					const struct tbv_native_wire_info *info,
					const struct tbv_native_wire_hello *remote)
{
	return tbv_native_control_apply_remote(state, source_xd, info, remote,
					       true);
}

static int tbv_native_control_mark_remote_ready(struct tbv_state *state,
						const struct tb_xdomain *source_xd,
						const struct tbv_native_wire_info *info,
						const struct tbv_native_wire_hello *remote)
{
	struct tbv_peer *peer;
	int ret = -ENOENT;

	mutex_lock(&state->lock);
	list_for_each_entry(peer, &state->peers, node) {
		struct tbv_rail *rail;

		if (peer->backend != TBV_BACKEND_NATIVE)
			continue;
		if (!tbv_native_control_peer_matches_source(peer, source_xd))
			continue;

		list_for_each_entry(rail, &peer->rails, node) {
			if (rail->key.route != info->route ||
			    rail->rail_id != remote->rail_id)
				continue;

			rail->native_remote_ready = true;
			ret = 0;
			goto out;
		}
	}

out:
	mutex_unlock(&state->lock);
	return ret;
}

int tbv_native_control_handle_packet(struct tbv_state *state,
				     struct tb_xdomain *source_xd,
				     const void *buf, size_t size)
{
	struct tbv_native_wire_hello remote;
	struct tbv_native_wire_hello local;
	struct tbv_native_wire_info info;
	u8 reply[TBV_NATIVE_WIRE_HELLO_MSG_SIZE];
	struct tb_xdomain *xd = NULL;
	int ret;

	if (!state)
		return 0;

	ret = tbv_native_wire_parse_hello(buf, size, &remote, &info);
	if (ret)
		return 0;

	if (info.op == TBV_NATIVE_WIRE_OP_HELLO_ACK) {
		ret = tbv_native_control_apply_ack(state, source_xd, &info,
						   &remote);
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

	if (info.op == TBV_NATIVE_WIRE_OP_READY_ACK)
		return 0;

	if (info.op == TBV_NATIVE_WIRE_OP_READY) {
		memset(&local, 0, sizeof(local));
		ret = tbv_native_control_snapshot(state, source_xd, &info,
						  remote.rail_id, true,
						  true,
						  &local, &xd);
		if (ret) {
			pr_warn_ratelimited("native READY route=0x%llx rail=0x%x has no tunnel-enabled peer rail\n",
					    info.route, remote.rail_id);
			return 1;
		}

		ret = tbv_native_control_mark_remote_ready(state, source_xd,
							  &info, &remote);
		if (!ret)
			pr_info("native READY received route=0x%llx rail=0x%x\n",
				info.route, remote.rail_id);

		ret = tbv_native_wire_build_hello(reply, sizeof(reply),
						  &local,
						  TBV_NATIVE_WIRE_OP_READY_ACK,
						  0, info.seq, local.route,
						  info.xdomain_sequence);
		if (ret >= 0)
			ret = tb_xdomain_response(xd, reply, sizeof(reply),
						  TB_CFG_PKG_XDOMAIN_RESP);

		if (ret < 0)
			pr_warn("native READY_ACK route=0x%llx rail=0x%x failed: %d\n",
				info.route, remote.rail_id, ret);

		tb_xdomain_put(xd);
		return 1;
	}

	if (info.op != TBV_NATIVE_WIRE_OP_HELLO)
		return 0;

	memset(&local, 0, sizeof(local));
	ret = tbv_native_control_snapshot(state, source_xd, &info,
					  remote.rail_id, true, false, &local,
					  &xd);
	if (ret) {
		pr_warn("native HELLO route=0x%llx rail=0x%x has no matching peer rail\n",
			info.route, remote.rail_id);
		return 1;
	}

	ret = tbv_native_control_apply_remote(state, source_xd, &info,
					      &remote, true);
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

static int tbv_native_control_exchange_once(struct tbv_state *state,
					    struct tbv_peer *peer,
					    struct tbv_rail *rail,
					    u32 attempt)
{
	struct tbv_native_wire_hello local;
	struct tbv_native_wire_hello remote;
	u8 response[TBV_NATIVE_WIRE_HELLO_MSG_SIZE];
	u8 request[TBV_NATIVE_WIRE_HELLO_MSG_SIZE];
	struct tbv_native_wire_info info;
	int ret;

	if (peer->backend != TBV_BACKEND_NATIVE)
		return 0;

	if (rail->path.state != TBV_PATH_RING_STARTED)
		return -EINVAL;

	tbv_native_control_fill_hello(state, peer, rail, &local);
	ret = tbv_native_wire_build_hello(request, sizeof(request), &local,
					  TBV_NATIVE_WIRE_OP_HELLO, 0,
					  rail->rail_id, local.route,
					  tbv_native_control_sequence(rail));
	if (ret < 0)
		return ret;

	memset(response, 0, sizeof(response));
	ret = tb_xdomain_request(peer->xd, request, sizeof(request),
				 TB_CFG_PKG_XDOMAIN_REQ, response,
				 sizeof(response),
				 TB_CFG_PKG_XDOMAIN_RESP,
				 TBV_NATIVE_HELLO_TIMEOUT_MS);
	if (ret)
		return ret;

	ret = tbv_native_wire_parse_hello(response, sizeof(response), &remote,
					 &info);
	if (ret)
		return ret;
	if (info.op != TBV_NATIVE_WIRE_OP_HELLO_ACK)
		return -EPROTO;
	ret = tbv_native_control_apply_ack(state, peer->xd, &info, &remote);
	if (ret)
		return ret;

	pr_info("native HELLO negotiated route=0x%llx rail=0x%x remote_out=%u remote_tx=%u remote_rx=%u attempt=%u\n",
		info.route, remote.rail_id, remote.transmit_path,
		remote.tx_hop, remote.rx_hop, attempt);
	return 0;
}

static int tbv_native_control_ready_once(struct tbv_state *state,
					 struct tbv_peer *peer,
					 struct tbv_rail *rail)
{
	struct tbv_native_wire_hello local;
	struct tbv_native_wire_hello remote;
	u8 response[TBV_NATIVE_WIRE_HELLO_MSG_SIZE];
	u8 request[TBV_NATIVE_WIRE_HELLO_MSG_SIZE];
	struct tbv_native_wire_info info;
	int ret;

	if (peer->backend != TBV_BACKEND_NATIVE)
		return 0;

	if (rail->path.state != TBV_PATH_TUNNEL_ENABLED)
		return -EINVAL;

	tbv_native_control_fill_hello(state, peer, rail, &local);
	ret = tbv_native_wire_build_hello(request, sizeof(request), &local,
					  TBV_NATIVE_WIRE_OP_READY, 0,
					  rail->rail_id, local.route,
					  tbv_native_control_sequence(rail));
	if (ret < 0)
		return ret;

	memset(response, 0, sizeof(response));
	ret = tb_xdomain_request(peer->xd, request, sizeof(request),
				 TB_CFG_PKG_XDOMAIN_REQ, response,
				 sizeof(response),
				 TB_CFG_PKG_XDOMAIN_RESP,
				 TBV_NATIVE_HELLO_TIMEOUT_MS);
	if (ret)
		return ret;

	ret = tbv_native_wire_parse_hello(response, sizeof(response), &remote,
					 &info);
	if (ret)
		return ret;
	if (info.op != TBV_NATIVE_WIRE_OP_READY_ACK ||
	    remote.rail_id != rail->rail_id)
		return -EPROTO;

	ret = tbv_native_control_mark_remote_ready(state, peer->xd, &info,
						  &remote);
	if (ret)
		return ret;

	pr_info("native READY sent route=0x%llx rail=0x%x\n",
		info.route, rail->rail_id);
	return 0;
}

int tbv_native_control_exchange(struct tbv_state *state, struct tbv_peer *peer,
				struct tbv_rail *rail)
{
	u32 attempt;
	int ret = 0;

	for (attempt = 1; attempt <= TBV_NATIVE_HELLO_RETRIES; attempt++) {
		ret = tbv_native_control_exchange_once(state, peer, rail,
						       attempt);
		if (!ret)
			return 0;
		if (ret != -ETIMEDOUT)
			return ret;
		if (attempt < TBV_NATIVE_HELLO_RETRIES)
			msleep(TBV_NATIVE_HELLO_RETRY_DELAY_MS);
	}

	pr_warn("native HELLO route=0x%llx rail=0x%x timed out after %d attempts\n",
		rail->key.route, rail->rail_id, TBV_NATIVE_HELLO_RETRIES);
	return ret;
}

static void tbv_native_control_work(struct work_struct *work)
{
	struct tbv_rail *rail =
		container_of(to_delayed_work(work), struct tbv_rail,
			     native_work);
	struct tbv_state *state = rail->native_work_state;
	struct tbv_peer *peer = rail->peer;
	bool retry = false;
	u32 attempt;
	int ret = 0;

	if (!state || !peer || READ_ONCE(rail->native_work_stop))
		return;

	if (peer->backend != TBV_BACKEND_NATIVE)
		return;

	if (rail->path.state != TBV_PATH_RING_STARTED &&
	    rail->path.state != TBV_PATH_TUNNEL_ENABLED)
		return;

	if (state->negotiate_native && !rail->native_negotiated) {
		attempt = ++rail->native_attempts;
		ret = tbv_native_control_exchange_once(state, peer, rail,
						       attempt);
		rail->native_last_error = ret;
		if (ret) {
			if (attempt < TBV_NATIVE_HELLO_RETRIES) {
				retry = true;
			} else {
				pr_warn("native HELLO route=0x%llx rail=0x%x failed after %u attempts: %d\n",
					rail->key.route, rail->rail_id,
					attempt, ret);
			}
			goto out;
		}
	}

	if (state->enable_tunnels &&
	    rail->path.state == TBV_PATH_RING_STARTED) {
		if (!rail->native_negotiated ||
		    rail->remote_transmit_path < 0) {
			ret = -ENOTCONN;
			rail->native_last_error = ret;
			goto out;
		}

		attempt = ++rail->native_tunnel_attempts;
		ret = tbv_path_enable_tunnel(&rail->path, peer->xd,
					     rail->remote_transmit_path);
		rail->native_last_error = ret;
		if (ret) {
			if (attempt < TBV_NATIVE_TUNNEL_RETRIES) {
				retry = true;
			} else {
				pr_warn("native tunnel route=0x%llx rail=0x%x enable failed after %u attempts: %d\n",
					rail->key.route, rail->rail_id,
					attempt, ret);
			}
			goto out;
		}

		pr_info("enabled tunnel route=0x%llx rail=0x%x out_hop=%d remote_out_hop=%d tx_hop=%d rx_hop=%d\n",
			rail->key.route, rail->rail_id,
			rail->path.local_transmit_path,
			rail->remote_transmit_path,
			rail->path.tx_ring->hop, rail->path.rx_ring->hop);
	}

	if (state->enable_tunnels &&
	    rail->path.state == TBV_PATH_TUNNEL_ENABLED &&
	    !rail->native_remote_ready) {
		attempt = ++rail->native_ready_attempts;
		ret = tbv_native_control_ready_once(state, peer, rail);
		rail->native_last_error = ret;
		if (ret) {
			if (attempt < TBV_NATIVE_READY_RETRIES) {
				retry = true;
			} else {
				pr_warn("native READY route=0x%llx rail=0x%x failed after %u attempts: %d\n",
					rail->key.route, rail->rail_id,
					attempt, ret);
			}
			goto out;
		}
		rail->native_ready_sent = true;
	}

out:
	if (retry) {
		mutex_lock(&state->lock);
		if (!READ_ONCE(rail->native_work_stop))
			schedule_delayed_work(&rail->native_work,
					      msecs_to_jiffies(TBV_NATIVE_HELLO_RETRY_DELAY_MS));
		mutex_unlock(&state->lock);
	}
}

int tbv_native_control_start(struct tbv_state *state)
{
	int ret;

	if (!state->cfg.native_enabled)
		return 0;

#ifdef TB_PROTOCOL_HANDLER_HAS_XDOMAIN
	state->native_control_source_aware = true;
	ret = tbv_native_control_xdomain_start(state);
#else
	state->native_control_source_aware = false;
	ret = tbv_native_control_legacy_start(state);
#endif
	if (!ret)
		state->native_control_registered = true;
	return ret;
}

void tbv_native_control_stop(struct tbv_state *state)
{
#ifdef TB_PROTOCOL_HANDLER_HAS_XDOMAIN
	tbv_native_control_xdomain_stop();
#else
	tbv_native_control_legacy_stop();
#endif
	if (state) {
		state->native_control_registered = false;
		state->native_control_source_aware = false;
	}
}

const char *tbv_native_control_mode_name(const struct tbv_state *state)
{
	if (!state || !state->cfg.native_enabled ||
	    !state->native_control_registered)
		return "off";

	return state->native_control_source_aware ? "source_aware" : "legacy";
}
