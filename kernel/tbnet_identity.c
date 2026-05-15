// SPDX-License-Identifier: GPL-2.0
/*
 * Apple-compatible TBnet identity policy.
 *
 * This file will eventually own the minimal ThunderboltIP identity backend.
 * For now it validates profile combinations so the public module starts with
 * explicit behavior instead of hidden Mac-specific assumptions.
 */

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/errno.h>
#include <linux/if_arp.h>
#include <linux/if_ether.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "tbv.h"

#define TBV_TBIP_LOGIN_PROTO_VERSION	1
#define TBV_TBIP_HDR_LENGTH_MASK	GENMASK(5, 0)
#define TBV_TBIP_HDR_SN_MASK		GENMASK(28, 27)
#define TBV_TBIP_HDR_SN_SHIFT		27

enum tbv_tbip_type {
	TBV_TBIP_LOGIN,
	TBV_TBIP_LOGIN_RESPONSE,
	TBV_TBIP_LOGOUT,
	TBV_TBIP_STATUS,
};

struct tbv_tbip_header {
	u32 route_hi;
	u32 route_lo;
	u32 length_sn;
	uuid_t uuid;
	uuid_t initiator_uuid;
	uuid_t target_uuid;
	u32 type;
	u32 command_id;
};

struct tbv_tbip_login {
	struct tbv_tbip_header hdr;
	u32 proto_version;
	u32 transmit_path;
	u32 reserved[4];
};

struct tbv_tbip_login_response {
	struct tbv_tbip_header hdr;
	u32 status;
	u32 receiver_mac[2];
	u32 receiver_mac_len;
	u32 reserved[4];
};

struct tbv_arp_ipv4_payload {
	u8 sender_mac[TBV_ETH_ALEN];
	__be32 sender_ip;
	u8 target_mac[TBV_ETH_ALEN];
	__be32 target_ip;
} __packed;

/* ThunderboltIP protocol UUID: 798f589e-3616-8a47-97c6-5664a920c8dd */
static const uuid_t tbv_tbip_uuid =
	UUID_INIT(0x798f589e, 0x3616, 0x8a47, 0x97, 0xc6,
		  0x56, 0x64, 0xa9, 0x20, 0xc8, 0xdd);

static void tbv_tbip_fill_header(struct tbv_tbip_header *hdr,
				 const struct tbv_tbip_control *ctrl,
				 enum tbv_tbip_type type, size_t size)
{
	u32 length_sn;

	length_sn = (size - 3 * sizeof(u32)) / sizeof(u32);
	length_sn |= (ctrl->sequence << TBV_TBIP_HDR_SN_SHIFT) &
		     TBV_TBIP_HDR_SN_MASK;

	hdr->route_hi = upper_32_bits(ctrl->route);
	hdr->route_lo = lower_32_bits(ctrl->route);
	hdr->length_sn = length_sn;
	uuid_copy(&hdr->uuid, &tbv_tbip_uuid);
	uuid_copy(&hdr->initiator_uuid, &ctrl->initiator_uuid);
	uuid_copy(&hdr->target_uuid, &ctrl->target_uuid);
	hdr->type = type;
	hdr->command_id = ctrl->command_id;
}

int tbv_tbip_build_login(void *buf, size_t size,
			 const struct tbv_tbip_login_params *params)
{
	struct tbv_tbip_login *msg = buf;

	if (!buf || !params)
		return -EINVAL;
	if (size < sizeof(*msg))
		return -ENOSPC;

	memset(msg, 0, sizeof(*msg));
	tbv_tbip_fill_header(&msg->hdr, &params->ctrl, TBV_TBIP_LOGIN,
			     sizeof(*msg));
	msg->proto_version = TBV_TBIP_LOGIN_PROTO_VERSION;
	msg->transmit_path = params->transmit_path;
	return sizeof(*msg);
}

