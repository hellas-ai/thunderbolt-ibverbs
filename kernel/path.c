// SPDX-License-Identifier: GPL-2.0

#include <linux/errno.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/thunderbolt.h>

#include "tbv.h"

#define TBV_NATIVE_RING_SIZE 1024
#define TBV_APPLE_RING_SIZE 256
#define TBV_DATA_FRAME_SIZE SZ_4K
#define TBV_DATA_PDF_FRAME_START 1
#define TBV_DATA_PDF_FRAME_END 3
#define TBV_CONTROL_FRAME_SIZE 256
#define TBV_CONTROL_QUEUE_MULTIPLIER 4
#define TBV_DATA_QUEUE_MULTIPLIER 4

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
	tbv_path_tx_done_fn done;
	void *done_ctx;
	bool control;
	bool pooled;
	bool queued;
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

static void tbv_path_tx_packet_release(struct tbv_tx_packet *packet, int status)
{
	struct tbv_path *path = packet->path;
	unsigned long flags;

	if (packet->done)
		packet->done(packet->done_ctx, status);

	packet->done = NULL;
	packet->done_ctx = NULL;
	packet->len = 0;
	packet->queued = false;

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
		   const struct tbv_path_config *cfg, struct tbv_rail *rail)
{
	memset(path, 0, sizeof(*path));
	path->state = TBV_PATH_NEW;
	path->cfg = *cfg;
	path->rail = rail;
	spin_lock_init(&path->tx_lock);
	INIT_LIST_HEAD(&path->tx_free);
	INIT_LIST_HEAD(&path->tx_control_free);
	INIT_LIST_HEAD(&path->tx_control_queue);
	INIT_LIST_HEAD(&path->tx_data_queue);
	atomic_set(&path->tx_inflight, 0);
	path->local_transmit_path = -1;
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
	INIT_LIST_HEAD(&path->tx_control_queue);
	INIT_LIST_HEAD(&path->tx_data_queue);
	atomic_set(&path->tx_inflight, 0);
	path->local_transmit_path = -1;
	path->remote_transmit_path = -1;
}

static void tbv_path_tx_complete(struct tb_ring *ring, struct ring_frame *frame,
				 bool canceled)
{
	struct tbv_data_frame *f = container_of(frame, struct tbv_data_frame,
						frame);
	struct tbv_path *path = f->path;
	struct tbv_tx_packet *packet = f->packet;
	struct tbv_state *state = tbv_path_state(path);
	unsigned long flags;

	dma_sync_single_for_cpu(tb_ring_dma_device(ring), f->dma,
				TBV_DATA_FRAME_SIZE, DMA_TO_DEVICE);
	if (state) {
		if (canceled)
			atomic64_inc(&state->data_tx_canceled);
		else
			atomic64_inc(&state->data_tx_completed);
	}
	if (packet)
		tbv_path_tx_packet_release(packet,
					   canceled ? -ECANCELED : 0);

	f->packet = NULL;
	f->done = NULL;
	f->done_ctx = NULL;
	f->frame.callback = NULL;
	f->frame.size = 0;
	f->frame.flags = 0;
	f->frame.sof = 0;
	f->frame.eof = 0;

	spin_lock_irqsave(&path->tx_lock, flags);
	list_add_tail(&f->free_node, &path->tx_free);
	spin_unlock_irqrestore(&path->tx_lock, flags);
	atomic_dec(&path->tx_inflight);
	tbv_path_schedule_tx(path);
}

static int tbv_path_post_rx_frame(struct tbv_data_frame *f);

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

	dma_sync_single_for_cpu(tb_ring_dma_device(ring), f->dma,
				TBV_DATA_FRAME_SIZE, DMA_FROM_DEVICE);
	if (len <= TBV_DATA_FRAME_SIZE && state)
		tbv_ibdev_rx_frame(state, f->buf, len);
	else if (state)
		atomic64_inc(&state->data_rx_bad_frame);

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

	if (tbv_path_alloc_frames(path, true))
		goto err_rx_ring;
	if (tbv_path_alloc_frames(path, false))
		goto err_tx_frames;
	if (tbv_path_alloc_control_packets(path))
		goto err_rx_frames;

	path->state = TBV_PATH_RING_ALLOCATED;
	return 0;

