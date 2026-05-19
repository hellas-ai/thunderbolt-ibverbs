// SPDX-License-Identifier: GPL-2.0
/*
 * Apple-compatible TBnet identity policy.
 *
 * This file owns mode selection, stock Thunderbolt-net proxying, and shared
 * ThunderboltIP/ARP helpers. The minimal packet backend lives in
 * tbnet_minimal.c so the stock and owned-packet paths remain mechanically
 * separate.
 */

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/etherdevice.h>
#include <linux/errno.h>
#include <linux/if_arp.h>
#include <linux/if_ether.h>
#include <linux/inetdevice.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/notifier.h>
#include <linux/rcupdate.h>
#include <linux/rtnetlink.h>
#include <linux/skbuff.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/byteorder/generic.h>
#include <net/arp.h>
#include <net/net_namespace.h>

#include "tbv.h"

#define TBV_TBIP_LOGIN_PROTO_VERSION	1
#define TBV_TBIP_HDR_LENGTH_MASK	GENMASK(5, 0)
#define TBV_TBIP_HDR_SN_MASK		GENMASK(28, 27)
#define TBV_TBIP_HDR_SN_SHIFT		27
#define TBV_ETH_P_IP			0x0800
#define TBV_ETH_P_ARP			0x0806
#define TBV_ARPHRD_ETHER		1
#define TBV_ARPOP_REQUEST		1
#define TBV_ARPOP_REPLY		2

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

struct tbv_tbip_logout {
	struct tbv_tbip_header hdr;
};

struct tbv_tbip_status {
	struct tbv_tbip_header hdr;
	u32 status;
};

struct tbv_ethhdr {
	u8 h_dest[TBV_ETH_ALEN];
	u8 h_source[TBV_ETH_ALEN];
	__be16 h_proto;
} __packed;

struct tbv_arphdr {
	__be16 ar_hrd;
	__be16 ar_pro;
	u8 ar_hln;
	u8 ar_pln;
	__be16 ar_op;
} __packed;

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

int tbv_tbip_build_logout(void *buf, size_t size,
			  const struct tbv_tbip_control *ctrl)
{
	struct tbv_tbip_logout *msg = buf;

	if (!buf || !ctrl)
		return -EINVAL;
	if (size < sizeof(*msg))
		return -ENOSPC;

	memset(msg, 0, sizeof(*msg));
	tbv_tbip_fill_header(&msg->hdr, ctrl, TBV_TBIP_LOGOUT, sizeof(*msg));
	return sizeof(*msg);
}

int tbv_tbip_build_status(void *buf, size_t size,
			  const struct tbv_tbip_status_params *params)
{
	struct tbv_tbip_status *msg = buf;

	if (!buf || !params)
		return -EINVAL;
	if (size < sizeof(*msg))
		return -ENOSPC;

	memset(msg, 0, sizeof(*msg));
	tbv_tbip_fill_header(&msg->hdr, &params->ctrl, TBV_TBIP_STATUS,
			     sizeof(*msg));
	msg->status = params->status;
	return sizeof(*msg);
}

static int tbv_tbip_parse_header(const void *buf, size_t size,
				 enum tbv_tbip_type *type,
				 struct tbv_tbip_control *ctrl)
{
	const struct tbv_tbip_header *hdr = buf;

	if (!buf)
		return -EINVAL;
	if (size < sizeof(*hdr))
		return -EINVAL;
	if (!uuid_equal(&hdr->uuid, &tbv_tbip_uuid))
		return -EPROTO;

	switch (hdr->type) {
	case TBV_TBIP_LOGIN:
	case TBV_TBIP_LOGIN_RESPONSE:
	case TBV_TBIP_LOGOUT:
	case TBV_TBIP_STATUS:
		break;
	default:
		return -EPROTO;
	}

