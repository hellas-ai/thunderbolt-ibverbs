// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal ThunderboltIP packet identity backend.
 *
 * This is not a replacement for Linux's full thunderbolt_net driver. It owns
 * only the small packet surface the Apple-compatible RDMA backend needs:
 * ThunderboltIP LOGIN/LOGOUT, one framed packet path, and ARP replies for the
 * RDMA GID. Other Ethernet traffic is intentionally ignored.
 */

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/highmem.h>
#include <linux/if_ether.h>
#include <linux/inetdevice.h>
#include <linux/jhash.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/thunderbolt.h>
#include <linux/uuid.h>
#include <linux/workqueue.h>
#include <net/net_namespace.h>

#include "tbv.h"
#include "proto/tbnet.h"

#define TBV_TBNET_MIN_LOGIN_DELAY_MS	4500
#define TBV_TBNET_MIN_LOGIN_TIMEOUT_MS	500
#define TBV_TBNET_MIN_LOGIN_RETRIES	60
#define TBV_TBNET_MIN_LOGOUT_TIMEOUT_MS	1000
#define TBV_TBNET_MIN_RING_SIZE		256
#define TBV_TBNET_MIN_FRAME_SIZE	SZ_4K
#define TBV_TBNET_L0_PORT_NUM(route)	((route) & GENMASK(5, 0))
#define TBV_TBNET_PDF_FRAME_START	1
#define TBV_TBNET_PDF_FRAME_END		2

struct tbv_tbnet_frame_header {
	__le32 frame_size;
	__le16 frame_index;
	__le16 frame_id;
	__le32 frame_count;
} __packed;

struct tbv_tbnet_minimal_frame {
	struct ring_frame frame;
	struct list_head node;
	struct tbv_tbnet_minimal_session *session;
	struct page *page;
	void *buf;
	dma_addr_t dma;
	bool tx;
	bool arp_reply;
};

struct tbv_tbnet_minimal_session {
	struct list_head node;
	struct tbv_tbnet_identity *identity;
	const struct tb_service *svc;
	struct tb_xdomain *xd;
	struct tb_protocol_handler handler;
	struct mutex lock;
	spinlock_t tx_lock;
	struct delayed_work login_work;
	struct delayed_work connected_work;
	struct work_struct disconnect_work;
	struct work_struct rx_work;
	struct delayed_work tx_poll_work;
	struct tb_ring *tx_ring;
	struct tb_ring *rx_ring;
	struct tbv_tbnet_minimal_frame *tx_frames;
	struct tbv_tbnet_minimal_frame *rx_frames;
	struct list_head tx_free;
	atomic_t command_id;
	atomic_t frame_id;
	atomic_t tx_inflight;
	u8 mac[TBV_ETH_ALEN];
	int local_transmit_path;
	int remote_transmit_path;
	int login_retries;
	int path_retries;
	bool rings_started;
	bool path_enabled;
	bool login_sent;
	bool login_received;
	bool neighbor_seen;
	bool handler_registered;
	bool removing;
	atomic64_t login_rx;
	atomic64_t login_tx;
	atomic64_t logout_rx;
	atomic64_t logout_tx;
	atomic64_t status_rx;
	atomic64_t status_tx;
	atomic64_t packet_rx;
	atomic64_t packet_tx_posted;
	atomic64_t packet_tx;
	atomic64_t packet_tx_errors;
	atomic64_t path_errors;
	atomic64_t arp_requests;
	atomic64_t arp_replies;
	atomic64_t arp_ignored;
	atomic64_t arp_errors;
	atomic64_t rx_arp;
	atomic64_t rx_ipv4;
	atomic64_t rx_ipv6;
	atomic64_t rx_other;
	u32 last_rx_len;
	u32 last_tx_len;
	u16 last_rx_ethertype;
	u16 last_tx_ethertype;
};

/* Network property directory UUID: c66189ca-1cce-4195-bdb8-49592e5f5a4f */
static const uuid_t tbv_tbnet_dir_uuid =
	UUID_INIT(0xc66189ca, 0x1cce, 0x4195, 0xbd, 0xb8,
		  0x49, 0x59, 0x2e, 0x5f, 0x5a, 0x4f);

/* ThunderboltIP protocol UUID: 798f589e-3616-8a47-97c6-5664a920c8dd */
static const uuid_t tbv_tbnet_svc_uuid =
	UUID_INIT(0x798f589e, 0x3616, 0x8a47, 0x97, 0xc6,
		  0x56, 0x64, 0xa9, 0x20, 0xc8, 0xdd);

static struct tbv_tbnet_identity *tbv_tbnet_minimal_identity;

static void tbv_tbnet_minimal_login_work(struct work_struct *work);
static void tbv_tbnet_minimal_connected_work(struct work_struct *work);
static void tbv_tbnet_minimal_disconnect_work(struct work_struct *work);
static void tbv_tbnet_minimal_rx_work(struct work_struct *work);
static void tbv_tbnet_minimal_tx_poll_work(struct work_struct *work);
static void
tbv_tbnet_minimal_free_rings(struct tbv_tbnet_minimal_session *session);
static int
tbv_tbnet_minimal_send_logout_request(struct tbv_tbnet_minimal_session *session,
				      u8 sequence);
static __be32
tbv_tbnet_minimal_lookup_proxy_ipv4(struct tbv_tbnet_identity *identity);

static void
tbv_tbnet_minimal_queue_connected(struct tbv_tbnet_minimal_session *session,
				  unsigned long delay)
{
	queue_delayed_work(system_long_wq, &session->connected_work, delay);
}

static void
tbv_tbnet_minimal_fill_reply_ctrl(struct tbv_tbnet_minimal_session *session,
				  const struct tbv_tbip_control *request,
				  struct tbv_tbip_control *reply)
{
	memset(reply, 0, sizeof(*reply));
	reply->route = request->route;
	reply->sequence = request->sequence;
	uuid_copy(&reply->initiator_uuid, session->xd->local_uuid);
	uuid_copy(&reply->target_uuid, session->xd->remote_uuid);
	reply->command_id = request->command_id;
}

static bool
tbv_tbnet_minimal_ctrl_matches(const struct tbv_tbnet_minimal_session *session,
			       const struct tbv_tbip_control *ctrl)
{
	return ctrl->route == session->xd->route &&
	       uuid_equal(&ctrl->initiator_uuid, session->xd->remote_uuid) &&
	       uuid_equal(&ctrl->target_uuid, session->xd->local_uuid);
}

static bool
tbv_tbnet_minimal_response_matches(
	const struct tbv_tbnet_minimal_session *session,
	const struct tbv_tbip_control *request,
	const struct tbv_tbip_control *response)
{
	/*
	 * macOS echoes command_id in direct responses but advances sequence from
	 * its own control stream. command_id identifies the request; sequence is
	 * only meaningful for the sender's XDomain control flow.
	 */
	return tbv_tbnet_minimal_ctrl_matches(session, response) &&
	       response->command_id == request->command_id;
}

static bool
tbv_tbnet_minimal_status_matches(const struct tbv_tbnet_minimal_session *session,
				 const struct tbv_tbip_control *response)
{
	return tbv_tbnet_minimal_ctrl_matches(session, response);
}

static u32 tbv_tbnet_minimal_frame_len(const struct ring_frame *frame)
{
	return frame->size ? frame->size : (u32)TBV_TBNET_MIN_FRAME_SIZE;
}

