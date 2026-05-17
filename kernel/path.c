// SPDX-License-Identifier: GPL-2.0

#include <linux/errno.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/thunderbolt.h>

#include "../proto/native_data.h"
#include "tbv.h"

#define TBV_NATIVE_RING_SIZE 1024
#define TBV_APPLE_RING_SIZE 256
#define TBV_DATA_FRAME_SIZE SZ_4K
#define TBV_DATA_PDF_FRAME_START 1
#define TBV_DATA_PDF_FRAME_END 3
#define TBV_CONTROL_FRAME_SIZE 256
#define TBV_CONTROL_QUEUE_MULTIPLIER 4
#define TBV_DATA_QUEUE_MULTIPLIER 4
#define TBV_DATA_PACKET_POOL_LIMIT 1024
#define TBV_TX_POLL_DELAY msecs_to_jiffies(1)

struct tbv_data_frame {
	struct ring_frame frame;
	struct tbv_path *path;
	struct list_head free_node;
	void *buf;
	dma_addr_t dma;
	struct tbv_tx_packet *packet;
	tbv_path_tx_done_fn done;
	void *done_ctx;
	bool tx;
};

struct tbv_tx_packet {
	struct list_head node;
	struct tbv_path *path;
	u8 *buf;
	u32 len;
	struct ring_frame frame;
	dma_addr_t dma;
	tbv_path_tx_done_fn done;
	void *done_ctx;
	u8 sof;
	u8 eof;
	bool control;
	bool pooled;
	bool queued;
	bool zcopy;
	bool unmap_dma;
	u8 control_buf[TBV_CONTROL_FRAME_SIZE];
};

static u32 tbv_frame_len(const struct ring_frame *frame)
{
	return frame->size ? frame->size : (u32)TBV_DATA_FRAME_SIZE;
}

static struct tbv_state *tbv_path_state(struct tbv_path *path)
{
	return path->rail && path->rail->peer ? path->rail->peer->state : NULL;
}

static u32 tbv_path_control_packet_count(const struct tbv_path *path)
{
	u32 count = path->cfg.tx_ring_size * TBV_CONTROL_QUEUE_MULTIPLIER;

	return clamp_t(u32, count, 64, 4096);
}

static u32 tbv_path_data_packet_count(const struct tbv_path *path)
{
	u32 count = path->cfg.tx_ring_size * TBV_DATA_QUEUE_MULTIPLIER;

	return min_t(u32, count, TBV_DATA_PACKET_POOL_LIMIT);
}

static void tbv_path_tx_packet_release(struct tbv_tx_packet *packet, int status)
{
	struct tbv_path *path = packet->path;
	unsigned long flags;

	if (packet->zcopy && packet->unmap_dma)
		dma_unmap_page(tb_ring_dma_device(path->tx_ring), packet->dma,
			       packet->len, DMA_TO_DEVICE);

	if (packet->done)
		packet->done(packet->done_ctx, status);

	packet->done = NULL;
	packet->done_ctx = NULL;
	packet->len = 0;
	packet->queued = false;

	if (packet->zcopy) {
		kfree(packet);
		return;
	}

	if (!packet->control && packet->pooled) {
		spin_lock_irqsave(&path->tx_lock, flags);
		list_add_tail(&packet->node, &path->tx_data_free);
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return;
	}

	if (!packet->control) {
		kfree(packet->buf);
		kfree(packet);
		return;
	}
	if (!packet->pooled) {
		kfree(packet);
		return;
	}

	packet->buf = packet->control_buf;
	spin_lock_irqsave(&path->tx_lock, flags);
	list_add_tail(&packet->node, &path->tx_control_free);
	spin_unlock_irqrestore(&path->tx_lock, flags);
}

static void tbv_path_schedule_tx(struct tbv_path *path);
static void tbv_path_tx_poll_work(struct work_struct *work);

static void tbv_path_schedule_tx_poll(struct tbv_path *path)
{
	if (atomic_read(&path->tx_inflight) > 0 &&
	    path->state == TBV_PATH_TUNNEL_ENABLED)
		schedule_delayed_work(&path->tx_poll_work, TBV_TX_POLL_DELAY);
}