	if (type)
		*type = hdr->type;
	if (ctrl) {
		memset(ctrl, 0, sizeof(*ctrl));
		ctrl->route = ((u64)hdr->route_hi << 32) | hdr->route_lo;
		ctrl->route &= ~BIT_ULL(63);
		ctrl->sequence = (hdr->length_sn & TBV_TBIP_HDR_SN_MASK) >>
				 TBV_TBIP_HDR_SN_SHIFT;
		uuid_copy(&ctrl->initiator_uuid, &hdr->initiator_uuid);
		uuid_copy(&ctrl->target_uuid, &hdr->target_uuid);
		ctrl->command_id = hdr->command_id;
	}

	return 0;
}

int tbv_tbip_parse_type(const void *buf, size_t size,
			enum tbv_tbip_type *type,
			struct tbv_tbip_control *ctrl)
{
	if (!type)
		return -EINVAL;

	return tbv_tbip_parse_header(buf, size, type, ctrl);
}

int tbv_tbip_parse_login(const void *buf, size_t size,
			 struct tbv_tbip_login_params *params)
{
	const struct tbv_tbip_login *msg = buf;
	enum tbv_tbip_type type;
	int ret;

	if (!params)
		return -EINVAL;
	if (size < sizeof(*msg))
		return -EINVAL;

	ret = tbv_tbip_parse_header(buf, size, &type, &params->ctrl);
	if (ret)
		return ret;
	if (type != TBV_TBIP_LOGIN)
		return -EPROTO;
	if (msg->proto_version != TBV_TBIP_LOGIN_PROTO_VERSION)
		return -EPROTONOSUPPORT;

	params->transmit_path = msg->transmit_path;
	return 0;
}

int tbv_tbip_parse_login_response(const void *buf, size_t size,
				  struct tbv_tbip_login_response_result *result)
{
	const struct tbv_tbip_login_response *msg = buf;
	enum tbv_tbip_type type;
	int ret;

	if (!result)
		return -EINVAL;
	if (size < sizeof(*msg))
		return -EINVAL;

	memset(result, 0, sizeof(*result));
	ret = tbv_tbip_parse_header(buf, size, &type, &result->ctrl);
	if (ret)
		return ret;
	if (type != TBV_TBIP_LOGIN_RESPONSE)
		return -EPROTO;
	if (msg->receiver_mac_len > TBV_ETH_ALEN)
		return -EPROTO;

	result->status = msg->status;
	result->receiver_mac_len = msg->receiver_mac_len;
	memcpy(result->receiver_mac, msg->receiver_mac,
	       min_t(u32, msg->receiver_mac_len, TBV_ETH_ALEN));
	return 0;
}

static bool tbv_arp_is_ipv4_ethernet(const struct tbv_arphdr *arp)
{
	return arp->ar_hrd == cpu_to_be16(TBV_ARPHRD_ETHER) &&
	       arp->ar_pro == cpu_to_be16(TBV_ETH_P_IP) &&
	       arp->ar_hln == TBV_ETH_ALEN &&
	       arp->ar_pln == sizeof(__be32);
}