static void tbv_tbnet_minimal_generate_mac(struct tbv_tbnet_minimal_session *s)
{
	u8 phy_port = tb_phy_port_from_link(TBV_TBNET_L0_PORT_NUM(s->xd->route));
	u32 hash;

	s->mac[0] = (phy_port << 4) | 0x02;
	hash = jhash2((const u32 *)s->xd->local_uuid, 4, 0);
	memcpy(s->mac + 1, &hash, sizeof(hash));
	hash = jhash2((const u32 *)s->xd->local_uuid, 4, hash);
	s->mac[5] = hash & 0xff;
}

static void
tbv_tbnet_minimal_mark_neighbor_seen(struct tbv_tbnet_minimal_session *session)
{
	bool ready;

	mutex_lock(&session->identity->lock);
	session->neighbor_seen = true;
	tbv_tbnet_minimal_recompute_state_locked(session->identity);
	ready = session->identity->state & TBV_TBNET_ID_STATE_NEIGHBOR_READY;
	mutex_unlock(&session->identity->lock);

	if (ready)
		tbv_services_tbnet_identity_ready(session->identity);
}

static void
tbv_tbnet_minimal_tx_complete(struct tb_ring *ring, struct ring_frame *frame,
			      bool canceled)
{
	struct tbv_tbnet_minimal_frame *f =
		container_of(frame, struct tbv_tbnet_minimal_frame, frame);
	struct tbv_tbnet_minimal_session *session = f->session;
	unsigned long flags;
	bool arp_reply = f->arp_reply;

	dma_sync_single_for_cpu(tb_ring_dma_device(ring), f->dma,
				TBV_TBNET_MIN_FRAME_SIZE, DMA_TO_DEVICE);
	f->frame.size = 0;
	f->frame.flags = 0;
	f->frame.sof = TBV_TBNET_PDF_FRAME_START;
	f->frame.eof = TBV_TBNET_PDF_FRAME_END;
	f->arp_reply = false;

	spin_lock_irqsave(&session->tx_lock, flags);
	list_add_tail(&f->node, &session->tx_free);
	spin_unlock_irqrestore(&session->tx_lock, flags);
	atomic_dec(&session->tx_inflight);

	if (!canceled) {
		atomic64_inc(&session->identity->minimal_packet_tx);
		atomic64_inc(&session->packet_tx);
		if (arp_reply)
			tbv_tbnet_minimal_mark_neighbor_seen(session);
	}
}

static void tbv_tbnet_minimal_rx_ready(void *data)
{
	struct tbv_tbnet_minimal_session *session = data;

	schedule_work(&session->rx_work);
}

static int
tbv_tbnet_minimal_alloc_frame_array(struct tbv_tbnet_minimal_session *session,
				    struct tbv_tbnet_minimal_frame **out,
				    struct tb_ring *ring, bool tx)
{
	struct device *dma_dev = tb_ring_dma_device(ring);
	enum dma_data_direction dir = tx ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
	struct tbv_tbnet_minimal_frame *frames;
	u32 i;

	if (!tbv_dma_device_ready(dma_dev)) {
		pr_warn_ratelimited("minimal TBnet %s ring DMA device is not ready for mapping\n",
				    tx ? "TX" : "RX");
		return -EPROBE_DEFER;
	}

	frames = kcalloc(TBV_TBNET_MIN_RING_SIZE, sizeof(*frames), GFP_KERNEL);
	if (!frames)
		return -ENOMEM;

	for (i = 0; i < TBV_TBNET_MIN_RING_SIZE; i++) {
		struct tbv_tbnet_minimal_frame *f = &frames[i];

		INIT_LIST_HEAD(&f->node);
		INIT_LIST_HEAD(&f->frame.list);
		f->session = session;
		f->tx = tx;
		f->page = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!f->page)
			goto err;
		f->buf = page_address(f->page);

		f->dma = dma_map_page(dma_dev, f->page, 0,
				      TBV_TBNET_MIN_FRAME_SIZE, dir);
		if (dma_mapping_error(dma_dev, f->dma)) {
			__free_page(f->page);
			f->page = NULL;
			f->buf = NULL;
			goto err;
		}

		f->frame.buffer_phy = f->dma;
		f->frame.sof = TBV_TBNET_PDF_FRAME_START;
		f->frame.eof = TBV_TBNET_PDF_FRAME_END;
		if (tx) {
			f->frame.callback = tbv_tbnet_minimal_tx_complete;
			list_add_tail(&f->node, &session->tx_free);
		}
	}

	*out = frames;
	return 0;

err:
	while (i--) {
		struct tbv_tbnet_minimal_frame *f = &frames[i];

		if (!f->page)
			continue;
		dma_unmap_page(dma_dev, f->dma, TBV_TBNET_MIN_FRAME_SIZE,
			       dir);
		__free_page(f->page);
		f->page = NULL;
		f->buf = NULL;
	}
	kfree(frames);
	return -ENOMEM;
}

static void
tbv_tbnet_minimal_free_frame_array(struct tbv_tbnet_minimal_frame *frames,
				   struct tb_ring *ring, bool tx)
{
	struct device *dma_dev;
	u32 i;

	if (!frames || !ring)
		return;