void tbv_path_default_config(enum tbv_backend_type backend,
			     struct tbv_path_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->tx_hop = -1;
	cfg->rx_hop = -1;
	cfg->transmit_path = -1;
	cfg->receive_path = -1;

	switch (backend) {
	case TBV_BACKEND_APPLE:
		cfg->tx_ring_size = TBV_APPLE_RING_SIZE;
		cfg->rx_ring_size = TBV_APPLE_RING_SIZE;
		cfg->tx_flags = RING_FLAG_FRAME;
		cfg->rx_flags = RING_FLAG_FRAME | RING_FLAG_E2E;
		cfg->tx_hop = 2;
		cfg->rx_hop = 2;
		cfg->transmit_path = 9;
		cfg->receive_path = 9;
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
		   const struct tbv_path_config *cfg, struct tbv_rail *rail)
{
	memset(path, 0, sizeof(*path));
	path->state = TBV_PATH_NEW;
	path->cfg = *cfg;
	path->rail = rail;
	spin_lock_init(&path->tx_lock);
	INIT_LIST_HEAD(&path->tx_free);
	INIT_LIST_HEAD(&path->tx_control_free);
	INIT_LIST_HEAD(&path->tx_data_free);
	INIT_LIST_HEAD(&path->tx_control_queue);
	INIT_LIST_HEAD(&path->tx_data_queue);
	INIT_DELAYED_WORK(&path->tx_poll_work, tbv_path_tx_poll_work);
	atomic_set(&path->tx_inflight, 0);
	path->local_transmit_path = -1;
	path->local_tx_hop = -1;
	path->local_rx_hop = -1;
	path->remote_transmit_path = -1;
}

void tbv_path_reset(struct tbv_path *path)
{
	path->tx_ring = NULL;
	path->rx_ring = NULL;
	memset(path, 0, sizeof(*path));
	path->state = TBV_PATH_STOPPED;
	spin_lock_init(&path->tx_lock);
	INIT_LIST_HEAD(&path->tx_free);
	INIT_LIST_HEAD(&path->tx_control_free);
	INIT_LIST_HEAD(&path->tx_data_free);
	INIT_LIST_HEAD(&path->tx_control_queue);
	INIT_LIST_HEAD(&path->tx_data_queue);
	INIT_DELAYED_WORK(&path->tx_poll_work, tbv_path_tx_poll_work);
	atomic_set(&path->tx_inflight, 0);
	path->local_transmit_path = -1;
	path->local_tx_hop = -1;
	path->local_rx_hop = -1;
	path->remote_transmit_path = -1;
}

static void tbv_path_tx_complete(struct tb_ring *ring, struct ring_frame *frame,
				 bool canceled)
{
	struct tbv_data_frame *f = container_of(frame, struct tbv_data_frame,
						frame);
	struct tbv_path *path = f->path;
	struct tbv_tx_packet *packet;
	struct tbv_state *state = tbv_path_state(path);
	unsigned long flags;

	dma_sync_single_for_cpu(tb_ring_dma_device(ring), f->dma,
				TBV_DATA_FRAME_SIZE, DMA_TO_DEVICE);
	spin_lock_irqsave(&path->tx_lock, flags);
	packet = f->packet;
	f->packet = NULL;
	f->done = NULL;
	f->done_ctx = NULL;
	f->frame.callback = NULL;
	f->frame.size = 0;
	f->frame.flags = 0;
	f->frame.sof = 0;
	f->frame.eof = 0;
	list_add_tail(&f->free_node, &path->tx_free);
	spin_unlock_irqrestore(&path->tx_lock, flags);

	if (state) {
		if (canceled)
			atomic64_inc(&state->data_tx_canceled);
		else
			atomic64_inc(&state->data_tx_completed);
	}
	if (packet && !packet->control && !canceled)
		atomic64_inc(&path->data_tx_completed);
	if (packet)
		tbv_path_tx_packet_release(packet,
					   canceled ? -ECANCELED : 0);

	atomic_dec(&path->tx_inflight);
	tbv_path_schedule_tx(path);
}

static int tbv_path_post_rx_frame(struct tbv_data_frame *f);

static void tbv_path_rx_start_raw(struct tbv_path *path,
				  const struct tbv_native_data_header *hdr)
{
	path->rx_raw_opcode = hdr->opcode;
	path->rx_raw_flags = hdr->flags & ~TBV_NATIVE_DATA_F_RAW_STREAM;
	path->rx_raw_dest_qp = hdr->dest_qp;
	path->rx_raw_src_qp = hdr->src_qp;
	path->rx_raw_psn = hdr->psn;
	path->rx_raw_imm_data = hdr->imm_data;
	path->rx_raw_rkey = hdr->rkey;
	path->rx_raw_base = hdr->remote_addr;
	path->rx_raw_done = 0;
	path->rx_raw_remaining = hdr->length;
	path->rx_raw_pending = hdr->length != 0;
}

static void tbv_path_rx_raw_payload(struct tbv_path *path,
				    struct tbv_state *state,
				    const void *payload, u32 len)
{
	struct tbv_native_data_header hdr = {};

	if (!path->rx_raw_pending || !len || len > path->rx_raw_remaining) {
		if (state)
			atomic64_inc(&state->data_rx_bad_frame);
		path->rx_raw_pending = false;
		path->rx_raw_remaining = 0;
		return;
	}

	hdr.opcode = path->rx_raw_opcode;
	hdr.flags = path->rx_raw_flags;
	if (len == path->rx_raw_remaining) {
		hdr.flags |= TBV_NATIVE_DATA_F_LAST;
		path->rx_raw_pending = false;
	}
	hdr.dest_qp = path->rx_raw_dest_qp;
	hdr.src_qp = path->rx_raw_src_qp;
	hdr.psn = path->rx_raw_psn;
	hdr.length = len;
	hdr.imm_data = path->rx_raw_imm_data;
	hdr.remote_addr = path->rx_raw_base + path->rx_raw_done;
	hdr.rkey = path->rx_raw_rkey;

	path->rx_raw_done += len;
	path->rx_raw_remaining -= len;
	if (state)
		tbv_ibdev_rx_native_frame(state, &hdr, payload);
}

static void tbv_path_zcopy_tx_complete(struct tb_ring *ring,
				       struct ring_frame *frame,
				       bool canceled)
{
	struct tbv_tx_packet *packet = container_of(frame,
						   struct tbv_tx_packet,
						   frame);
	struct tbv_path *path = packet->path;
	struct tbv_state *state = tbv_path_state(path);

	if (state) {
		if (canceled)
			atomic64_inc(&state->data_tx_canceled);
		else
			atomic64_inc(&state->data_tx_completed);
	}
	if (!canceled)
		atomic64_inc(&path->data_tx_completed);

	tbv_path_tx_packet_release(packet, canceled ? -ECANCELED : 0);
	atomic_dec(&path->tx_inflight);
	tbv_path_schedule_tx(path);
	tbv_path_schedule_tx_poll(path);
}

static void tbv_path_tx_poll_work(struct work_struct *work)
{
	struct tbv_path *path =
		container_of(to_delayed_work(work), struct tbv_path,
			     tx_poll_work);
	struct ring_frame *frame;
	u32 completed = 0;

	if (!path->tx_ring || path->state != TBV_PATH_TUNNEL_ENABLED)
		return;

	while ((frame = tb_ring_poll(path->tx_ring))) {
		void (*callback)(struct tb_ring *, struct ring_frame *, bool);

		callback = frame->callback;
		if (callback)
			callback(path->tx_ring, frame, false);
		completed++;
	}

	if (completed)
		tbv_path_schedule_tx(path);
	tbv_path_schedule_tx_poll(path);
}

static void tbv_path_rx_complete(struct tb_ring *ring, struct ring_frame *frame,
				 bool canceled)
{
	struct tbv_data_frame *f = container_of(frame, struct tbv_data_frame,
						frame);
	struct tbv_path *path = f->path;
	struct tbv_state *state = tbv_path_state(path);
	u32 len = tbv_frame_len(frame);

	if (canceled)
		return;
	if (state)
		atomic64_inc(&state->data_rx_completed);
	atomic64_inc(&path->data_rx_completed);

	dma_sync_single_for_cpu(tb_ring_dma_device(ring), f->dma,
				TBV_DATA_FRAME_SIZE, DMA_FROM_DEVICE);
	if (len <= TBV_DATA_FRAME_SIZE && state) {
		struct tbv_native_data_header hdr;

		if (path->rail && path->rail->peer &&
		    path->rail->peer->backend == TBV_BACKEND_APPLE) {
			tbv_ibdev_rx_apple_frame(state, path, f->buf, len,
						 frame->eof);
		} else if (path->rx_raw_pending) {
			tbv_path_rx_raw_payload(path, state, f->buf, len);
		} else if (!tbv_native_data_parse_header(f->buf, len, &hdr) &&
			   (hdr.flags & TBV_NATIVE_DATA_F_RAW_STREAM)) {
			if (len != TBV_NATIVE_DATA_HDR_SIZE ||
			    !hdr.length ||
			    (hdr.flags & ~(TBV_NATIVE_DATA_F_LAST |
					   TBV_NATIVE_DATA_F_SOLICITED |
					   TBV_NATIVE_DATA_F_RAW_STREAM))) {
				atomic64_inc(&state->data_rx_bad_frame);
			} else {
				tbv_path_rx_start_raw(path, &hdr);
			}
		} else {
			tbv_ibdev_rx_frame(state, f->buf, len);
		}
	} else if (state) {
		atomic64_inc(&state->data_rx_bad_frame);
	}

	if (path->state == TBV_PATH_RING_STARTED ||
	    path->state == TBV_PATH_TUNNEL_ENABLED) {
		int ret = tbv_path_post_rx_frame(f);

		if (ret)
			pr_warn_ratelimited("RX repost failed ret=%d\n", ret);
	}
}

static int tbv_path_alloc_frames(struct tbv_path *path, bool tx)
{
	struct tbv_data_frame **frames_out = tx ? &path->tx_frames :
						 &path->rx_frames;
	u32 *count_out = tx ? &path->tx_frame_count : &path->rx_frame_count;
	u32 count = tx ? path->cfg.tx_ring_size : path->cfg.rx_ring_size;
	struct tb_ring *ring = tx ? path->tx_ring : path->rx_ring;
	struct device *dma_dev = tb_ring_dma_device(ring);
	struct tbv_data_frame *frames;
	int i;

	frames = kcalloc(count, sizeof(*frames), GFP_KERNEL);
	if (!frames)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		struct tbv_data_frame *f = &frames[i];

		f->path = path;
		f->tx = tx;
		INIT_LIST_HEAD(&f->frame.list);
		INIT_LIST_HEAD(&f->free_node);
		f->buf = kmalloc(TBV_DATA_FRAME_SIZE, GFP_KERNEL);
		if (!f->buf)
			goto err;
		f->dma = dma_map_single(dma_dev, f->buf, TBV_DATA_FRAME_SIZE,
					tx ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
		if (dma_mapping_error(dma_dev, f->dma)) {
			kfree(f->buf);
			f->buf = NULL;
			goto err;
		}
		f->frame.buffer_phy = f->dma;
		f->frame.size = 0;
		if (tx)
			list_add_tail(&f->free_node, &path->tx_free);
	}

	*frames_out = frames;
	*count_out = count;
	return 0;

err:
	while (--i >= 0) {
		struct tbv_data_frame *f = &frames[i];

		if (f->buf) {
			dma_unmap_single(dma_dev, f->dma, TBV_DATA_FRAME_SIZE,
					 tx ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
			kfree(f->buf);
		}
	}
	kfree(frames);
	return -ENOMEM;
}

static void tbv_path_free_frames(struct tbv_path *path, bool tx)
{
	struct tbv_data_frame *frames = tx ? path->tx_frames : path->rx_frames;
	u32 count = tx ? path->tx_frame_count : path->rx_frame_count;
	struct tb_ring *ring = tx ? path->tx_ring : path->rx_ring;
	struct device *dma_dev;
	u32 i;

	if (!frames || !ring)
		return;

	dma_dev = tb_ring_dma_device(ring);
	for (i = 0; i < count; i++) {
		struct tbv_data_frame *f = &frames[i];

		if (!f->buf)
			continue;
		dma_unmap_single(dma_dev, f->dma, TBV_DATA_FRAME_SIZE,
				 tx ? DMA_TO_DEVICE : DMA_FROM_DEVICE);
		kfree(f->buf);
	}

	if (tx) {
		path->tx_frames = NULL;
		path->tx_frame_count = 0;
		INIT_LIST_HEAD(&path->tx_free);
	} else {
		path->rx_frames = NULL;
		path->rx_frame_count = 0;
	}
	kfree(frames);
}

static int tbv_path_alloc_control_packets(struct tbv_path *path)
{
	struct tbv_tx_packet *packets;
	u32 count = tbv_path_control_packet_count(path);
	u32 i;

	packets = kcalloc(count, sizeof(*packets), GFP_KERNEL);
	if (!packets)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		struct tbv_tx_packet *packet = &packets[i];

		INIT_LIST_HEAD(&packet->node);
		packet->path = path;
		packet->buf = packet->control_buf;
		packet->control = true;
		packet->pooled = true;
		list_add_tail(&packet->node, &path->tx_control_free);
	}

	path->tx_control_packets = packets;
	path->tx_control_packet_count = count;
	path->tx_data_queue_limit = path->cfg.tx_ring_size *
				    TBV_DATA_QUEUE_MULTIPLIER;
	return 0;
}

static void tbv_path_free_control_packets(struct tbv_path *path)
{
	kfree(path->tx_control_packets);
	path->tx_control_packets = NULL;
	path->tx_control_packet_count = 0;
	path->tx_control_queued = 0;
	path->tx_data_queued = 0;
	path->tx_data_reserved = 0;
	path->tx_data_queue_limit = 0;
	INIT_LIST_HEAD(&path->tx_control_free);
}

static int tbv_path_alloc_data_packets(struct tbv_path *path)
{
	struct tbv_tx_packet *packets;
	u32 count = tbv_path_data_packet_count(path);
	u32 i;

	if (!path->rail || !path->rail->peer ||
	    path->rail->peer->backend != TBV_BACKEND_APPLE)
		return 0;

	packets = kcalloc(count, sizeof(*packets), GFP_KERNEL);
	if (!packets)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		struct tbv_tx_packet *packet = &packets[i];

		INIT_LIST_HEAD(&packet->node);
		packet->path = path;
		packet->buf = kmalloc(TBV_DATA_FRAME_SIZE, GFP_KERNEL);
		if (!packet->buf)
			goto err;
		packet->pooled = true;
		list_add_tail(&packet->node, &path->tx_data_free);
	}

	path->tx_data_packets = packets;
	path->tx_data_packet_count = count;
	return 0;

err:
	while (i-- > 0)
		kfree(packets[i].buf);
	kfree(packets);
	INIT_LIST_HEAD(&path->tx_data_free);
	return -ENOMEM;
}