err_rx_frames:
	tbv_path_free_frames(path, false);
err_tx_frames:
	tbv_path_free_frames(path, true);
err_rx_ring:
	tb_ring_free(path->rx_ring);
	path->rx_ring = NULL;
	tb_xdomain_release_out_hopid(xd, path->local_transmit_path);
	path->local_transmit_path = -1;
	tb_ring_free(path->tx_ring);
	path->tx_ring = NULL;
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
				      path->tx_ring->hop,
				      remote_transmit_path,
				      path->rx_ring->hop);
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
tbv_path_alloc_data_packet(struct tbv_path *path, const void *data, u32 len,
			   tbv_path_tx_done_fn done, void *done_ctx)
{
	struct tbv_tx_packet *packet;
	u8 *buf;

	packet = kzalloc(sizeof(*packet), GFP_KERNEL);
	if (!packet)
		return NULL;
	buf = kmemdup(data, len, GFP_KERNEL);
	if (!buf) {
		kfree(packet);
		return NULL;
	}

	INIT_LIST_HEAD(&packet->node);
	packet->path = path;
	packet->buf = buf;
	packet->len = len;
	packet->done = done;
	packet->done_ctx = done_ctx;
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
				 struct tbv_tx_packet *packet)
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
	spin_unlock_irqrestore(&path->tx_lock, flags);

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
		int ret;

		spin_lock_irqsave(&path->tx_lock, flags);
		if (path->state != TBV_PATH_TUNNEL_ENABLED ||
		    list_empty(&path->tx_free) ||
		    (list_empty(&path->tx_control_queue) &&
		     list_empty(&path->tx_data_queue))) {
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

		f = list_first_entry(&path->tx_free, struct tbv_data_frame,
				     free_node);
		list_del_init(&f->free_node);
		atomic_inc(&path->tx_inflight);
		spin_unlock_irqrestore(&path->tx_lock, flags);

		memcpy(f->buf, packet->buf, packet->len);
		f->packet = packet;
		f->done = packet->done;
		f->done_ctx = packet->done_ctx;
		f->frame.callback = tbv_path_tx_complete;
		f->frame.size = packet->len == TBV_DATA_FRAME_SIZE ? 0 :
							       packet->len;
		f->frame.flags = 0;
		f->frame.sof = TBV_DATA_PDF_FRAME_START;
		f->frame.eof = TBV_DATA_PDF_FRAME_END;
		dma_sync_single_for_device(tb_ring_dma_device(path->tx_ring),
					   f->dma, TBV_DATA_FRAME_SIZE,
					   DMA_TO_DEVICE);

		ret = tb_ring_tx(path->tx_ring, &f->frame);
		if (!ret) {
			if (state)
				atomic64_inc(&state->data_tx_posted);
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

int tbv_path_send(struct tbv_path *path, const void *data, u32 len,
		  unsigned int send_flags,
		  tbv_path_tx_done_fn done, void *done_ctx)
{
	struct tbv_tx_packet *packet;
	int ret;

	if (!len || len > TBV_DATA_FRAME_SIZE)
		return -EINVAL;
	if (send_flags & ~TBV_PATH_SEND_CONTROL)
		return -EINVAL;
	if (send_flags & TBV_PATH_SEND_CONTROL)
		return tbv_path_enqueue_control(path, data, len);

	packet = tbv_path_alloc_data_packet(path, data, len, done, done_ctx);
	if (!packet)
		return -ENOMEM;

	ret = tbv_path_enqueue_data(path, packet);
	if (ret) {
		kfree(packet->buf);
		kfree(packet);
		return ret;
	}
	return 0;
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

	tbv_path_flush_tx_queue(path, -ECANCELED);

	if (path->rx_ring) {
		tbv_path_free_frames(path, false);
		tb_ring_free(path->rx_ring);
		path->rx_ring = NULL;
	}

	if (path->local_transmit_path >= 0) {
		tb_xdomain_release_out_hopid(xd, path->local_transmit_path);
		path->local_transmit_path = -1;
	}

	if (path->tx_ring) {
		tbv_path_free_frames(path, true);
		tb_ring_free(path->tx_ring);
		path->tx_ring = NULL;
	}
	tbv_path_free_control_packets(path);

	path->state = TBV_PATH_STOPPED;
}