	dma_dev = tb_ring_dma_device(ring);
	if (!tbv_dma_device_ready(dma_dev)) {
		pr_warn_ratelimited("minimal TBnet %s ring DMA device is not ready for unmapping\n",
				    tx ? "TX" : "RX");
		dma_dev = NULL;
	}
	for (i = 0; i < TBV_TBNET_MIN_RING_SIZE; i++) {
		struct tbv_tbnet_minimal_frame *f = &frames[i];

		if (!f->page)
			continue;
		if (dma_dev)
			dma_unmap_page(dma_dev, f->dma, TBV_TBNET_MIN_FRAME_SIZE,
				       tx ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
		__free_page(f->page);
	}
	kfree(frames);
}

static int tbv_tbnet_minimal_prime_rx(struct tbv_tbnet_minimal_session *session)
{
	u32 i;
	int ret;

	for (i = 0; i < TBV_TBNET_MIN_RING_SIZE; i++) {
		struct tbv_tbnet_minimal_frame *f = &session->rx_frames[i];

		f->frame.size = 0;
		f->frame.flags = 0;
		ret = tb_ring_rx(session->rx_ring, &f->frame);
		if (ret)
			return ret;
	}

	return 0;
}

static int
tbv_tbnet_minimal_alloc_rings(struct tbv_tbnet_minimal_session *session)
{
	unsigned int flags = RING_FLAG_FRAME;
	u16 sof_mask = BIT(TBV_TBNET_PDF_FRAME_START);
	u16 eof_mask = BIT(TBV_TBNET_PDF_FRAME_END);
	int hopid;
	int ret;

	if (tbv_tbnet_minimal_e2e_enabled(session->identity->minimal_e2e,
					  session->svc->prtcstns))
		flags |= RING_FLAG_E2E;

	session->tx_ring = tb_ring_alloc_tx(session->xd->tb->nhi, -1,
					    TBV_TBNET_MIN_RING_SIZE, flags);
	if (!session->tx_ring)
		return -ENOMEM;

	hopid = tb_xdomain_alloc_out_hopid(session->xd, -1);
	if (hopid < 0) {
		ret = hopid;
		goto err_tx_ring;
	}
	session->local_transmit_path = hopid;

	session->rx_ring = tb_ring_alloc_rx(session->xd->tb->nhi, -1,
					    TBV_TBNET_MIN_RING_SIZE, flags,
					    session->tx_ring->hop,
					    sof_mask, eof_mask,
					    tbv_tbnet_minimal_rx_ready,
					    session);
	if (!session->rx_ring) {
		ret = -ENOMEM;
		goto err_out_hop;
	}

	ret = tbv_tbnet_minimal_alloc_frame_array(session, &session->tx_frames,
						 session->tx_ring, true);
	if (ret)
		goto err_rx_ring;

	ret = tbv_tbnet_minimal_alloc_frame_array(session, &session->rx_frames,
						 session->rx_ring, false);
	if (ret)
		goto err_tx_frames;

	return 0;

err_tx_frames:
	tbv_tbnet_minimal_free_frame_array(session->tx_frames,
					   session->tx_ring, true);
	session->tx_frames = NULL;
err_rx_ring:
	tb_ring_free(session->rx_ring);
	session->rx_ring = NULL;
err_out_hop:
	tb_xdomain_release_out_hopid(session->xd, session->local_transmit_path);
	session->local_transmit_path = 0;
err_tx_ring:
	tb_ring_free(session->tx_ring);
	session->tx_ring = NULL;
	return ret;
}

void tbv_tbnet_minimal_recompute_state_locked(struct tbv_tbnet_identity *identity)
{
	struct tbv_tbnet_minimal_session *session;
	unsigned long state = 0;
	bool neighbor_seen = false;

	if (identity->mode != TBV_TBNET_ID_MINIMAL_PACKET)
		return;

	list_for_each_entry(session, &identity->minimal_sessions, node) {
		if (!READ_ONCE(session->path_enabled)) {
			session->neighbor_seen = false;
			continue;
		}
		state |= TBV_TBNET_ID_STATE_CARRIER;
		state |= TBV_TBNET_ID_STATE_PACKET_PATH_ACTIVE;
		if (identity->proxy_ipv4 && READ_ONCE(session->neighbor_seen))
			neighbor_seen = true;
	}

	if (!identity->proxy_ipv4)
		list_for_each_entry(session, &identity->minimal_sessions, node)
			session->neighbor_seen = false;

	if (!(state & TBV_TBNET_ID_STATE_PACKET_PATH_ACTIVE) ||
	    !identity->proxy_ipv4)
		identity->minimal_neighbor_seen = false;
	else
		identity->minimal_neighbor_seen = neighbor_seen;

	if (identity->minimal_neighbor_seen)
		state |= TBV_TBNET_ID_STATE_NEIGHBOR_READY;

	identity->state = state;
}

bool tbv_tbnet_minimal_neighbor_ready(struct tbv_tbnet_identity *identity,
				      const uuid_t *remote_uuid)
{
	struct tbv_tbnet_minimal_session *session;
	bool ready = false;

	if (identity->mode != TBV_TBNET_ID_MINIMAL_PACKET)
		return false;

	mutex_lock(&identity->lock);
	if (!identity->proxy_ipv4)
		goto out;

	list_for_each_entry(session, &identity->minimal_sessions, node) {
		if (!READ_ONCE(session->path_enabled) ||
		    !READ_ONCE(session->neighbor_seen))
			continue;
		if (remote_uuid &&
		    !uuid_equal(session->xd->remote_uuid, remote_uuid))
			continue;
		ready = true;
		break;
	}

out:
	mutex_unlock(&identity->lock);
	return ready;
}

bool tbv_tbnet_minimal_path_ready(struct tbv_tbnet_identity *identity,
				  const uuid_t *remote_uuid)
{
	struct tbv_tbnet_minimal_session *session;
	bool ready = false;

	if (identity->mode != TBV_TBNET_ID_MINIMAL_PACKET)
		return false;

	mutex_lock(&identity->lock);
	list_for_each_entry(session, &identity->minimal_sessions, node) {
		if (remote_uuid &&
		    !uuid_equal(session->xd->remote_uuid, remote_uuid))
			continue;
		if (!READ_ONCE(session->path_enabled))
			continue;
		ready = true;
		break;
	}
	mutex_unlock(&identity->lock);
	return ready;
}

void tbv_tbnet_minimal_clear_neighbors_locked(struct tbv_tbnet_identity *identity)
{
	struct tbv_tbnet_minimal_session *session;

	if (identity->mode != TBV_TBNET_ID_MINIMAL_PACKET)
		return;

	list_for_each_entry(session, &identity->minimal_sessions, node)
		session->neighbor_seen = false;
	identity->minimal_neighbor_seen = false;
}

static void tbv_tbnet_minimal_recompute_state(struct tbv_tbnet_identity *identity)
{
	bool ready;

	mutex_lock(&identity->lock);
	tbv_tbnet_minimal_recompute_state_locked(identity);
	ready = identity->state & (TBV_TBNET_ID_STATE_PACKET_PATH_ACTIVE |
				   TBV_TBNET_ID_STATE_NEIGHBOR_READY);
	mutex_unlock(&identity->lock);

	if (ready)
		tbv_services_tbnet_identity_ready(identity);
}

void tbv_tbnet_minimal_debugfs_show(struct seq_file *s,
				    struct tbv_tbnet_identity *identity)
{
	struct tbv_tbnet_minimal_session *session;
	u32 idx = 0;

	if (identity->mode != TBV_TBNET_ID_MINIMAL_PACKET)
		return;