static void tbv_path_free_data_packets(struct tbv_path *path)
{
	u32 i;

	for (i = 0; i < path->tx_data_packet_count; i++)
		kfree(path->tx_data_packets[i].buf);
	kfree(path->tx_data_packets);
	path->tx_data_packets = NULL;
	path->tx_data_packet_count = 0;
	INIT_LIST_HEAD(&path->tx_data_free);
}

static int tbv_path_post_rx_frame(struct tbv_data_frame *f)
{
	struct tbv_path *path = f->path;

	f->frame.callback = tbv_path_rx_complete;
	f->frame.size = 0;
	f->frame.flags = 0;
	f->frame.sof = 0;
	f->frame.eof = 0;
	dma_sync_single_for_device(tb_ring_dma_device(path->rx_ring), f->dma,
				   TBV_DATA_FRAME_SIZE, DMA_FROM_DEVICE);
	return tb_ring_rx(path->rx_ring, &f->frame);
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
	int tx_hop;
	int rx_hop;

	if (path->state != TBV_PATH_NEW && path->state != TBV_PATH_STOPPED)
		return -EBUSY;

	tx_hop = path->cfg.tx_hop;
	rx_hop = path->cfg.rx_hop;
	if (requested_transmit_path < 0)
		requested_transmit_path = path->cfg.transmit_path;

	path->tx_ring = tb_ring_alloc_tx(xd->tb->nhi, tx_hop,
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
	path->local_tx_hop = path->tx_ring->hop;

	if (path->cfg.e2e)
		e2e_tx_hop = path->tx_ring->hop;

	path->rx_ring = tb_ring_alloc_rx(xd->tb->nhi, rx_hop,
					 path->cfg.rx_ring_size,
					 path->cfg.rx_flags, e2e_tx_hop,
					 path->cfg.sof_mask,
					 path->cfg.eof_mask, NULL, NULL);
	if (!path->rx_ring) {
		tb_xdomain_release_out_hopid(xd, path->local_transmit_path);
		path->local_transmit_path = -1;
		tb_ring_free(path->tx_ring);
		path->tx_ring = NULL;
		path->local_tx_hop = -1;
		return -ENOMEM;
	}
	path->local_rx_hop = path->rx_ring->hop;

	if (tbv_path_alloc_frames(path, true))
		goto err_rx_ring;
	if (tbv_path_alloc_frames(path, false))
		goto err_tx_frames;
	if (tbv_path_alloc_control_packets(path))
		goto err_rx_frames;
	if (tbv_path_alloc_data_packets(path))
		goto err_control_packets;

	path->state = TBV_PATH_RING_ALLOCATED;
	return 0;

err_control_packets:
	tbv_path_free_control_packets(path);
err_rx_frames:
	tbv_path_free_frames(path, false);
err_tx_frames:
	tbv_path_free_frames(path, true);
err_rx_ring:
	tb_ring_free(path->rx_ring);
	path->rx_ring = NULL;
	path->local_rx_hop = -1;
	tb_xdomain_release_out_hopid(xd, path->local_transmit_path);
	path->local_transmit_path = -1;
	tb_ring_free(path->tx_ring);
	path->tx_ring = NULL;
	path->local_tx_hop = -1;
	return -ENOMEM;
}

int tbv_path_start_rings(struct tbv_path *path)
{
	u32 i;
	int ret;

	if (path->state != TBV_PATH_RING_ALLOCATED)
		return -EINVAL;

	tb_ring_start(path->tx_ring);
	tb_ring_start(path->rx_ring);
	path->state = TBV_PATH_RING_STARTED;
	for (i = 0; i < path->rx_frame_count; i++) {
		ret = tbv_path_post_rx_frame(&path->rx_frames[i]);
		if (ret) {
			pr_warn("post RX frame %u/%u failed ret=%d\n", i,
				path->rx_frame_count, ret);
			return ret;
		}
	}
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
				      path->local_tx_hop,
				      remote_transmit_path,
				      path->local_rx_hop);
	if (ret) {
		tb_xdomain_release_in_hopid(xd, remote_transmit_path);
		return ret;
	}

	path->remote_transmit_path = remote_transmit_path;
	path->state = TBV_PATH_TUNNEL_ENABLED;
	tbv_path_schedule_tx(path);
	return 0;
}