int tbv_tbnet_arp_reply_for_request(void *reply, size_t reply_size,
				    const void *request, size_t request_size,
				    const struct tbv_tbnet_arp_proxy *proxy)
{
	const struct tbv_arp_ipv4_payload *req_payload;
	struct tbv_arp_ipv4_payload *reply_payload;
	const struct tbv_ethhdr *req_eth = request;
	struct tbv_ethhdr *reply_eth = reply;
	const struct tbv_arphdr *req_arp;
	struct tbv_arphdr *reply_arp;
	size_t frame_len;

	if (!reply || !request || !proxy)
		return -EINVAL;

	frame_len = sizeof(struct tbv_ethhdr) + sizeof(struct tbv_arphdr) +
		    sizeof(struct tbv_arp_ipv4_payload);
	if (reply_size < frame_len)
		return -ENOSPC;
	if (request_size < frame_len)
		return -EINVAL;

	if (req_eth->h_proto != cpu_to_be16(TBV_ETH_P_ARP))
		return -ENOENT;

	req_arp = (const struct tbv_arphdr *)(req_eth + 1);
	if (!tbv_arp_is_ipv4_ethernet(req_arp))
		return -ENOENT;
	if (req_arp->ar_op != cpu_to_be16(TBV_ARPOP_REQUEST))
		return -ENOENT;

	req_payload = (const struct tbv_arp_ipv4_payload *)(req_arp + 1);
	if (req_payload->target_ip != proxy->ipv4)
		return -ENOENT;

	memset(reply, 0, frame_len);

	memcpy(reply_eth->h_dest, req_payload->sender_mac, TBV_ETH_ALEN);
	memcpy(reply_eth->h_source, proxy->mac, TBV_ETH_ALEN);
	reply_eth->h_proto = cpu_to_be16(TBV_ETH_P_ARP);

	reply_arp = (struct tbv_arphdr *)(reply_eth + 1);
	reply_arp->ar_hrd = cpu_to_be16(TBV_ARPHRD_ETHER);
	reply_arp->ar_pro = cpu_to_be16(TBV_ETH_P_IP);
	reply_arp->ar_hln = TBV_ETH_ALEN;
	reply_arp->ar_pln = sizeof(__be32);
	reply_arp->ar_op = cpu_to_be16(TBV_ARPOP_REPLY);

	reply_payload = (struct tbv_arp_ipv4_payload *)(reply_arp + 1);
	memcpy(reply_payload->sender_mac, proxy->mac, TBV_ETH_ALEN);
	reply_payload->sender_ip = proxy->ipv4;
	memcpy(reply_payload->target_mac, req_payload->sender_mac,
	       TBV_ETH_ALEN);
	reply_payload->target_ip = req_payload->sender_ip;

	return frame_len;
}

static bool tbv_identity_param_auto(const char *name)
{
	return !name || !*name || !strcmp(name, "auto");
}

static const char *tbv_identity_resolve_tbnet_name(const char *name)
{
	if (tbv_identity_param_auto(name))
		return "thunderbolt0";

	return name;
}

static const char *tbv_identity_resolve_gid_name(const char *name)
{
	const char *roce_name;

	if (!tbv_identity_param_auto(name))
		return name;

	roce_name = tbv_ibdev_roce_netdev_name();
	if (roce_name && *roce_name)
		return roce_name;

	return "";
}

static int tbv_identity_copy_netdev_name(char *dst, const char *src,
					 const char *param)
{
	if (!src)
		src = "";

	if (strnlen(src, IFNAMSIZ) >= IFNAMSIZ) {
		pr_err("%s netdev name is too long: %s\n", param, src);
		return -EINVAL;
	}

	strscpy(dst, src, IFNAMSIZ);
	return 0;
}

static __be32 tbv_netdev_first_ipv4_rtnl(struct net_device *dev)
{
	struct in_device *in_dev;
	struct in_ifaddr *ifa;
	__be32 addr = 0;

	in_dev = __in_dev_get_rtnl(dev);
	if (!in_dev)
		return 0;

	in_dev_for_each_ifa_rtnl(ifa, in_dev) {
		addr = ifa->ifa_local;
		break;
	}

	return addr;
}

static rx_handler_result_t
tbv_tbnet_identity_rx_handler(struct sk_buff **pskb);