int tbv_tbip_build_login_response(void *buf, size_t size,
				  const struct tbv_tbip_login_response_params *params)
{
	struct tbv_tbip_login_response *msg = buf;

	if (!buf || !params)
		return -EINVAL;
	if (size < sizeof(*msg))
		return -ENOSPC;

	memset(msg, 0, sizeof(*msg));
	tbv_tbip_fill_header(&msg->hdr, &params->ctrl,
			     TBV_TBIP_LOGIN_RESPONSE, sizeof(*msg));
	msg->status = params->status;
	memcpy(msg->receiver_mac, params->receiver_mac, TBV_ETH_ALEN);
	msg->receiver_mac_len = TBV_ETH_ALEN;
	return sizeof(*msg);
}

int tbv_tbip_parse_login(const void *buf, size_t size,
			 struct tbv_tbip_login_params *params)
{
	const struct tbv_tbip_login *msg = buf;

	if (!buf || !params)
		return -EINVAL;
	if (size < sizeof(*msg))
		return -EINVAL;
	if (!uuid_equal(&msg->hdr.uuid, &tbv_tbip_uuid))
		return -EPROTO;
	if (msg->hdr.type != TBV_TBIP_LOGIN)
		return -EPROTO;
	if (msg->proto_version != TBV_TBIP_LOGIN_PROTO_VERSION)
		return -EPROTONOSUPPORT;

	memset(params, 0, sizeof(*params));
	params->ctrl.route = ((u64)msg->hdr.route_hi << 32) |
			     msg->hdr.route_lo;
	params->ctrl.route &= ~BIT_ULL(63);
	params->ctrl.sequence = (msg->hdr.length_sn & TBV_TBIP_HDR_SN_MASK) >>
				TBV_TBIP_HDR_SN_SHIFT;
	uuid_copy(&params->ctrl.initiator_uuid, &msg->hdr.initiator_uuid);
	uuid_copy(&params->ctrl.target_uuid, &msg->hdr.target_uuid);
	params->ctrl.command_id = msg->hdr.command_id;
	params->transmit_path = msg->transmit_path;
	return 0;
}

static bool tbv_arp_is_ipv4_ethernet(const struct arphdr *arp)
{
	return arp->ar_hrd == htons(ARPHRD_ETHER) &&
	       arp->ar_pro == htons(ETH_P_IP) &&
	       arp->ar_hln == TBV_ETH_ALEN &&
	       arp->ar_pln == sizeof(__be32);
}

int tbv_tbnet_arp_reply_for_request(void *reply, size_t reply_size,
				    const void *request, size_t request_size,
				    const struct tbv_tbnet_arp_proxy *proxy)
{
	const struct tbv_arp_ipv4_payload *req_payload;
	struct tbv_arp_ipv4_payload *reply_payload;
	const struct ethhdr *req_eth = request;
	struct ethhdr *reply_eth = reply;
	const struct arphdr *req_arp;
	struct arphdr *reply_arp;
	size_t frame_len;

	if (!reply || !request || !proxy)
		return -EINVAL;

	frame_len = sizeof(struct ethhdr) + sizeof(struct arphdr) +
		    sizeof(struct tbv_arp_ipv4_payload);
	if (reply_size < frame_len)
		return -ENOSPC;
	if (request_size < frame_len)
		return -EINVAL;

	if (req_eth->h_proto != htons(ETH_P_ARP))
		return -ENOENT;

	req_arp = (const struct arphdr *)(req_eth + 1);
	if (!tbv_arp_is_ipv4_ethernet(req_arp))
		return -ENOENT;
	if (req_arp->ar_op != htons(ARPOP_REQUEST))
		return -ENOENT;

	req_payload = (const struct tbv_arp_ipv4_payload *)(req_arp + 1);
	if (req_payload->target_ip != proxy->ipv4)
		return -ENOENT;

	memset(reply, 0, frame_len);

	memcpy(reply_eth->h_dest, req_payload->sender_mac, TBV_ETH_ALEN);
	memcpy(reply_eth->h_source, proxy->mac, TBV_ETH_ALEN);
	reply_eth->h_proto = htons(ETH_P_ARP);

	reply_arp = (struct arphdr *)(reply_eth + 1);
	reply_arp->ar_hrd = htons(ARPHRD_ETHER);
	reply_arp->ar_pro = htons(ETH_P_IP);
	reply_arp->ar_hln = TBV_ETH_ALEN;
	reply_arp->ar_pln = sizeof(__be32);
	reply_arp->ar_op = htons(ARPOP_REPLY);

	reply_payload = (struct tbv_arp_ipv4_payload *)(reply_arp + 1);
	memcpy(reply_payload->sender_mac, proxy->mac, TBV_ETH_ALEN);
	reply_payload->sender_ip = proxy->ipv4;
	memcpy(reply_payload->target_mac, req_payload->sender_mac,
	       TBV_ETH_ALEN);
	reply_payload->target_ip = req_payload->sender_ip;

	return frame_len;
}