static struct tbv_tx_packet *
tbv_path_alloc_data_packet_owned(struct tbv_path *path, u8 *buf, u32 len,
				 tbv_path_tx_done_fn done, void *done_ctx)
{
	struct tbv_tx_packet *packet;

	packet = kzalloc(sizeof(*packet), GFP_KERNEL);
	if (!packet)
		return NULL;

	INIT_LIST_HEAD(&packet->node);
	packet->path = path;
	packet->buf = buf;
	packet->len = len;
	packet->done = done;
	packet->done_ctx = done_ctx;
	packet->sof = TBV_DATA_PDF_FRAME_START;
	packet->eof = TBV_DATA_PDF_FRAME_END;
	return packet;
}

static struct tbv_tx_packet *
tbv_path_alloc_data_packet(struct tbv_path *path, const void *data, u32 len,
			   tbv_path_tx_done_fn done, void *done_ctx)
{
	struct tbv_tx_packet *packet;
	u8 *buf;

	buf = kmemdup(data, len, GFP_KERNEL);
	if (!buf)
		return NULL;

	packet = tbv_path_alloc_data_packet_owned(path, buf, len, done,
						  done_ctx);
	if (!packet)
		kfree(buf);
	return packet;
}

static struct tbv_tx_packet *tbv_path_alloc_pooled_data_packet(
	struct tbv_path *path, u32 len, tbv_path_tx_done_fn done, void *done_ctx)
{
	struct tbv_tx_packet *packet;
	unsigned long flags;