static int tbv_tbnet_identity_send_arp_reply(struct net_device *dev,
					     const unsigned char *dst_mac,
					     __be32 dst_ip, __be32 src_ip)
{
	struct tbv_arp_ipv4_payload *payload;
	struct tbv_ethhdr *eth;
	struct tbv_arphdr *arp;
	struct sk_buff *reply;
	unsigned char *buf;
	size_t frame_len;
	int ret;

	frame_len = sizeof(*eth) + sizeof(*arp) + sizeof(*payload);
	reply = netdev_alloc_skb(dev, LL_RESERVED_SPACE(dev) + frame_len);
	if (!reply)
		return -ENOMEM;

	skb_reserve(reply, LL_RESERVED_SPACE(dev));
	buf = skb_put(reply, frame_len);
	memset(buf, 0, frame_len);

	eth = (struct tbv_ethhdr *)buf;
	memcpy(eth->h_dest, dst_mac, TBV_ETH_ALEN);
	memcpy(eth->h_source, dev->dev_addr, TBV_ETH_ALEN);
	eth->h_proto = htons(ETH_P_ARP);

	arp = (struct tbv_arphdr *)(eth + 1);
	arp->ar_hrd = htons(ARPHRD_ETHER);
	arp->ar_pro = htons(ETH_P_IP);
	arp->ar_hln = TBV_ETH_ALEN;
	arp->ar_pln = sizeof(__be32);
	arp->ar_op = htons(ARPOP_REPLY);

	payload = (struct tbv_arp_ipv4_payload *)(arp + 1);
	memcpy(payload->sender_mac, dev->dev_addr, TBV_ETH_ALEN);
	payload->sender_ip = src_ip;
	memcpy(payload->target_mac, dst_mac, TBV_ETH_ALEN);
	payload->target_ip = dst_ip;

	reply->dev = dev;
	reply->protocol = htons(ETH_P_ARP);
	reply->ip_summed = CHECKSUM_UNNECESSARY;
	skb_reset_mac_header(reply);

	ret = dev_queue_xmit(reply);
	if (ret == NET_XMIT_SUCCESS || ret == NET_XMIT_CN)
		return 0;

	return -EIO;
}

static void tbv_tbnet_identity_disarm_locked(struct tbv_tbnet_identity *identity)
{
	if (identity->rx_handler_registered) {
		netdev_rx_handler_unregister(identity->tbnet_dev);
		identity->rx_handler_registered = false;
	}

	if (identity->tbnet_dev) {
		dev_put(identity->tbnet_dev);
		identity->tbnet_dev = NULL;
	}

	if (identity->gid_dev) {
		dev_put(identity->gid_dev);
		identity->gid_dev = NULL;
	}

	identity->proxy_ipv4 = 0;
	identity->state = 0;
}

static unsigned long tbv_tbnet_identity_packet_state(struct net_device *dev)
{
	unsigned long state = 0;

	if (netif_carrier_ok(dev))
		state |= TBV_TBNET_ID_STATE_CARRIER;
	if (netif_running(dev))
		state |= TBV_TBNET_ID_STATE_PACKET_PATH_ACTIVE;

	return state;
}

static void tbv_tbnet_identity_put_gid_locked(struct tbv_tbnet_identity *identity)
{
	if (identity->gid_dev) {
		dev_put(identity->gid_dev);
		identity->gid_dev = NULL;
	}
	identity->proxy_ipv4 = 0;
	tbv_tbnet_minimal_clear_neighbors_locked(identity);
	identity->minimal_neighbor_seen = false;
}

static int
tbv_tbnet_identity_refresh_minimal_locked(struct tbv_tbnet_identity *identity)
{
	struct net_device *gid_dev;
	__be32 proxy_ipv4;

	if (!identity->gid_netdev_name[0]) {
		tbv_tbnet_identity_put_gid_locked(identity);
		tbv_tbnet_minimal_recompute_state_locked(identity);
		return 0;
	}

	gid_dev = dev_get_by_name(&init_net, identity->gid_netdev_name);
	if (!gid_dev) {
		tbv_tbnet_identity_put_gid_locked(identity);
		tbv_tbnet_minimal_recompute_state_locked(identity);
		return 0;
	}

	proxy_ipv4 = tbv_netdev_first_ipv4_rtnl(gid_dev);
	if (!proxy_ipv4) {
		dev_put(gid_dev);
		tbv_tbnet_identity_put_gid_locked(identity);
		tbv_tbnet_minimal_recompute_state_locked(identity);
		return 0;
	}

	if (identity->gid_dev == gid_dev && identity->proxy_ipv4 == proxy_ipv4) {
		dev_put(gid_dev);
		tbv_tbnet_minimal_recompute_state_locked(identity);
		return 0;
	}

	tbv_tbnet_identity_put_gid_locked(identity);
	identity->gid_dev = gid_dev;
	identity->proxy_ipv4 = proxy_ipv4;
	identity->minimal_neighbor_seen = false;
	tbv_tbnet_minimal_recompute_state_locked(identity);
	return 0;
}