	list_for_each_entry(session, &identity->minimal_sessions, node) {
		seq_printf(s,
			   "tbnet_minimal_session%u: svc=%d route=0x%llx prtcstns=0x%x mac=%pM local_uuid=%pUb remote_uuid=%pUb\n",
			   idx, session->svc->id, session->xd->route,
			   session->svc->prtcstns, session->mac,
			   session->xd->local_uuid, session->xd->remote_uuid);
		seq_printf(s,
			   "tbnet_minimal_session%u_state: rings_started=%u path_enabled=%u login_sent=%u login_received=%u neighbor_seen=%u removing=%u tx_inflight=%d\n",
			   idx, READ_ONCE(session->rings_started),
			   READ_ONCE(session->path_enabled),
			   READ_ONCE(session->login_sent),
			   READ_ONCE(session->login_received),
			   READ_ONCE(session->neighbor_seen),
			   READ_ONCE(session->removing),
			   atomic_read(&session->tx_inflight));
		seq_printf(s,
			   "tbnet_minimal_session%u_paths: local_tx_path=%d remote_tx_path=%d tx_hop=%d rx_hop=%d\n",
			   idx, READ_ONCE(session->local_transmit_path),
			   READ_ONCE(session->remote_transmit_path),
			   session->tx_ring ? session->tx_ring->hop : -1,
			   session->rx_ring ? session->rx_ring->hop : -1);
		seq_printf(s,
			   "tbnet_minimal_session%u_login: rx=%lld tx=%lld retries=%d path_retries=%d\n",
			   idx, atomic64_read(&session->login_rx),
			   atomic64_read(&session->login_tx),
			   READ_ONCE(session->login_retries),
			   READ_ONCE(session->path_retries));
		seq_printf(s,
			   "tbnet_minimal_session%u_control: logout_rx=%lld logout_tx=%lld status_rx=%lld status_tx=%lld\n",
			   idx, atomic64_read(&session->logout_rx),
			   atomic64_read(&session->logout_tx),
			   atomic64_read(&session->status_rx),
			   atomic64_read(&session->status_tx));
		seq_printf(s,
			   "tbnet_minimal_session%u_packets: rx=%lld tx_posted=%lld tx_completed=%lld tx_errors=%lld path_errors=%lld\n",
			   idx, atomic64_read(&session->packet_rx),
			   atomic64_read(&session->packet_tx_posted),
			   atomic64_read(&session->packet_tx),
			   atomic64_read(&session->packet_tx_errors),
			   atomic64_read(&session->path_errors));
		seq_printf(s,
			   "tbnet_minimal_session%u_arp: requests=%lld replies=%lld ignored=%lld errors=%lld\n",
			   idx, atomic64_read(&session->arp_requests),
			   atomic64_read(&session->arp_replies),
			   atomic64_read(&session->arp_ignored),
			   atomic64_read(&session->arp_errors));
		seq_printf(s,
			   "tbnet_minimal_session%u_rx_ethertypes: arp=%lld ipv4=%lld ipv6=%lld other=%lld last=0x%04x last_len=%u\n",
			   idx, atomic64_read(&session->rx_arp),
			   atomic64_read(&session->rx_ipv4),
			   atomic64_read(&session->rx_ipv6),
			   atomic64_read(&session->rx_other),
			   READ_ONCE(session->last_rx_ethertype),
			   READ_ONCE(session->last_rx_len));
		seq_printf(s,
			   "tbnet_minimal_session%u_tx_last: ethertype=0x%04x len=%u\n",
			   idx, READ_ONCE(session->last_tx_ethertype),
			   READ_ONCE(session->last_tx_len));
		idx++;
	}
}

static int
tbv_tbnet_minimal_send_frame(struct tbv_tbnet_minimal_session *session,
			     const void *payload, u32 payload_len,
			     bool arp_reply)
{
	struct tbv_tbnet_minimal_frame *f;
	struct tbv_tbnet_frame_header *hdr;
	unsigned long flags;
	u16 frame_id;
	int ret;

	if (!payload_len ||
	    payload_len > TBV_TBNET_MIN_FRAME_SIZE - sizeof(*hdr))
		return -EINVAL;

	mutex_lock(&session->lock);
	if (!session->path_enabled || session->removing) {
		mutex_unlock(&session->lock);
		return -ENETDOWN;
	}
	mutex_unlock(&session->lock);

	spin_lock_irqsave(&session->tx_lock, flags);
	if (list_empty(&session->tx_free)) {
		spin_unlock_irqrestore(&session->tx_lock, flags);
		return -ENOMEM;
	}
	f = list_first_entry(&session->tx_free, struct tbv_tbnet_minimal_frame,
			     node);
	list_del_init(&f->node);
	atomic_inc(&session->tx_inflight);
	spin_unlock_irqrestore(&session->tx_lock, flags);

	hdr = f->buf;
	frame_id = (u16)atomic_inc_return(&session->frame_id);
	dma_sync_single_for_cpu(tb_ring_dma_device(session->tx_ring),
				f->dma, TBV_TBNET_MIN_FRAME_SIZE,
				DMA_TO_DEVICE);
	memset(f->buf, 0, TBV_TBNET_MIN_FRAME_SIZE);
	hdr->frame_size = cpu_to_le32(payload_len);
	hdr->frame_index = cpu_to_le16(0);
	hdr->frame_id = cpu_to_le16(frame_id);
	hdr->frame_count = cpu_to_le32(1);
	memcpy(hdr + 1, payload, payload_len);

	f->frame.size = sizeof(*hdr) + payload_len;
	f->frame.flags = 0;
	f->frame.sof = TBV_TBNET_PDF_FRAME_START;
	f->frame.eof = TBV_TBNET_PDF_FRAME_END;
	f->arp_reply = arp_reply;
	dma_sync_single_for_device(tb_ring_dma_device(session->tx_ring),
				   f->dma, TBV_TBNET_MIN_FRAME_SIZE,
				   DMA_TO_DEVICE);

	ret = tb_ring_tx(session->tx_ring, &f->frame);
	if (!ret) {
		atomic64_inc(&session->identity->minimal_packet_tx_posted);
		atomic64_inc(&session->packet_tx_posted);
		WRITE_ONCE(session->last_tx_len, payload_len);
		schedule_delayed_work(&session->tx_poll_work,
				      msecs_to_jiffies(1));
		return 0;
	}

	atomic64_inc(&session->identity->minimal_packet_tx_errors);
	atomic64_inc(&session->packet_tx_errors);
	f->arp_reply = false;
	atomic_dec(&session->tx_inflight);
	spin_lock_irqsave(&session->tx_lock, flags);
	list_add_tail(&f->node, &session->tx_free);
	spin_unlock_irqrestore(&session->tx_lock, flags);
	return ret;
}

static int
tbv_tbnet_minimal_send_arp_reply(struct tbv_tbnet_minimal_session *session,
				 const void *request, u32 request_len)
{
	struct tbv_tbnet_arp_proxy proxy;
	u8 reply[ETH_ZLEN];
	__be32 proxy_ipv4;
	u32 reply_len;
	int ret;

	proxy_ipv4 = READ_ONCE(session->identity->proxy_ipv4);
	if (!proxy_ipv4)
		proxy_ipv4 =
			tbv_tbnet_minimal_lookup_proxy_ipv4(session->identity);

	memset(&proxy, 0, sizeof(proxy));
	proxy.ipv4 = proxy_ipv4;
	memcpy(proxy.mac, session->mac, sizeof(proxy.mac));

	memset(reply, 0, sizeof(reply));
	ret = tbv_tbnet_arp_reply_for_request(reply, sizeof(reply),
					      request, request_len, &proxy);
	if (ret <= 0)
		return ret;

	atomic64_inc(&session->identity->arp_requests);
	atomic64_inc(&session->arp_requests);
	reply_len = max_t(u32, ret, ETH_ZLEN);
	ret = tbv_tbnet_minimal_send_frame(session, reply, reply_len, true);
	if (ret)
		return ret;

	atomic64_inc(&session->identity->arp_replies);
	atomic64_inc(&session->arp_replies);
	WRITE_ONCE(session->last_tx_ethertype, ETH_P_ARP);
	return 0;
}

static __be32
tbv_tbnet_minimal_lookup_proxy_ipv4(struct tbv_tbnet_identity *identity)
{
	struct net_device *dev;
	struct in_device *in_dev;
	struct in_ifaddr *ifa;
	__be32 addr = 0;

	if (!identity->gid_netdev_name[0])
		return 0;

	rtnl_lock();
	dev = dev_get_by_name(&init_net, identity->gid_netdev_name);
	if (!dev)
		goto out_unlock;

	in_dev = __in_dev_get_rtnl(dev);
	if (in_dev) {
		in_dev_for_each_ifa_rtnl(ifa, in_dev) {
			addr = ifa->ifa_local;
			break;
		}
	}
	dev_put(dev);

out_unlock:
	rtnl_unlock();

	if (addr)
		WRITE_ONCE(identity->proxy_ipv4, addr);
	return addr;
}

static void
tbv_tbnet_minimal_count_rx_payload(struct tbv_tbnet_minimal_session *session,
				   const void *payload, u32 payload_len)
{
	const struct ethhdr *eth = payload;
	u16 ethertype = 0;

	WRITE_ONCE(session->last_rx_len, payload_len);
	if (payload_len < sizeof(*eth)) {
		atomic64_inc(&session->rx_other);
		WRITE_ONCE(session->last_rx_ethertype, 0);
		return;
	}

	ethertype = be16_to_cpu(eth->h_proto);
	WRITE_ONCE(session->last_rx_ethertype, ethertype);
	switch (ethertype) {
	case ETH_P_ARP:
		atomic64_inc(&session->rx_arp);
		break;
	case ETH_P_IP:
		atomic64_inc(&session->rx_ipv4);
		break;
	case ETH_P_IPV6:
		atomic64_inc(&session->rx_ipv6);
		break;
	default:
		atomic64_inc(&session->rx_other);
		break;
	}
}

static void
tbv_tbnet_minimal_handle_rx_frame(struct tbv_tbnet_minimal_session *session,
				  struct tbv_tbnet_minimal_frame *f,
				  u32 frame_len)
{
	const struct tbv_tbnet_frame_header *hdr = f->buf;
	u32 payload_len;
	u32 frame_count;
	u16 frame_index;
	int ret;

	if (frame_len < sizeof(*hdr)) {
		atomic64_inc(&session->identity->minimal_path_errors);
		atomic64_inc(&session->path_errors);
		return;
	}

	payload_len = le32_to_cpu(hdr->frame_size);
	frame_count = le32_to_cpu(hdr->frame_count);
	frame_index = le16_to_cpu(hdr->frame_index);
	if (!payload_len || payload_len > frame_len - sizeof(*hdr) ||
	    frame_count != 1 || frame_index != 0) {
		atomic64_inc(&session->identity->minimal_path_errors);
		atomic64_inc(&session->path_errors);
		return;
	}

	atomic64_inc(&session->identity->minimal_packet_rx);
	atomic64_inc(&session->packet_rx);
	tbv_tbnet_minimal_count_rx_payload(session, hdr + 1, payload_len);
	tbv_tbnet_minimal_mark_neighbor_seen(session);
	ret = tbv_tbnet_minimal_send_arp_reply(session, hdr + 1, payload_len);
	if (ret == -ENOENT) {
		atomic64_inc(&session->identity->arp_ignored);
		atomic64_inc(&session->arp_ignored);
	} else if (ret) {
		atomic64_inc(&session->identity->arp_errors);
		atomic64_inc(&session->arp_errors);
	}
}

static void tbv_tbnet_minimal_rx_work(struct work_struct *work)
{
	struct tbv_tbnet_minimal_session *session =
		container_of(work, struct tbv_tbnet_minimal_session, rx_work);
	struct device *dma_dev;
	struct ring_frame *frame;

	if (!session->rx_ring)
		return;

	dma_dev = tb_ring_dma_device(session->rx_ring);
	while ((frame = tb_ring_poll(session->rx_ring))) {
		struct tbv_tbnet_minimal_frame *f =
			container_of(frame, struct tbv_tbnet_minimal_frame,
				     frame);

		dma_sync_single_for_cpu(dma_dev, f->dma,
					TBV_TBNET_MIN_FRAME_SIZE,
					DMA_FROM_DEVICE);
		tbv_tbnet_minimal_handle_rx_frame(session, f,
				tbv_tbnet_minimal_frame_len(frame));
		dma_sync_single_for_device(dma_dev, f->dma,
					   TBV_TBNET_MIN_FRAME_SIZE,
					   DMA_FROM_DEVICE);
		frame->size = 0;
		frame->flags = 0;
		if (tb_ring_rx(session->rx_ring, frame)) {
			atomic64_inc(&session->identity->minimal_path_errors);
			atomic64_inc(&session->path_errors);
			break;
		}
	}

	tb_ring_poll_complete(session->rx_ring);
}

static void tbv_tbnet_minimal_tx_poll_work(struct work_struct *work)
{
	struct tbv_tbnet_minimal_session *session =
		container_of(to_delayed_work(work),
			     struct tbv_tbnet_minimal_session, tx_poll_work);
	struct ring_frame *frame;
	u32 completed = 0;

	if (!session->tx_ring)
		return;

	while ((frame = tb_ring_poll(session->tx_ring))) {
		if (frame->callback)
			frame->callback(session->tx_ring, frame, false);
		completed++;
	}

	if (atomic_read(&session->tx_inflight) > 0 || completed)
		schedule_delayed_work(&session->tx_poll_work,
				      msecs_to_jiffies(1));
}

static int
tbv_tbnet_minimal_enable_paths(struct tbv_tbnet_minimal_session *s,
			       int remote_transmit_path)
{
	/*
	 * Keep this wired exactly like upstream drivers/net/thunderbolt/main.c.
	 * ThunderboltIP's path setup predates the cleaner native backend in this
	 * module, and macOS expects the stock TBnet ring/path pairing here.
	 */
	return tb_xdomain_enable_paths(s->xd, s->local_transmit_path,
				       s->tx_ring->hop,
				       remote_transmit_path,
				       s->rx_ring->hop);
}

static void tbv_tbnet_minimal_disable_paths(struct tbv_tbnet_minimal_session *s)
{
	tb_xdomain_disable_paths(s->xd, s->local_transmit_path,
				 s->tx_ring->hop,
				 s->remote_transmit_path,
				 s->rx_ring->hop);
}

static void tbv_tbnet_minimal_teardown_path(struct tbv_tbnet_minimal_session *s)
{
	mutex_lock(&s->lock);
	if (s->rings_started) {
		tb_ring_stop(s->rx_ring);
		tb_ring_stop(s->tx_ring);
		s->rings_started = false;
	}
	if (s->path_enabled) {
		tbv_tbnet_minimal_disable_paths(s);
		tb_xdomain_release_in_hopid(s->xd, s->remote_transmit_path);
		s->path_enabled = false;
		s->remote_transmit_path = 0;
	}
	s->login_sent = false;
	s->login_received = false;
	s->login_retries = 0;
	s->path_retries = 0;
	mutex_unlock(&s->lock);

	tbv_tbnet_minimal_recompute_state(s->identity);
}

static void
tbv_tbnet_minimal_retry_connected(struct tbv_tbnet_minimal_session *session,
				  const char *stage, int ret)
{
	bool retry = false;
	int attempt = 0;

	mutex_lock(&session->lock);
	if (!session->removing && !session->path_enabled &&
	    session->login_sent && session->login_received) {
		if (session->path_retries++ < TBV_TBNET_MIN_LOGIN_RETRIES) {
			attempt = session->path_retries;
			retry = true;
		} else {
			pr_info("minimal TBnet packet path setup timed out route=0x%llx stage=%s ret=%d\n",
				session->xd->route, stage, ret);
		}
	}
	mutex_unlock(&session->lock);

	if (!retry)
		return;

	pr_debug("minimal TBnet retrying packet path route=0x%llx stage=%s ret=%d attempt=%d\n",
		 session->xd->route, stage, ret, attempt);
	tbv_tbnet_minimal_queue_connected(
		session, msecs_to_jiffies(TBV_TBNET_MIN_LOGIN_DELAY_MS));
}

static void tbv_tbnet_minimal_connected_work(struct work_struct *work)
{
	struct tbv_tbnet_minimal_session *session =
		container_of(to_delayed_work(work),
			     struct tbv_tbnet_minimal_session, connected_work);
	int remote_transmit_path;
	bool connected;
	int ret;

	mutex_lock(&session->lock);
	if (session->path_enabled || session->removing) {
		mutex_unlock(&session->lock);
		return;
	}

	connected = session->login_sent && session->login_received;
	remote_transmit_path = session->remote_transmit_path;
	mutex_unlock(&session->lock);
	if (!connected)
		return;

	ret = tb_xdomain_alloc_in_hopid(session->xd, remote_transmit_path);
	if (ret != remote_transmit_path) {
		atomic64_inc(&session->identity->minimal_path_errors);
		atomic64_inc(&session->path_errors);
		if (ret > 0)
			tb_xdomain_release_in_hopid(session->xd, ret);
		pr_warn("minimal TBnet failed to allocate Rx HopID %d: %d\n",
			remote_transmit_path, ret);
		tbv_tbnet_minimal_retry_connected(session, "alloc-hopid", ret);
		return;
	}

	tb_ring_start(session->tx_ring);
	tb_ring_start(session->rx_ring);
	ret = tbv_tbnet_minimal_prime_rx(session);
	if (ret) {
		atomic64_inc(&session->identity->minimal_path_errors);
		atomic64_inc(&session->path_errors);
		goto err_stop;
	}

	ret = tbv_tbnet_minimal_enable_paths(session, remote_transmit_path);
	if (ret) {
		atomic64_inc(&session->identity->minimal_path_errors);
		atomic64_inc(&session->path_errors);
		pr_warn("minimal TBnet failed to enable packet path: %d\n",
			ret);
		goto err_stop;
	}

	mutex_lock(&session->lock);
	session->rings_started = true;
	session->path_enabled = true;
	session->path_retries = 0;
	mutex_unlock(&session->lock);
	tbv_tbnet_minimal_recompute_state(session->identity);
	pr_info("minimal TBnet packet path active route=0x%llx tx_path=%d rx_path=%d tx_hop=%d rx_hop=%d\n",
		session->xd->route, session->local_transmit_path,
		remote_transmit_path, session->tx_ring->hop,
		session->rx_ring->hop);
	return;

err_stop:
	tb_ring_stop(session->rx_ring);
	tb_ring_stop(session->tx_ring);
	tb_xdomain_release_in_hopid(session->xd, remote_transmit_path);
	tbv_tbnet_minimal_retry_connected(session, "enable-path", ret);
}

static void tbv_tbnet_minimal_disconnect_work(struct work_struct *work)
{
	struct tbv_tbnet_minimal_session *session =
		container_of(work, struct tbv_tbnet_minimal_session,
			     disconnect_work);

	tbv_tbnet_minimal_teardown_path(session);
}

static void tbv_tbnet_minimal_login_work(struct work_struct *work)
{
	struct tbv_tbnet_minimal_session *session =
		container_of(to_delayed_work(work),
			     struct tbv_tbnet_minimal_session, login_work);
	struct tbv_tbip_login_response_result response;
	struct tbv_tbip_login_params params;
	u8 request[128];
	u8 reply[128];
	bool need_reset;
	bool retry_later = false;
	u8 sequence;
	int ret;
	int len;

	mutex_lock(&session->lock);
	if (session->path_enabled || session->removing) {
		mutex_unlock(&session->lock);
		return;
	}
	sequence = session->login_retries % 4;
	need_reset = !session->login_received;
	mutex_unlock(&session->lock);

	if (need_reset) {
		/*
		 * A reload can leave macOS with a half-open ThunderboltIP
		 * connection from the previous module instance. Clear that
		 * protocol state before each LOGIN attempt until the peer's
		 * reciprocal LOGIN arrives; one reset is not sufficient for all
		 * observed macOS wakeup states.
		 */
		ret = tbv_tbnet_minimal_send_logout_request(session, sequence);
		if (ret && ret != -ETIMEDOUT && ret != -ENODEV)
			pr_debug("minimal TBnet logout reset route=0x%llx ret=%d\n",
				 session->xd->route, ret);
	}

	mutex_lock(&session->lock);
	if (session->path_enabled || session->removing) {
		mutex_unlock(&session->lock);
		return;
	}
	memset(&params, 0, sizeof(params));
	params.ctrl.route = session->xd->route;
	params.ctrl.sequence = sequence;
	uuid_copy(&params.ctrl.initiator_uuid, session->xd->local_uuid);
	uuid_copy(&params.ctrl.target_uuid, session->xd->remote_uuid);
	params.ctrl.command_id = atomic_inc_return(&session->command_id);
	params.transmit_path = session->local_transmit_path;
	mutex_unlock(&session->lock);

	len = tbv_tbip_build_login(request, sizeof(request), &params);
	if (len < 0)
		return;

	memset(reply, 0, sizeof(reply));
	atomic64_inc(&session->identity->minimal_login_tx);
	atomic64_inc(&session->login_tx);
	ret = tb_xdomain_request(session->xd, request, len,
				 TB_CFG_PKG_XDOMAIN_RESP, reply, sizeof(reply),
				 TB_CFG_PKG_XDOMAIN_RESP,
				 TBV_TBNET_MIN_LOGIN_TIMEOUT_MS);
	if (!ret)
		ret = tbv_tbip_parse_login_response(reply, sizeof(reply),
						    &response);
	if (!ret &&
	    (!tbv_tbnet_minimal_response_matches(session, &params.ctrl,
						 &response.ctrl) ||
	     response.status)) {
		ret = -EPROTO;
	}
	if (ret) {
		mutex_lock(&session->lock);
		if (!session->removing &&
		    session->login_retries++ < TBV_TBNET_MIN_LOGIN_RETRIES)
			queue_delayed_work(system_long_wq,
					   &session->login_work,
					   msecs_to_jiffies(TBV_TBNET_MIN_LOGIN_DELAY_MS));
		else if (!session->removing)
			pr_info("minimal TBnet login timed out route=0x%llx\n",
				session->xd->route);
		mutex_unlock(&session->lock);
		return;
	}

	mutex_lock(&session->lock);
	session->login_sent = true;
	if (session->login_received) {
		session->login_retries = 0;
	} else if (!session->removing &&
		   session->login_retries++ < TBV_TBNET_MIN_LOGIN_RETRIES) {
		retry_later = true;
	} else if (!session->removing) {
		pr_info("minimal TBnet peer login timed out route=0x%llx\n",
			session->xd->route);
	}
	mutex_unlock(&session->lock);
	tbv_services_tbnet_identity_ready(session->identity);
	tbv_tbnet_minimal_queue_connected(session, 0);
	if (retry_later)
		queue_delayed_work(system_long_wq, &session->login_work,
				   msecs_to_jiffies(TBV_TBNET_MIN_LOGIN_DELAY_MS));
}

static int tbv_tbnet_minimal_send_login_response(
	struct tbv_tbnet_minimal_session *session,
	const struct tbv_tbip_login_params *request)
{
	struct tbv_tbip_login_response_params params;
	u8 reply[128];
	int len;

	memset(&params, 0, sizeof(params));
	tbv_tbnet_minimal_fill_reply_ctrl(session, &request->ctrl,
					  &params.ctrl);
	memcpy(params.receiver_mac, session->mac, sizeof(params.receiver_mac));
	len = tbv_tbip_build_login_response(reply, sizeof(reply), &params);
	if (len < 0)
		return len;

	return tb_xdomain_response(session->xd, reply, len,
				   TB_CFG_PKG_XDOMAIN_RESP);
}

static int tbv_tbnet_minimal_send_status(
	struct tbv_tbnet_minimal_session *session,
	const struct tbv_tbip_control *request)
{
	struct tbv_tbip_status_params params;
	u8 reply[128];
	int len;

	memset(&params, 0, sizeof(params));
	tbv_tbnet_minimal_fill_reply_ctrl(session, request, &params.ctrl);
	params.ctrl.command_id = atomic_inc_return(&session->command_id);
	len = tbv_tbip_build_status(reply, sizeof(reply), &params);
	if (len < 0)
		return len;

	return tb_xdomain_response(session->xd, reply, len,
				   TB_CFG_PKG_XDOMAIN_RESP);
}

static int
tbv_tbnet_minimal_send_logout_request(struct tbv_tbnet_minimal_session *session,
				      u8 sequence)
{
	struct tbv_tbip_status_result status;
	struct tbv_tbip_control ctrl;
	u8 request[128];
	u8 reply[128];
	int ret;
	int len;

	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.route = session->xd->route;
	ctrl.sequence = sequence;
	uuid_copy(&ctrl.initiator_uuid, session->xd->local_uuid);
	uuid_copy(&ctrl.target_uuid, session->xd->remote_uuid);
	ctrl.command_id = atomic_inc_return(&session->command_id);

	len = tbv_tbip_build_logout(request, sizeof(request), &ctrl);
	if (len < 0)
		return len;

	memset(reply, 0, sizeof(reply));
	atomic64_inc(&session->identity->minimal_logout_tx);
	atomic64_inc(&session->logout_tx);
	ret = tb_xdomain_request(session->xd, request, len,
				 TB_CFG_PKG_XDOMAIN_RESP, reply, sizeof(reply),
				 TB_CFG_PKG_XDOMAIN_RESP,
				 TBV_TBNET_MIN_LOGOUT_TIMEOUT_MS);
	if (!ret)
		ret = tbv_tbip_parse_status(reply, sizeof(reply), &status);
	if (!ret &&
	    (!tbv_tbnet_minimal_status_matches(session, &status.ctrl) ||
	     status.status)) {
		ret = -EPROTO;
	}
	if (!ret) {
		atomic64_inc(&session->identity->minimal_status_rx);
		atomic64_inc(&session->status_rx);
	}

	return ret;
}

static void
tbv_tbnet_minimal_send_shutdown_logout(struct tbv_tbnet_minimal_session *session)
{
	bool should_logout;
	u8 sequence;
	int ret;

	mutex_lock(&session->lock);
	should_logout = session->login_sent || session->login_received ||
			session->path_enabled;
	sequence = (u8)(atomic_read(&session->command_id) & 0x3);
	mutex_unlock(&session->lock);

	if (!should_logout)
		return;

	ret = tbv_tbnet_minimal_send_logout_request(session, sequence);
	if (ret)
		pr_info("minimal TBnet shutdown logout route=0x%llx failed: %d\n",
			session->xd->route, ret);
	else
		pr_info("minimal TBnet shutdown logout route=0x%llx acknowledged\n",
			session->xd->route);
}

static int tbv_tbnet_minimal_handle_packet_common(
	struct tb_xdomain *source_xd, const void *buf, size_t size, void *data)
{
	struct tbv_tbnet_minimal_session *session = data;
	struct tbv_tbip_login_params login;
	struct tbv_tbip_control ctrl;
	enum tbv_tbip_type type;
	int ret;