	spin_lock_irqsave(&path->tx_lock, flags);
	if (list_empty(&path->tx_data_free)) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return NULL;
	}

	packet = list_first_entry(&path->tx_data_free, struct tbv_tx_packet,
				  node);
	list_del_init(&packet->node);
	spin_unlock_irqrestore(&path->tx_lock, flags);

	packet->len = len;
	packet->done = done;
	packet->done_ctx = done_ctx;
	packet->sof = TBV_DATA_PDF_FRAME_START;
	packet->eof = TBV_DATA_PDF_FRAME_END;
	packet->control = false;
	packet->queued = false;
	packet->zcopy = false;
	packet->unmap_dma = false;
	return packet;
}

static struct tbv_tx_packet *
tbv_path_alloc_zcopy_packet(struct tbv_path *path, dma_addr_t dma, u32 len,
			    bool unmap_dma, tbv_path_tx_done_fn done,
			    void *done_ctx)
{
	struct tbv_tx_packet *packet;

	packet = kzalloc(sizeof(*packet), GFP_KERNEL);
	if (!packet)
		return NULL;

	INIT_LIST_HEAD(&packet->node);
	INIT_LIST_HEAD(&packet->frame.list);
	packet->path = path;
	packet->len = len;
	packet->dma = dma;
	packet->done = done;
	packet->done_ctx = done_ctx;
	packet->sof = TBV_DATA_PDF_FRAME_START;
	packet->eof = TBV_DATA_PDF_FRAME_END;
	packet->zcopy = true;
	packet->unmap_dma = unmap_dma;
	return packet;
}

static int tbv_path_enqueue_control(struct tbv_path *path, const void *data,
				    u32 len)
{
	struct tbv_tx_packet *packet;
	unsigned long flags;
	bool pooled = true;

	if (len > TBV_CONTROL_FRAME_SIZE)
		return -EMSGSIZE;

	spin_lock_irqsave(&path->tx_lock, flags);
	if (path->state != TBV_PATH_TUNNEL_ENABLED) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return -ENOTCONN;
	}
	if (list_empty(&path->tx_control_free)) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		packet = kzalloc(sizeof(*packet), GFP_ATOMIC);
		if (!packet)
			return -ENOMEM;
		INIT_LIST_HEAD(&packet->node);
		packet->path = path;
		packet->buf = packet->control_buf;
		packet->control = true;
		pooled = false;
		spin_lock_irqsave(&path->tx_lock, flags);
		if (path->state != TBV_PATH_TUNNEL_ENABLED) {
			spin_unlock_irqrestore(&path->tx_lock, flags);
			kfree(packet);
			return -ENOTCONN;
		}
	} else {
		packet = list_first_entry(&path->tx_control_free,
					  struct tbv_tx_packet, node);
		list_del_init(&packet->node);
	}

	packet->len = len;
	packet->done = NULL;
	packet->done_ctx = NULL;
	packet->sof = TBV_DATA_PDF_FRAME_START;
	packet->eof = TBV_DATA_PDF_FRAME_END;
	packet->pooled = pooled;
	packet->queued = true;
	memcpy(packet->buf, data, len);
	list_add_tail(&packet->node, &path->tx_control_queue);
	path->tx_control_queued++;
	spin_unlock_irqrestore(&path->tx_lock, flags);

	tbv_path_schedule_tx(path);
	return 0;
}

static int tbv_path_enqueue_data(struct tbv_path *path,
				 struct tbv_tx_packet *packet,
				 bool defer_schedule)
{
	unsigned long flags;

	spin_lock_irqsave(&path->tx_lock, flags);
	if (path->state != TBV_PATH_TUNNEL_ENABLED) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return -ENOTCONN;
	}
	if (path->tx_data_reserved) {
		path->tx_data_reserved--;
	} else if (path->tx_data_queued >= path->tx_data_queue_limit) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return -ENOMEM;
	}

	packet->queued = true;
	list_add_tail(&packet->node, &path->tx_data_queue);
	path->tx_data_queued++;
	atomic64_inc(&path->data_tx_enqueued);
	spin_unlock_irqrestore(&path->tx_lock, flags);

	if (!defer_schedule)
		tbv_path_schedule_tx(path);
	return 0;
}

static int tbv_path_enqueue_data_list(struct tbv_path *path,
				      struct list_head *packets, u32 count,
				      bool defer_schedule)
{
	struct tbv_tx_packet *packet;
	unsigned long flags;
	u32 used;

	if (!count)
		return 0;

	spin_lock_irqsave(&path->tx_lock, flags);
	if (path->state != TBV_PATH_TUNNEL_ENABLED) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return -ENOTCONN;
	}

	used = path->tx_data_queued + path->tx_data_reserved;
	if (count > path->tx_data_queue_limit ||
	    used > path->tx_data_queue_limit - count) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return -ENOMEM;
	}

	list_for_each_entry(packet, packets, node) {
		packet->queued = true;
		path->tx_data_queued++;
		atomic64_inc(&path->data_tx_enqueued);
	}
	list_splice_tail_init(packets, &path->tx_data_queue);
	spin_unlock_irqrestore(&path->tx_lock, flags);

	if (!defer_schedule)
		tbv_path_schedule_tx(path);
	return 0;
}

int tbv_path_reserve_data(struct tbv_path *path, u32 frames)
{
	unsigned long flags;
	u32 used;

	if (!frames)
		return 0;

	spin_lock_irqsave(&path->tx_lock, flags);
	if (path->state != TBV_PATH_TUNNEL_ENABLED) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return -ENOTCONN;
	}

	used = path->tx_data_queued + path->tx_data_reserved;
	if (frames > path->tx_data_queue_limit ||
	    used > path->tx_data_queue_limit - frames) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return -ENOMEM;
	}

	path->tx_data_reserved += frames;
	spin_unlock_irqrestore(&path->tx_lock, flags);
	return 0;
}