static int
tbv_tbnet_identity_refresh_stock_proxy_locked(struct tbv_tbnet_identity *identity)
{
	struct net_device *tbnet_dev;
	struct net_device *gid_dev;
	unsigned long packet_state;
	__be32 proxy_ipv4;
	int ret;

	if (!identity->tbnet_netdev_name[0] || !identity->gid_netdev_name[0]) {
		tbv_tbnet_identity_disarm_locked(identity);
		return 0;
	}

	tbnet_dev = dev_get_by_name(&init_net, identity->tbnet_netdev_name);
	if (!tbnet_dev) {
		tbv_tbnet_identity_disarm_locked(identity);
		return 0;
	}

	packet_state = tbv_tbnet_identity_packet_state(tbnet_dev);

	gid_dev = dev_get_by_name(&init_net, identity->gid_netdev_name);
	if (!gid_dev) {
		tbv_tbnet_identity_disarm_locked(identity);
		identity->state = packet_state;
		dev_put(tbnet_dev);
		return 0;
	}

	proxy_ipv4 = tbv_netdev_first_ipv4_rtnl(gid_dev);
	if (!proxy_ipv4) {
		tbv_tbnet_identity_disarm_locked(identity);
		identity->state = packet_state;
		dev_put(gid_dev);
		dev_put(tbnet_dev);
		return 0;
	}

	if (identity->rx_handler_registered &&
	    identity->tbnet_dev == tbnet_dev &&
	    identity->gid_dev == gid_dev &&
	    identity->proxy_ipv4 == proxy_ipv4) {
		identity->state = packet_state;
		if (identity->state & TBV_TBNET_ID_STATE_PACKET_PATH_ACTIVE)
			identity->state |= TBV_TBNET_ID_STATE_NEIGHBOR_READY;
		dev_put(gid_dev);
		dev_put(tbnet_dev);
		return 0;
	}

	tbv_tbnet_identity_disarm_locked(identity);
	identity->tbnet_dev = tbnet_dev;
	identity->gid_dev = gid_dev;
	identity->proxy_ipv4 = proxy_ipv4;
	identity->state = packet_state;

	ret = netdev_rx_handler_register(tbnet_dev,
					 tbv_tbnet_identity_rx_handler,
					 identity);
	if (ret) {
		atomic64_inc(&identity->arp_errors);
		pr_warn("failed to arm TBnet ARP proxy on %s for %pI4 via %s: %d\n",
			identity->tbnet_netdev_name, &proxy_ipv4,
			identity->gid_netdev_name, ret);
		tbv_tbnet_identity_disarm_locked(identity);
		return 0;
	}

	identity->rx_handler_registered = true;
	if (identity->state & TBV_TBNET_ID_STATE_PACKET_PATH_ACTIVE)
		identity->state |= TBV_TBNET_ID_STATE_NEIGHBOR_READY;
	pr_info("armed TBnet ARP proxy on %s for %pI4 via %s\n",
		identity->tbnet_netdev_name, &proxy_ipv4,
		identity->gid_netdev_name);
	return 0;
}

static int tbv_tbnet_identity_refresh_locked(struct tbv_tbnet_identity *identity)
{
	switch (identity->mode) {
	case TBV_TBNET_ID_STOCK_PROXY:
		return tbv_tbnet_identity_refresh_stock_proxy_locked(identity);
	case TBV_TBNET_ID_MINIMAL_PACKET:
		return tbv_tbnet_identity_refresh_minimal_locked(identity);
	default:
		return 0;
	}
}