	if (!session)
		return 0;
	if (source_xd && source_xd != session->xd)
		return 0;

	ret = tbv_tbip_parse_type(buf, size, &type, &ctrl);
	if (ret || !tbv_tbnet_minimal_ctrl_matches(session, &ctrl))
		return 0;

	switch (type) {
	case TBV_TBIP_LOGIN:
		ret = tbv_tbip_parse_login(buf, size, &login);
		if (ret)
			return 1;
		if (!login.transmit_path ||
		    login.transmit_path > (u32)INT_MAX)
			return 1;
		ret = tbv_tbnet_minimal_send_login_response(session, &login);
		if (ret)
			return 1;

		atomic64_inc(&session->identity->minimal_login_rx);
		atomic64_inc(&session->login_rx);
		mutex_lock(&session->lock);
		if (!session->removing) {
			session->login_received = true;
			session->login_retries = 0;
			session->remote_transmit_path =
				(int)login.transmit_path;
			if (!session->login_sent)
				queue_delayed_work(system_long_wq,
						   &session->login_work, 0);
		}
		mutex_unlock(&session->lock);
		tbv_services_tbnet_identity_ready(session->identity);
		tbv_tbnet_minimal_queue_connected(session, 0);
		return 1;

	case TBV_TBIP_LOGOUT:
		atomic64_inc(&session->identity->minimal_logout_rx);
		atomic64_inc(&session->logout_rx);
		ret = tbv_tbnet_minimal_send_status(session, &ctrl);
		if (!ret) {
			atomic64_inc(&session->identity->minimal_status_tx);
			atomic64_inc(&session->status_tx);
		}
		pr_info("minimal TBnet logout received route=0x%llx status_ret=%d\n",
			session->xd->route, ret);
		queue_work(system_long_wq, &session->disconnect_work);
		return 1;

	default:
		return 0;
	}
}

#ifdef TB_PROTOCOL_HANDLER_HAS_XDOMAIN
static int tbv_tbnet_minimal_handle_packet_xd(struct tb_xdomain *source_xd,
					      const void *buf, size_t size,
					      void *data)
{
	return tbv_tbnet_minimal_handle_packet_common(source_xd, buf, size,
						     data);
}
#else
static int tbv_tbnet_minimal_handle_packet(const void *buf, size_t size,
					   void *data)
{
	return tbv_tbnet_minimal_handle_packet_common(NULL, buf, size, data);
}
#endif

static bool
tbv_tbnet_minimal_xdomain_allowed(const struct tbv_tbnet_identity *identity,
				  const struct tb_xdomain *xd)
{
	const char *vendor = xd && xd->vendor_name ? xd->vendor_name : NULL;

	if (!identity->minimal_apple_only)
		return true;

	if (vendor && !strcmp(vendor, "Apple Inc."))
		return true;

	pr_info("skipping minimal TBnet service route=0x%llx vendor='%s'; Apple-only identity is enabled\n",
		xd ? xd->route : 0, vendor ?: "<unknown>");
	return false;
}

static bool
tbv_tbnet_minimal_legacy_session_allowed(struct tbv_tbnet_identity *identity,
					 struct tb_xdomain *xd)
{
#ifdef TB_PROTOCOL_HANDLER_HAS_XDOMAIN
	return true;
#else
	struct tbv_tbnet_minimal_session *session;
	bool allowed = true;