void tbv_path_release_data_reservation(struct tbv_path *path, u32 frames)
{
	unsigned long flags;

	if (!frames)
		return;

	spin_lock_irqsave(&path->tx_lock, flags);
	if (path->tx_data_reserved >= frames)
		path->tx_data_reserved -= frames;
	else
		path->tx_data_reserved = 0;
	spin_unlock_irqrestore(&path->tx_lock, flags);
}

static void tbv_path_schedule_tx(struct tbv_path *path)
{
	struct tbv_state *state = tbv_path_state(path);
	unsigned long flags;

	spin_lock_irqsave(&path->tx_lock, flags);
	if (path->tx_scheduling) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return;
	}
	path->tx_scheduling = true;
	spin_unlock_irqrestore(&path->tx_lock, flags);

	for (;;) {
		struct tbv_tx_packet *packet;
		struct tbv_data_frame *f;
		bool needs_staging;
		int ret;

		spin_lock_irqsave(&path->tx_lock, flags);
		if (path->state != TBV_PATH_TUNNEL_ENABLED ||
		    (list_empty(&path->tx_control_queue) &&
		     list_empty(&path->tx_data_queue))) {
			path->tx_scheduling = false;
			spin_unlock_irqrestore(&path->tx_lock, flags);
			return;
		}

		if (!list_empty(&path->tx_control_queue)) {
			packet = list_first_entry(&path->tx_control_queue,
						  struct tbv_tx_packet, node);
			needs_staging = true;
		} else {
			packet = list_first_entry(&path->tx_data_queue,
						  struct tbv_tx_packet, node);
			needs_staging = !packet->zcopy;
		}

		if (needs_staging && list_empty(&path->tx_free)) {
			path->tx_scheduling = false;
			spin_unlock_irqrestore(&path->tx_lock, flags);
			return;
		}

		if (!list_empty(&path->tx_control_queue)) {
			packet = list_first_entry(&path->tx_control_queue,
						  struct tbv_tx_packet, node);
			path->tx_control_queued--;
		} else {
			packet = list_first_entry(&path->tx_data_queue,
						  struct tbv_tx_packet, node);
			path->tx_data_queued--;
		}
		list_del_init(&packet->node);
		packet->queued = false;

		if (needs_staging) {
			f = list_first_entry(&path->tx_free,
					     struct tbv_data_frame, free_node);
			list_del_init(&f->free_node);
		} else {
			f = NULL;
		}
		atomic_inc(&path->tx_inflight);
		spin_unlock_irqrestore(&path->tx_lock, flags);

		if (packet->zcopy) {
			packet->frame.buffer_phy = packet->dma;
			packet->frame.callback = tbv_path_zcopy_tx_complete;
			packet->frame.size = packet->len == TBV_DATA_FRAME_SIZE ?
					     0 : packet->len;
			packet->frame.flags = 0;
			packet->frame.sof = packet->sof;
			packet->frame.eof = packet->eof;

			ret = tb_ring_tx(path->tx_ring, &packet->frame);
			if (!ret) {
				if (state)
					atomic64_inc(&state->data_tx_posted);
				atomic64_inc(&path->data_tx_posted);
				tbv_path_schedule_tx_poll(path);
				continue;
			}

			if (state)
				atomic64_inc(&state->data_tx_errors);
			spin_lock_irqsave(&path->tx_lock, flags);
			if (ret == -ENOMEM &&
			    path->state == TBV_PATH_TUNNEL_ENABLED) {
				list_add(&packet->node, &path->tx_data_queue);
				path->tx_data_queued++;
				packet->queued = true;
				path->tx_scheduling = false;
				spin_unlock_irqrestore(&path->tx_lock, flags);
				atomic_dec(&path->tx_inflight);
				return;
			}
			spin_unlock_irqrestore(&path->tx_lock, flags);
			atomic_dec(&path->tx_inflight);
			tbv_path_tx_packet_release(packet, ret);
			spin_lock_irqsave(&path->tx_lock, flags);
			path->tx_scheduling = false;
			spin_unlock_irqrestore(&path->tx_lock, flags);
			return;
		}

		memcpy(f->buf, packet->buf, packet->len);
		f->packet = packet;
		f->done = packet->done;
		f->done_ctx = packet->done_ctx;
		f->frame.callback = tbv_path_tx_complete;
		f->frame.size = packet->len == TBV_DATA_FRAME_SIZE ? 0 :
							       packet->len;
		f->frame.flags = 0;
		f->frame.sof = packet->sof;
		f->frame.eof = packet->eof;
		dma_sync_single_for_device(tb_ring_dma_device(path->tx_ring),
					   f->dma, TBV_DATA_FRAME_SIZE,
					   DMA_TO_DEVICE);

		ret = tb_ring_tx(path->tx_ring, &f->frame);
		if (!ret) {
			if (state)
				atomic64_inc(&state->data_tx_posted);
			if (!packet->control)
				atomic64_inc(&path->data_tx_posted);
			continue;
		}

		if (state)
			atomic64_inc(&state->data_tx_errors);
		f->packet = NULL;
		f->done = NULL;
		f->done_ctx = NULL;
		f->frame.callback = NULL;
		spin_lock_irqsave(&path->tx_lock, flags);
		list_add_tail(&f->free_node, &path->tx_free);
		if (ret == -ENOMEM &&
		    path->state == TBV_PATH_TUNNEL_ENABLED) {
			if (packet->control) {
				list_add(&packet->node,
					 &path->tx_control_queue);
				path->tx_control_queued++;
			} else {
				list_add(&packet->node, &path->tx_data_queue);
				path->tx_data_queued++;
			}
			packet->queued = true;
			path->tx_scheduling = false;
			spin_unlock_irqrestore(&path->tx_lock, flags);
			atomic_dec(&path->tx_inflight);
			return;
		}
		spin_unlock_irqrestore(&path->tx_lock, flags);
		atomic_dec(&path->tx_inflight);
		tbv_path_tx_packet_release(packet, ret);
		spin_lock_irqsave(&path->tx_lock, flags);
		path->tx_scheduling = false;
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return;
	}
}

void tbv_path_kick_tx(struct tbv_path *path)
{
	if (path)
		tbv_path_schedule_tx(path);
}