static rx_handler_result_t tbv_tbnet_identity_rx_handler(struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	struct tbv_tbnet_identity *identity;
	struct net_device *dev = skb->dev;
	unsigned char *arpptr;
	struct arphdr *arp;
	unsigned char *sha;
	__be32 proxy_ipv4;
	__be32 sip;
	__be32 tip;

	identity = rcu_dereference(dev->rx_handler_data);
	if (!identity)
		return RX_HANDLER_PASS;

	if (skb->protocol != htons(ETH_P_ARP))
		return RX_HANDLER_PASS;

	if (!pskb_may_pull(skb, arp_hdr_len(dev))) {
		atomic64_inc(&identity->arp_errors);
		return RX_HANDLER_PASS;
	}

	arp = arp_hdr(skb);
	if (arp->ar_hrd != htons(ARPHRD_ETHER) ||
	    arp->ar_pro != htons(ETH_P_IP) ||
	    arp->ar_hln != dev->addr_len ||
	    arp->ar_pln != sizeof(__be32) ||
	    arp->ar_op != htons(ARPOP_REQUEST)) {
		atomic64_inc(&identity->arp_ignored);
		return RX_HANDLER_PASS;
	}

	arpptr = (unsigned char *)(arp + 1);
	sha = arpptr;
	arpptr += dev->addr_len;
	memcpy(&sip, arpptr, sizeof(sip));
	arpptr += sizeof(sip);
	arpptr += dev->addr_len;
	memcpy(&tip, arpptr, sizeof(tip));

	atomic64_inc(&identity->arp_requests);

	proxy_ipv4 = READ_ONCE(identity->proxy_ipv4);
	if (!proxy_ipv4 || tip != proxy_ipv4) {
		atomic64_inc(&identity->arp_ignored);
		return RX_HANDLER_PASS;
	}

	if (tbv_tbnet_identity_send_arp_reply(dev, sha, sip, tip)) {
		atomic64_inc(&identity->arp_errors);
		return RX_HANDLER_PASS;
	}

	atomic64_inc(&identity->arp_replies);
	return RX_HANDLER_CONSUMED;
}

static int tbv_tbnet_identity_netdev_event(struct notifier_block *nb,
					   unsigned long event, void *ptr)
{
	struct tbv_tbnet_identity *identity =
		container_of(nb, struct tbv_tbnet_identity, netdev_nb);

	switch (event) {
	case NETDEV_REGISTER:
	case NETDEV_UNREGISTER:
	case NETDEV_UP:
	case NETDEV_DOWN:
	case NETDEV_CHANGE:
	case NETDEV_CHANGEADDR:
		break;
	default:
		return NOTIFY_DONE;
	}

	mutex_lock(&identity->lock);
	tbv_tbnet_identity_refresh_locked(identity);
	mutex_unlock(&identity->lock);
	return NOTIFY_DONE;
}

static int tbv_tbnet_identity_inetaddr_event(struct notifier_block *nb,
					     unsigned long event, void *ptr)
{
	struct tbv_tbnet_identity *identity =
		container_of(nb, struct tbv_tbnet_identity, inetaddr_nb);

	switch (event) {
	case NETDEV_UP:
	case NETDEV_DOWN:
		break;
	default:
		return NOTIFY_DONE;
	}

	mutex_lock(&identity->lock);
	tbv_tbnet_identity_refresh_locked(identity);
	mutex_unlock(&identity->lock);
	return NOTIFY_DONE;
}

static void tbv_tbnet_identity_unregister_notifiers(struct tbv_tbnet_identity *identity)
{
	if (identity->inetaddr_nb_registered) {
		unregister_inetaddr_notifier(&identity->inetaddr_nb);
		identity->inetaddr_nb_registered = false;
	}

	if (identity->netdev_nb_registered) {
		unregister_netdevice_notifier(&identity->netdev_nb);
		identity->netdev_nb_registered = false;
	}
}