	mutex_lock(&identity->lock);
	list_for_each_entry(session, &identity->minimal_sessions, node) {
		struct tb_xdomain *existing = session->xd;
		bool same_host;

		if (existing == xd) {
			same_host = true;
		} else if (existing && xd && existing->remote_uuid &&
			   xd->remote_uuid) {
			same_host = uuid_equal(existing->remote_uuid,
					       xd->remote_uuid);
		} else {
			same_host = existing && xd &&
				    existing->route == xd->route &&
				    existing->link == xd->link &&
				    existing->depth == xd->depth;
		}

		if (!same_host)
			continue;

		allowed = false;
		break;
	}
	mutex_unlock(&identity->lock);

	if (!allowed)
		pr_warn("legacy source-blind minimal TBnet control: limiting remote Apple host to one link; apply callback_xd kernel support for multi-link Apple identity\n");

	return allowed;
#endif
}

static int tbv_tbnet_minimal_probe(struct tb_service *svc,
				   const struct tb_service_id *id)
{
	struct tbv_tbnet_identity *identity = tbv_tbnet_minimal_identity;
	struct tb_xdomain *xd = tb_service_parent(svc);
	struct tbv_tbnet_minimal_session *session;
	int ret;

	if (!identity)
		return -ENODEV;
	if (!tbv_tbnet_minimal_xdomain_allowed(identity, xd))
		return -ENODEV;
	if (!tbv_tbnet_minimal_legacy_session_allowed(identity, xd))
		return -EBUSY;

	session = kzalloc(sizeof(*session), GFP_KERNEL);
	if (!session)
		return -ENOMEM;

	session->identity = identity;
	session->svc = svc;
	session->xd = xd;
	session->local_transmit_path = 0;
	session->remote_transmit_path = 0;
	mutex_init(&session->lock);
	spin_lock_init(&session->tx_lock);
	INIT_LIST_HEAD(&session->tx_free);
	INIT_DELAYED_WORK(&session->login_work, tbv_tbnet_minimal_login_work);
	INIT_DELAYED_WORK(&session->connected_work,
			  tbv_tbnet_minimal_connected_work);
	INIT_WORK(&session->disconnect_work, tbv_tbnet_minimal_disconnect_work);
	INIT_WORK(&session->rx_work, tbv_tbnet_minimal_rx_work);
	INIT_DELAYED_WORK(&session->tx_poll_work,
			  tbv_tbnet_minimal_tx_poll_work);
	atomic_set(&session->command_id, 0);
	atomic_set(&session->frame_id, 0);
	atomic_set(&session->tx_inflight, 0);
	tbv_tbnet_minimal_generate_mac(session);

	ret = tbv_tbnet_minimal_alloc_rings(session);
	if (ret)
		goto err_free;

	session->handler.uuid = &tbv_tbnet_svc_uuid;
#ifdef TB_PROTOCOL_HANDLER_HAS_XDOMAIN
	session->handler.callback_xd = tbv_tbnet_minimal_handle_packet_xd;
#else
	session->handler.callback = tbv_tbnet_minimal_handle_packet;
#endif
#ifdef TB_PROTOCOL_HANDLER_HAS_OWNER
	session->handler.owner = THIS_MODULE;
#endif
	session->handler.data = session;
	ret = tb_register_protocol_handler(&session->handler);
	if (ret)
		goto err_free_rings;
	session->handler_registered = true;

	mutex_lock(&identity->lock);
	list_add_tail(&session->node, &identity->minimal_sessions);
	tbv_tbnet_minimal_recompute_state_locked(identity);
	mutex_unlock(&identity->lock);

	tb_service_set_drvdata(svc, session);
	queue_delayed_work(system_long_wq, &session->login_work, 0);
	pr_info("bound minimal TBnet service id=%d route=0x%llx tx_path=%d mac=%pM prtcstns=0x%x\n",
		svc->id, xd->route, session->local_transmit_path,
		session->mac, svc->prtcstns);
	return 0;

err_free_rings:
	tbv_tbnet_minimal_free_rings(session);
err_free:
	mutex_destroy(&session->lock);
	kfree(session);
	return ret;
}

static void
tbv_tbnet_minimal_free_rings(struct tbv_tbnet_minimal_session *session)
{
	tbv_tbnet_minimal_free_frame_array(session->rx_frames, session->rx_ring,
					   false);
	session->rx_frames = NULL;
	tbv_tbnet_minimal_free_frame_array(session->tx_frames, session->tx_ring,
					   true);
	session->tx_frames = NULL;

	if (session->rx_ring) {
		tb_ring_free(session->rx_ring);
		session->rx_ring = NULL;
	}
	if (session->local_transmit_path > 0) {
		tb_xdomain_release_out_hopid(session->xd,
					     session->local_transmit_path);
		session->local_transmit_path = 0;
	}
	if (session->tx_ring) {
		tb_ring_free(session->tx_ring);
		session->tx_ring = NULL;
	}
}

static void
tbv_tbnet_minimal_destroy_session(struct tbv_tbnet_minimal_session *session)
{
	struct tbv_tbnet_identity *identity = session->identity;

	mutex_lock(&session->lock);
	session->removing = true;
	mutex_unlock(&session->lock);

	if (session->handler_registered) {
		tb_unregister_protocol_handler(&session->handler);
		session->handler_registered = false;
	}
	cancel_delayed_work_sync(&session->login_work);
	cancel_delayed_work_sync(&session->connected_work);
	cancel_work_sync(&session->disconnect_work);
	cancel_work_sync(&session->rx_work);
	cancel_delayed_work_sync(&session->tx_poll_work);
	tbv_tbnet_minimal_send_shutdown_logout(session);
	tbv_tbnet_minimal_teardown_path(session);

	mutex_lock(&identity->lock);
	list_del_init(&session->node);
	tbv_tbnet_minimal_recompute_state_locked(identity);
	mutex_unlock(&identity->lock);

	tbv_tbnet_minimal_free_rings(session);
	mutex_destroy(&session->lock);
	kfree(session);
}

static void tbv_tbnet_minimal_remove(struct tb_service *svc)
{
	struct tbv_tbnet_minimal_session *session = tb_service_get_drvdata(svc);

	tb_service_set_drvdata(svc, NULL);
	if (session)
		tbv_tbnet_minimal_destroy_session(session);
}

static const struct tb_service_id tbv_tbnet_minimal_ids[] = {
	{ TB_SERVICE("network", 1) },
	{ },
};

static struct tb_service_driver tbv_tbnet_minimal_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name = "thunderbolt-ibverbs-tbnet",
	},
	.probe = tbv_tbnet_minimal_probe,
	.remove = tbv_tbnet_minimal_remove,
	.id_table = tbv_tbnet_minimal_ids,
};