int tbv_path_send(struct tbv_path *path, const void *data, u32 len,
		  unsigned int send_flags,
		  tbv_path_tx_done_fn done, void *done_ctx)
{
	struct tbv_tx_packet *packet;
	int ret;

	if (!len || len > TBV_DATA_FRAME_SIZE)
		return -EINVAL;
	if (send_flags & ~(TBV_PATH_SEND_CONTROL | TBV_PATH_SEND_DEFER))
		return -EINVAL;
	if (send_flags & TBV_PATH_SEND_CONTROL) {
		if (send_flags & TBV_PATH_SEND_DEFER)
			return -EINVAL;
		return tbv_path_enqueue_control(path, data, len);
	}

	packet = tbv_path_alloc_data_packet(path, data, len, done, done_ctx);
	if (!packet)
		return -ENOMEM;

	ret = tbv_path_enqueue_data(path, packet,
				    send_flags & TBV_PATH_SEND_DEFER);
	if (ret) {
		kfree(packet->buf);
		kfree(packet);
		return ret;
	}
	return 0;
}

int tbv_path_send_owned(struct tbv_path *path, void *data, u32 len,
			unsigned int send_flags,
			tbv_path_tx_done_fn done, void *done_ctx)
{
	return tbv_path_send_marked_owned(path, data, len,
					  TBV_DATA_PDF_FRAME_START,
					  TBV_DATA_PDF_FRAME_END,
					  send_flags, done, done_ctx);
}

int tbv_path_send_marked_owned(struct tbv_path *path, void *data, u32 len,
			       u8 sof, u8 eof, unsigned int send_flags,
			       tbv_path_tx_done_fn done, void *done_ctx)
{
	struct tbv_tx_packet *packet;
	int ret;

	if (!data)
		return -EINVAL;
	if (!len || len > TBV_DATA_FRAME_SIZE ||
	    (send_flags & ~(TBV_PATH_SEND_CONTROL | TBV_PATH_SEND_DEFER)) ||
	    (send_flags & TBV_PATH_SEND_CONTROL)) {
		kfree(data);
		return -EINVAL;
	}

	packet = tbv_path_alloc_data_packet_owned(path, data, len, done,
						  done_ctx);
	if (!packet) {
		kfree(data);
		return -ENOMEM;
	}
	packet->sof = sof;
	packet->eof = eof;

	ret = tbv_path_enqueue_data(path, packet,
				    send_flags & TBV_PATH_SEND_DEFER);
	if (ret) {
		kfree(packet->buf);
		kfree(packet);
		return ret;
	}
	return 0;
}