static int tbv_tbnet_identity_register_notifiers(struct tbv_tbnet_identity *identity)
{
	int ret;

	identity->netdev_nb.notifier_call = tbv_tbnet_identity_netdev_event;
	ret = register_netdevice_notifier(&identity->netdev_nb);
	if (ret)
		return ret;
	identity->netdev_nb_registered = true;

	identity->inetaddr_nb.notifier_call = tbv_tbnet_identity_inetaddr_event;
	ret = register_inetaddr_notifier(&identity->inetaddr_nb);
	if (ret) {
		tbv_tbnet_identity_unregister_notifiers(identity);
		return ret;
	}
	identity->inetaddr_nb_registered = true;
	return 0;
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

	if (cfg->tbnet_identity == TBV_TBNET_ID_MINIMAL_PACKET &&
	    requested->tbnet == TBV_TBNET_ALLOW) {
		pr_err("tbnet_identity=minimal_packet owns the ThunderboltIP network service; use tbnet=block or tbnet=prefer_rdma\n");
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
			       const struct tbv_resolved_config *cfg,
			       const struct tbv_tbnet_identity_config *identity_cfg)
{
	const char *tbnet_name;
	const char *gid_name;
	int ret;

	memset(identity, 0, sizeof(*identity));
	identity->mode = cfg->tbnet_identity;
	identity->minimal_e2e = identity_cfg->minimal_e2e;
	identity->minimal_apple_only = identity_cfg->minimal_apple_only;
	mutex_init(&identity->lock);
	INIT_LIST_HEAD(&identity->minimal_sessions);

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
		tbnet_name = identity_cfg ?
			     identity_cfg->tbnet_netdev : "thunderbolt0";
		gid_name = identity_cfg ? identity_cfg->gid_netdev : "auto";
		tbnet_name = tbv_identity_resolve_tbnet_name(tbnet_name);
		gid_name = tbv_identity_resolve_gid_name(gid_name);
		ret = tbv_identity_copy_netdev_name(identity->tbnet_netdev_name,
						    tbnet_name,
						    "tbnet_identity_tbnet");
		if (ret) {
			mutex_destroy(&identity->lock);
			return ret;
		}
		ret = tbv_identity_copy_netdev_name(identity->gid_netdev_name,
						    gid_name,
						    "tbnet_identity_gid");
		if (ret) {
			mutex_destroy(&identity->lock);
			return ret;
		}

		ret = tbv_tbnet_identity_register_notifiers(identity);
		if (ret) {
			rtnl_lock();
			mutex_lock(&identity->lock);
			tbv_tbnet_identity_disarm_locked(identity);
			mutex_unlock(&identity->lock);
			rtnl_unlock();
			mutex_destroy(&identity->lock);
			return ret;
		}

		rtnl_lock();
		mutex_lock(&identity->lock);
		tbv_tbnet_identity_refresh_locked(identity);
		mutex_unlock(&identity->lock);
		rtnl_unlock();

		pr_info("TBnet identity uses stock ThunderboltIP with RDMA GID proxying tbnet=%s gid=%s\n",
			identity->tbnet_netdev_name,
			identity->gid_netdev_name[0] ?
			identity->gid_netdev_name : "<unset>");
		return 0;

	case TBV_TBNET_ID_MINIMAL_PACKET:
		gid_name = identity_cfg ? identity_cfg->gid_netdev : "auto";
		gid_name = tbv_identity_resolve_gid_name(gid_name);
		ret = tbv_identity_copy_netdev_name(identity->gid_netdev_name,
						    gid_name,
						    "tbnet_identity_gid");
		if (ret) {
			mutex_destroy(&identity->lock);
			return ret;
		}

		ret = tbv_tbnet_identity_register_notifiers(identity);
		if (ret) {
			mutex_destroy(&identity->lock);
			return ret;
		}

		rtnl_lock();
		mutex_lock(&identity->lock);
		tbv_tbnet_identity_refresh_locked(identity);
		mutex_unlock(&identity->lock);
		rtnl_unlock();

		pr_info("TBnet identity uses minimal ThunderboltIP packet responder gid=%s\n",
			identity->gid_netdev_name[0] ?
			identity->gid_netdev_name : "<unset>");
		return 0;

	case TBV_TBNET_ID_AUTO:
	case TBV_TBNET_ID_OFF:
	default:
		return 0;
	}
}

void tbv_tbnet_identity_stop(struct tbv_tbnet_identity *identity)
{
	tbv_tbnet_identity_unregister_notifiers(identity);
	tbv_tbnet_minimal_stop(identity);

	rtnl_lock();
	mutex_lock(&identity->lock);
	tbv_tbnet_identity_disarm_locked(identity);
	identity->state = 0;
	mutex_unlock(&identity->lock);
	rtnl_unlock();
	mutex_destroy(&identity->lock);
}