int tbv_tbnet_minimal_start(struct tbv_tbnet_identity *identity)
{
	u32 prtcstns = tbv_tbnet_minimal_prtcstns(identity->minimal_e2e);
	int ret;

#ifndef TB_PROTOCOL_HANDLER_UNREGISTER_DRAINS
	pr_err("tbnet_identity=minimal_packet requires XDomain protocol handler unregister-drain support\n");
	return -EOPNOTSUPP;
#endif

	if (identity->minimal_started)
		return 0;

	identity->minimal_dir = tb_property_create_dir(&tbv_tbnet_dir_uuid);
	if (!identity->minimal_dir)
		return -ENOMEM;

	ret = tb_property_add_immediate(identity->minimal_dir, "prtcid", 1);
	ret = ret ?: tb_property_add_immediate(identity->minimal_dir,
					       "prtcvers", 1);
	ret = ret ?: tb_property_add_immediate(identity->minimal_dir,
					       "prtcrevs", 1);
	ret = ret ?: tb_property_add_immediate(identity->minimal_dir,
					       "prtcstns",
					       prtcstns);
	if (ret)
		goto err_free_dir;

	ret = tb_register_property_dir("network", identity->minimal_dir);
	if (ret) {
		pr_err("tbnet_identity=minimal_packet could not register the ThunderboltIP network service: %d; unload thunderbolt_net or use stock_proxy\n",
		       ret);
		goto err_free_dir;
	}
	identity->minimal_dir_registered = true;

	tbv_tbnet_minimal_identity = identity;
	ret = tb_register_service_driver(&tbv_tbnet_minimal_driver);
	if (ret)
		goto err_unregister_dir;
	identity->minimal_driver_registered = true;
	identity->minimal_started = true;
	return 0;

err_unregister_dir:
	tbv_tbnet_minimal_identity = NULL;
	tb_unregister_property_dir("network", identity->minimal_dir);
	identity->minimal_dir_registered = false;
err_free_dir:
	tb_property_free_dir(identity->minimal_dir);
	identity->minimal_dir = NULL;
	return ret;
}

void tbv_tbnet_minimal_stop(struct tbv_tbnet_identity *identity)
{
	if (identity->mode != TBV_TBNET_ID_MINIMAL_PACKET)
		return;

	if (identity->minimal_driver_registered) {
		tb_unregister_service_driver(&tbv_tbnet_minimal_driver);
		identity->minimal_driver_registered = false;
	}
	tbv_tbnet_minimal_identity = NULL;

	if (identity->minimal_dir_registered) {
		tb_unregister_property_dir("network", identity->minimal_dir);
		identity->minimal_dir_registered = false;
	}
	if (identity->minimal_dir) {
		tb_property_free_dir(identity->minimal_dir);
		identity->minimal_dir = NULL;
	}

	identity->minimal_started = false;
}