int tbv_path_send_marked_fill(struct tbv_path *path, u32 len,
			      u8 sof, u8 eof, unsigned int send_flags,
			      tbv_path_tx_fill_fn fill, void *fill_ctx,
			      tbv_path_tx_done_fn done, void *done_ctx)
{
	struct tbv_tx_packet *packet;
	u8 *buf;
	int ret;

	if (!fill)
		return -EINVAL;
	if (!len || len > TBV_DATA_FRAME_SIZE ||
	    (send_flags & ~(TBV_PATH_SEND_CONTROL | TBV_PATH_SEND_DEFER)) ||
	    (send_flags & TBV_PATH_SEND_CONTROL))
		return -EINVAL;

	packet = tbv_path_alloc_pooled_data_packet(path, len, done, done_ctx);
	if (packet) {
		ret = fill(fill_ctx, packet->buf, len);
		if (ret) {
			packet->done = NULL;
			packet->done_ctx = NULL;
			tbv_path_tx_packet_release(packet, ret);
			return ret;
		}
	} else {
		buf = kmalloc(len, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
		ret = fill(fill_ctx, buf, len);
		if (ret) {
			kfree(buf);
			return ret;
		}
		packet = tbv_path_alloc_data_packet_owned(path, buf, len,
							  done, done_ctx);
		if (!packet) {
			kfree(buf);
			return -ENOMEM;
		}
	}

	packet->sof = sof;
	packet->eof = eof;
	ret = tbv_path_enqueue_data(path, packet,
				    send_flags & TBV_PATH_SEND_DEFER);
	if (ret) {
		packet->done = NULL;
		packet->done_ctx = NULL;
		tbv_path_tx_packet_release(packet, ret);
		return ret;
	}
	return 0;
}

static void tbv_path_release_packet_list(struct list_head *packets, int status)
{
	while (!list_empty(packets)) {
		struct tbv_tx_packet *packet =
			list_first_entry(packets, struct tbv_tx_packet, node);

		list_del_init(&packet->node);
		tbv_path_tx_packet_release(packet, status);
	}
}

int tbv_path_send_page_stream(struct tbv_path *path,
			      const struct tbv_native_data_header *hdr,
			      u32 total_length, unsigned int send_flags,
			      tbv_path_tx_done_fn meta_done,
			      void *meta_done_ctx,
			      tbv_path_next_page_fn next, void *next_ctx)
{
	struct tbv_native_data_header stream_hdr;
	struct device *dma_dev;
	LIST_HEAD(packets);
	u32 prepared = 0;
	u32 packet_count = 0;
	struct tbv_tx_packet *packet;
	u8 *hdr_buf;
	int ret;

	if (!path || !hdr || !next || !total_length) {
		ret = -EINVAL;
		goto err_meta_done;
	}
	if (send_flags & ~(TBV_PATH_SEND_DEFER)) {
		ret = -EINVAL;
		goto err_meta_done;
	}
	if (total_length > TBV_NATIVE_DATA_MAX_MSG_SIZE) {
		ret = -EMSGSIZE;
		goto err_meta_done;
	}
	if (!path->tx_ring) {
		ret = -ENOTCONN;
		goto err_meta_done;
	}

	hdr_buf = kzalloc(TBV_NATIVE_DATA_HDR_SIZE, GFP_KERNEL);
	if (!hdr_buf) {
		ret = -ENOMEM;
		goto err_meta_done;
	}

	stream_hdr = *hdr;
	stream_hdr.length = total_length;
	stream_hdr.remote_addr = hdr->remote_addr;
	stream_hdr.flags |= TBV_NATIVE_DATA_F_RAW_STREAM;
	ret = tbv_native_data_build_header(hdr_buf, TBV_NATIVE_DATA_HDR_SIZE,
					   &stream_hdr);
	if (ret < 0) {
		kfree(hdr_buf);
		goto err_meta_done;
	}

	packet = tbv_path_alloc_data_packet_owned(path, hdr_buf,
						  TBV_NATIVE_DATA_HDR_SIZE,
						  meta_done, meta_done_ctx);
	if (!packet) {
		kfree(hdr_buf);
		ret = -ENOMEM;
		goto err_meta_done;
	}
	list_add_tail(&packet->node, &packets);
	packet_count++;
	dma_dev = tb_ring_dma_device(path->tx_ring);

	while (prepared < total_length) {
		tbv_path_tx_done_fn done = NULL;
		struct page *page = NULL;
		void *done_ctx = NULL;
		dma_addr_t dma;
		u32 page_off = 0;
		u32 len = 0;

		ret = next(next_ctx, &page, &page_off, &len, &done, &done_ctx);
		if (ret)
			goto err_release;
		if (!page || !len || len > TBV_DATA_FRAME_SIZE ||
		    len > total_length - prepared ||
		    page_off > PAGE_SIZE || len > PAGE_SIZE - page_off) {
			if (done)
				done(done_ctx, -EINVAL);
			ret = -EINVAL;
			goto err_release;
		}

		dma = dma_map_page(dma_dev, page, page_off, len,
				   DMA_TO_DEVICE);
		if (dma_mapping_error(dma_dev, dma)) {
			if (done)
				done(done_ctx, -EIO);
			ret = -EIO;
			goto err_release;
		}

		packet = tbv_path_alloc_zcopy_packet(path, dma, len, true,
						     done, done_ctx);
		if (!packet) {
			dma_unmap_page(dma_dev, dma, len, DMA_TO_DEVICE);
			if (done)
				done(done_ctx, -ENOMEM);
			ret = -ENOMEM;
			goto err_release;
		}
		list_add_tail(&packet->node, &packets);
		packet_count++;
		prepared += len;
	}

	ret = tbv_path_enqueue_data_list(path, &packets, packet_count,
					 send_flags & TBV_PATH_SEND_DEFER);
	if (ret)
		goto err_release;
	return 0;

err_release:
	tbv_path_release_packet_list(&packets, ret);
	return ret;

err_meta_done:
	if (meta_done)
		meta_done(meta_done_ctx, ret);
	return ret;
}

u32 tbv_path_cancel_data_done_ctx(struct tbv_path *path,
				  tbv_path_tx_done_fn done, void *done_ctx)
{
	struct tbv_tx_packet *packet;
	struct tbv_tx_packet *tmp;
	LIST_HEAD(cancel);
	unsigned long flags;
	u32 canceled = 0;
	u32 i;

	if (!done || !done_ctx)
		return 0;

	spin_lock_irqsave(&path->tx_lock, flags);
	list_for_each_entry_safe(packet, tmp, &path->tx_data_queue, node) {
		if (packet->done != done || packet->done_ctx != done_ctx)
			continue;
		list_del_init(&packet->node);
		packet->queued = false;
		if (path->tx_data_queued)
			path->tx_data_queued--;
		packet->done = NULL;
		packet->done_ctx = NULL;
		list_add_tail(&packet->node, &cancel);
		canceled++;
	}

	for (i = 0; i < path->tx_frame_count; i++) {
		struct tbv_data_frame *f = &path->tx_frames[i];

		packet = f->packet;
		if (!packet || packet->control ||
		    packet->done != done || packet->done_ctx != done_ctx)
			continue;
		packet->done = NULL;
		packet->done_ctx = NULL;
		f->done = NULL;
		f->done_ctx = NULL;
		canceled++;
	}
	spin_unlock_irqrestore(&path->tx_lock, flags);

	while (!list_empty(&cancel)) {
		packet = list_first_entry(&cancel, struct tbv_tx_packet, node);
		list_del_init(&packet->node);
		tbv_path_tx_packet_release(packet, -ECANCELED);
	}

	return canceled;
}

static void tbv_path_flush_tx_queue(struct tbv_path *path, int status)
{
	LIST_HEAD(control);
	LIST_HEAD(data);
	unsigned long flags;

	spin_lock_irqsave(&path->tx_lock, flags);
	list_splice_init(&path->tx_control_queue, &control);
	list_splice_init(&path->tx_data_queue, &data);
	path->tx_control_queued = 0;
	path->tx_data_queued = 0;
	path->tx_data_reserved = 0;
	path->tx_scheduling = false;
	spin_unlock_irqrestore(&path->tx_lock, flags);

	while (!list_empty(&control)) {
		struct tbv_tx_packet *packet =
			list_first_entry(&control, struct tbv_tx_packet, node);

		list_del_init(&packet->node);
		tbv_path_tx_packet_release(packet, status);
	}

	while (!list_empty(&data)) {
		struct tbv_tx_packet *packet =
			list_first_entry(&data, struct tbv_tx_packet, node);

		list_del_init(&packet->node);
		tbv_path_tx_packet_release(packet, status);
	}
}

void tbv_path_destroy(struct tbv_path *path, struct tb_xdomain *xd)
{
	if (path->state == TBV_PATH_TUNNEL_ENABLED) {
		tb_xdomain_disable_paths(xd, path->local_transmit_path,
					 path->local_tx_hop,
					 path->remote_transmit_path,
					 path->local_rx_hop);
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

	cancel_delayed_work_sync(&path->tx_poll_work);
	tbv_path_flush_tx_queue(path, -ECANCELED);

	if (path->rx_ring) {
		tbv_path_free_frames(path, false);
		tb_ring_free(path->rx_ring);
		path->rx_ring = NULL;
		path->local_rx_hop = -1;
	}

	if (path->local_transmit_path >= 0) {
		tb_xdomain_release_out_hopid(xd, path->local_transmit_path);
		path->local_transmit_path = -1;
	}

	if (path->tx_ring) {
		tbv_path_free_frames(path, true);
		tb_ring_free(path->tx_ring);
		path->tx_ring = NULL;
		path->local_tx_hop = -1;
	}
	tbv_path_free_data_packets(path);
	tbv_path_free_control_packets(path);

	path->state = TBV_PATH_STOPPED;
}