int tbv_tbnet_identity_check_config(const struct tbv_resolved_config *cfg)
{
	const struct tbv_config *requested = &cfg->requested;

	if (cfg->apple_enabled &&
	    cfg->tbnet_identity == TBV_TBNET_ID_OFF) {
		pr_err("Apple-compatible backend requires TBnet identity\n");
		return -EINVAL;
	}

	if (requested->tbnet == TBV_TBNET_BLOCK &&
	    (cfg->tbnet_identity == TBV_TBNET_ID_STOCK ||
	     cfg->tbnet_identity == TBV_TBNET_ID_STOCK_PROXY)) {
		pr_err("tbnet=block conflicts with tbnet_identity=%s\n",
		       tbv_tbnet_identity_name(cfg->tbnet_identity));
		return -EINVAL;
	}

	if (cfg->profile == TBV_PROFILE_LINUX_PERF &&
	    requested->tbnet_identity != TBV_TBNET_ID_AUTO &&
	    cfg->tbnet_identity != TBV_TBNET_ID_OFF) {
		pr_warn("linux_perf ignores Apple TBnet identity unless an Apple peer is selected\n");
	}

	return 0;
}

int tbv_tbnet_identity_prepare(struct tbv_tbnet_identity *identity,
			       const struct tbv_resolved_config *cfg)
{
	memset(identity, 0, sizeof(*identity));
	identity->mode = cfg->tbnet_identity;

	if (!cfg->apple_enabled || cfg->tbnet_identity == TBV_TBNET_ID_OFF)
		return 0;

	switch (cfg->tbnet_identity) {
	case TBV_TBNET_ID_STOCK:
		identity->state = TBV_TBNET_ID_STATE_CARRIER |
				  TBV_TBNET_ID_STATE_NEIGHBOR_READY |
				  TBV_TBNET_ID_STATE_PACKET_PATH_ACTIVE |
				  TBV_TBNET_ID_STATE_FULL_IP_ACTIVE;
		pr_info("TBnet identity uses stock ThunderboltIP\n");
		return 0;

	case TBV_TBNET_ID_STOCK_PROXY:
		identity->state = TBV_TBNET_ID_STATE_CARRIER |
				  TBV_TBNET_ID_STATE_NEIGHBOR_READY |
				  TBV_TBNET_ID_STATE_PACKET_PATH_ACTIVE;
		pr_info("TBnet identity uses stock ThunderboltIP with RDMA GID proxying\n");
		return 0;

	case TBV_TBNET_ID_MINIMAL_PACKET:
		pr_err("tbnet_identity=minimal_packet is designed but not implemented\n");
		return -EOPNOTSUPP;

	case TBV_TBNET_ID_AUTO:
	case TBV_TBNET_ID_OFF:
	default:
		return 0;
	}
}

void tbv_tbnet_identity_stop(struct tbv_tbnet_identity *identity)
{
	identity->state = 0;
}
