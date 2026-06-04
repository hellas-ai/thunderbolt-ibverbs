// SPDX-License-Identifier: GPL-2.0

#include <linux/errno.h>
#include <linux/dma-mapping.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/thunderbolt.h>

#include "../proto/native_data.h"
#include "tbv.h"

#define TBV_NATIVE_RING_SIZE 1024
/* Apple-originated bursts can exhaust a 256-entry RX ring before credits
 * recycle. 1024 entries passed checked Mac-to-Linux UC bursts beyond one full
 * ring while keeping per-direction buffer cost modest.
 */
#define TBV_APPLE_RING_SIZE 16384
#define TBV_DATA_FRAME_SIZE SZ_4K
#define TBV_CONTROL_FRAME_SIZE 256
#define TBV_CONTROL_QUEUE_MULTIPLIER 4
#define TBV_DATA_CREDIT_CONTROL_RESERVE 256
#define TBV_DATA_TX_MAX_INFLIGHT 32
#define TBV_TX_POLL_DELAY_MS 1
#define TBV_RX_SUPP_POLL_DELAY_MS 1
#define TBV_RX_SUPP_POLL_WINDOW_MS 16
/*
 * Raw-stream zcopy serializes each DMA path, so several QPs sharing a rail can
 * briefly queue a full TX-depth worth of packetized WRs behind one active
 * stream. Keep enough metadata headroom for qps=8/TX-depth=16/1 MiB WRITE and
 * qps=4/TX-depth=128/64 KiB without reporting a false SQ-full error.
 */
#define TBV_DATA_QUEUE_MULTIPLIER 64
#define TBV_DATA_PACKET_POOL_LIMIT 1024

typedef int (*tbv_ring_throttling_fn)(struct tb_ring *ring,
				      unsigned int interval_nsec);

extern int tb_ring_throttling(struct tb_ring *ring,
			      unsigned int interval_nsec);

static uint nhi_interrupt_throttle_ns;
module_param(nhi_interrupt_throttle_ns, uint, 0644);
MODULE_PARM_DESC(nhi_interrupt_throttle_ns,
		 "NHI interrupt throttling interval for TBV data rings in ns; 0 disables ring throttling");

static tbv_ring_throttling_fn tbv_ring_throttling;

void tbv_path_init_optional_symbols(void)
{
	tbv_ring_throttling = symbol_get(tb_ring_throttling);
	if (tbv_ring_throttling)
		pr_info("using optional tb_ring_throttling() helper\n");
	else
		pr_info("optional tb_ring_throttling() helper unavailable; using stock NHI interrupt throttling\n");
}

void tbv_path_exit_optional_symbols(void)
{
	if (!tbv_ring_throttling)
		return;

	symbol_put(tb_ring_throttling);
	tbv_ring_throttling = NULL;
}

static bool apple_tx_raw_mode = true;
module_param(apple_tx_raw_mode, bool, 0644);
MODULE_PARM_DESC(apple_tx_raw_mode,
		 "Use RAW descriptors for Apple-compatible TX rings");

static bool apple_tx_e2e;
module_param(apple_tx_e2e, bool, 0644);
MODULE_PARM_DESC(apple_tx_e2e,
		 "Enable E2E flow control on Apple-compatible TX rings");

static bool apple_rx_raw_mode;
module_param(apple_rx_raw_mode, bool, 0644);
MODULE_PARM_DESC(apple_rx_raw_mode,
		 "Use RAW descriptors for Apple-compatible RX rings; default keeps FRAME reassembly");

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
	void *owner_ctx;
	u8 sof;
	u8 eof;
	u32 start_credit_group_frames;
	unsigned long queued_jiffies;
	bool control;
	bool pooled;
	bool queued;
	bool inflight;
	bool zcopy;
	bool unmap_dma;
	bool raw_stream_start;
	bool raw_stream_end;
	bool raw_stream_counted;
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

static u32 tbv_path_tx_control_frame_reserve(const struct tbv_path *path)
{
	u32 reserve;

	if (path->tx_frame_count <= 1)
		return 0;

	reserve = path->tx_frame_count / 4;
	return clamp_t(u32, reserve, 1, TBV_DATA_CREDIT_CONTROL_RESERVE);
}

static int tbv_path_configure_ring_throttling(struct tbv_path *path)
{
	u32 interval = READ_ONCE(nhi_interrupt_throttle_ns);
	int ret;

	if (!tbv_ring_throttling) {
		if (interval)
			pr_warn_once("nhi_interrupt_throttle_ns requires a kernel exporting tb_ring_throttling(); ignoring interval %u ns\n",
				     interval);
		return 0;
	}

	ret = tbv_ring_throttling(path->tx_ring, interval);
	if (ret) {
		pr_warn("TX ring throttling interval %u ns failed ret=%d\n",
			interval, ret);
		return ret;
	}

	ret = tbv_ring_throttling(path->rx_ring, interval);
	if (ret) {
		pr_warn("RX ring throttling interval %u ns failed ret=%d\n",
			interval, ret);
		return ret;
	}

	return 0;
}

static void tbv_path_tx_packet_release(struct tbv_tx_packet *packet, int status)
{
	struct tbv_path *path = packet->path;
	unsigned long flags;

	if (packet->zcopy && packet->unmap_dma) {
		struct device *dma_dev = tb_ring_dma_device(path->tx_ring);

		if (tbv_dma_device_ready(dma_dev))
			dma_unmap_page(dma_dev, packet->dma, packet->len,
				       DMA_TO_DEVICE);
		else
			pr_warn_ratelimited("TX ring DMA device is not ready for zcopy unmapping\n");
	}

	if (packet->done)
		packet->done(packet->done_ctx, status);

	packet->done = NULL;
	packet->done_ctx = NULL;
	packet->owner_ctx = NULL;
	packet->len = 0;
	packet->start_credit_group_frames = 0;
	packet->queued_jiffies = 0;
	packet->queued = false;
	packet->inflight = false;

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
static void tbv_path_rx_supp_poll_work(struct work_struct *work);

static bool tbv_path_native_progress_enabled(const struct tbv_path *path)
{
	return path->rail && path->rail->peer &&
	       path->rail->peer->backend == TBV_BACKEND_NATIVE;
}

static void tbv_path_queue_delayed_work(struct tbv_path *path,
					struct delayed_work *work,
					unsigned long delay)
{
	struct tbv_state *state = tbv_path_state(path);

	if (state && state->workqueue)
		queue_delayed_work(state->workqueue, work, delay);
	else
		schedule_delayed_work(work, delay);
}

static void tbv_path_queue_tx_poll(struct tbv_path *path, unsigned long delay)
{
	if (!path->tx_poll_enabled || !path->tx_ring)
		return;

	tbv_path_queue_delayed_work(path, &path->tx_poll_work, delay);
}

static void tbv_path_queue_rx_supp_poll(struct tbv_path *path,
					unsigned long delay)
{
	if (!path->rx_supp_poll_enabled || !path->rx_ring)
		return;

	WRITE_ONCE(path->rx_supp_poll_until,
		   jiffies + msecs_to_jiffies(TBV_RX_SUPP_POLL_WINDOW_MS));
	tbv_path_queue_delayed_work(path, &path->rx_supp_poll_work, delay);
}

static void tbv_path_atomic64_max_ms(atomic64_t *counter, u64 value)
{
	s64 old;

	if (value > S64_MAX)
		value = S64_MAX;

	for (;;) {
		old = atomic64_read(counter);
		if (old >= (s64)value)
			return;
		if (atomic64_cmpxchg(counter, old, (s64)value) == old)
			return;
	}
}

static u32 tbv_path_data_credit_window(u32 rx_ring_size)
{
	u32 credits;

	if (!rx_ring_size)
		return 0;

	if (rx_ring_size <= TBV_DATA_CREDIT_CONTROL_RESERVE)
		credits = rx_ring_size / 2;
	else
		credits = rx_ring_size - TBV_DATA_CREDIT_CONTROL_RESERVE;

	if (credits > TBV_NATIVE_DATA_CREDIT_BATCH)
		credits -= credits % TBV_NATIVE_DATA_CREDIT_BATCH;
	if (!credits)
		credits = 1;

	return credits;
}

void tbv_path_set_remote_rx_capacity(struct tbv_path *path, u32 rx_ring_size)
{
	unsigned long flags;
	u32 credits;

	if (!path)
		return;

	credits = tbv_path_data_credit_window(rx_ring_size);
	spin_lock_irqsave(&path->tx_lock, flags);
	path->tx_remote_data_credit_max = credits;
	path->tx_remote_data_credits = credits;
	path->rx_data_credit_pending = 0;
	spin_unlock_irqrestore(&path->tx_lock, flags);

	tbv_path_schedule_tx(path);
}

void tbv_path_add_remote_rx_credits(struct tbv_path *path, u32 credits)
{
	struct tbv_state *state;
	unsigned long flags;
	u32 accepted = 0;
	u32 old;
	u32 new;

	if (!path || !credits)
		return;

	state = tbv_path_state(path);
	spin_lock_irqsave(&path->tx_lock, flags);
	if (path->tx_remote_data_credit_max) {
		old = path->tx_remote_data_credits;
		if (old >= path->tx_remote_data_credit_max)
			new = path->tx_remote_data_credit_max;
		else if (credits > path->tx_remote_data_credit_max - old)
			new = path->tx_remote_data_credit_max;
		else
			new = old + credits;
		path->tx_remote_data_credits = new;
		accepted = new - old;
	}
	spin_unlock_irqrestore(&path->tx_lock, flags);

	if (!accepted)
		return;

	if (state)
		atomic64_add(accepted, &state->data_tx_credit_received);
	atomic64_add(accepted, &path->data_tx_credit_received);
	tbv_path_schedule_tx(path);
}

static int tbv_path_send_rx_credit(struct tbv_path *path, u32 credits)
{
	struct tbv_native_data_header hdr = {};
	u8 frame[TBV_NATIVE_DATA_HDR_SIZE];
	int len;

	hdr.opcode = TBV_NATIVE_DATA_OP_PATH_CREDIT;
	hdr.imm_data = credits;

	len = tbv_native_data_build_header(frame, sizeof(frame), &hdr);
	if (len < 0)
		return len;

	return tbv_path_send(path, frame, len, TBV_PATH_SEND_CONTROL, NULL, NULL);
}

static void tbv_path_return_rx_data_credit(struct tbv_path *path, u32 credits)
{
	struct tbv_state *state;
	unsigned long flags;
	u32 threshold;
	u32 pending;
	u32 send = 0;
	int ret;

	if (!path || !credits)
		return;

	state = tbv_path_state(path);
	threshold = tbv_native_data_credit_return_threshold(
		tbv_path_data_credit_window(path->cfg.rx_ring_size));
	spin_lock_irqsave(&path->tx_lock, flags);
	pending = path->rx_data_credit_pending;
	if (credits > U32_MAX - pending)
		pending = U32_MAX;
	else
		pending += credits;
	if (pending >= threshold) {
		send = pending;
		pending = 0;
	}
	path->rx_data_credit_pending = pending;
	spin_unlock_irqrestore(&path->tx_lock, flags);

	if (!send)
		return;

	ret = tbv_path_send_rx_credit(path, send);
	if (ret) {
		spin_lock_irqsave(&path->tx_lock, flags);
		pending = path->rx_data_credit_pending;
		if (send > U32_MAX - pending)
			path->rx_data_credit_pending = U32_MAX;
		else
			path->rx_data_credit_pending = pending + send;
		spin_unlock_irqrestore(&path->tx_lock, flags);
		if (state)
			atomic64_inc(&state->data_rx_credit_send_error);
		atomic64_inc(&path->data_rx_credit_send_error);
		return;
	}

	if (state)
		atomic64_add(send, &state->data_rx_credit_sent);
	atomic64_add(send, &path->data_rx_credit_sent);
}

static bool tbv_native_data_consumes_rx_credit(u8 opcode)
{
	switch (opcode) {
	case TBV_NATIVE_DATA_OP_SEND:
	case TBV_NATIVE_DATA_OP_SEND_IMM:
	case TBV_NATIVE_DATA_OP_RDMA_WRITE:
	case TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM:
	case TBV_NATIVE_DATA_OP_RDMA_READ_REQ:
	case TBV_NATIVE_DATA_OP_RDMA_READ_RESP:
	case TBV_NATIVE_DATA_OP_MAD:
		return true;
	default:
		return false;
	}
}

static bool
tbv_native_data_valid_path_credit(const struct tbv_native_data_header *hdr)
{
	return hdr->opcode == TBV_NATIVE_DATA_OP_PATH_CREDIT &&
	       hdr->imm_data &&
	       !hdr->flags &&
	       !hdr->dest_qp &&
	       !hdr->src_qp &&
	       !hdr->psn &&
	       !hdr->length &&
	       !hdr->remote_addr &&
	       !hdr->rkey;
}

static void tbv_path_count_raw_stream_locked(struct tbv_path *path,
					     struct tbv_tx_packet *packet)
{
	if (packet->raw_stream_start) {
		path->tx_raw_stream_active = true;
		path->tx_raw_stream_owner = packet->owner_ctx;
		path->tx_raw_stream_inflight = 0;
		path->tx_raw_stream_end_seen = false;
	}
	if (path->tx_raw_stream_active &&
	    packet->owner_ctx == path->tx_raw_stream_owner) {
		path->tx_raw_stream_inflight++;
		packet->raw_stream_counted = true;
	}
}

static void tbv_path_finish_raw_stream_if_needed(struct tbv_path *path,
						 struct tbv_tx_packet *packet)
{
	unsigned long flags;

	if (!packet || !packet->raw_stream_counted)
		return;

	spin_lock_irqsave(&path->tx_lock, flags);
	if (path->tx_raw_stream_owner == packet->owner_ctx) {
		if (packet->raw_stream_end)
			path->tx_raw_stream_end_seen = true;
		if (path->tx_raw_stream_inflight)
			path->tx_raw_stream_inflight--;
		if (path->tx_raw_stream_end_seen &&
		    !path->tx_raw_stream_inflight) {
			path->tx_raw_stream_active = false;
			path->tx_raw_stream_owner = NULL;
			path->tx_raw_stream_end_seen = false;
		}
	}
	packet->raw_stream_counted = false;
	if (!path->tx_raw_stream_active) {
		path->tx_raw_stream_active = false;
		path->tx_raw_stream_owner = NULL;
		path->tx_raw_stream_end_seen = false;
		path->tx_raw_stream_inflight = 0;
	}
	spin_unlock_irqrestore(&path->tx_lock, flags);
}

bool tbv_path_apple_tx_raw_mode(void)
{
	return READ_ONCE(apple_tx_raw_mode);
}

bool tbv_path_apple_rx_raw_mode(void)
{
	return READ_ONCE(apple_rx_raw_mode);
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
		cfg->tx_flags = 0;
		if (!tbv_path_apple_tx_raw_mode())
			cfg->tx_flags |= RING_FLAG_FRAME;
		if (READ_ONCE(apple_tx_e2e))
			cfg->tx_flags |= RING_FLAG_E2E;
		cfg->rx_flags = RING_FLAG_E2E;
		if (!tbv_path_apple_rx_raw_mode())
			cfg->rx_flags |= RING_FLAG_FRAME;
		cfg->tx_hop = 2;
		cfg->rx_hop = 2;
		cfg->transmit_path = 9;
		cfg->receive_path = 9;
		if (tbv_path_apple_rx_raw_mode()) {
			cfg->sof_mask = 0xffff;
			cfg->eof_mask = 0xffff;
		} else {
			cfg->sof_mask = BIT(1);
			cfg->eof_mask = BIT(2) | BIT(3);
		}
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
	INIT_LIST_HEAD(&path->tx_zcopy_inflight);
	INIT_DELAYED_WORK(&path->tx_poll_work, tbv_path_tx_poll_work);
	INIT_DELAYED_WORK(&path->rx_supp_poll_work,
			  tbv_path_rx_supp_poll_work);
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
	INIT_LIST_HEAD(&path->tx_zcopy_inflight);
	INIT_DELAYED_WORK(&path->tx_poll_work, tbv_path_tx_poll_work);
	INIT_DELAYED_WORK(&path->rx_supp_poll_work,
			  tbv_path_rx_supp_poll_work);
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
		if (packet && !canceled) {
			if (packet->control) {
				u64 age_ms = packet->queued_jiffies ?
					jiffies_to_msecs(jiffies -
							 packet->queued_jiffies) : 0;

				atomic64_inc(&path->control_tx_completed);
				tbv_path_atomic64_max_ms(
					&path->control_tx_queue_max_ms, age_ms);
			} else {
				atomic64_inc(&path->data_tx_completed);
			}
		}
	if (packet) {
		tbv_path_finish_raw_stream_if_needed(path, packet);
		tbv_path_tx_packet_release(packet,
					   canceled ? -ECANCELED : 0);
	}

	atomic_dec(&path->tx_inflight);
	tbv_path_schedule_tx(path);
}

static void tbv_path_tx_poll_work(struct work_struct *work)
{
	struct tbv_path *path = container_of(to_delayed_work(work),
					     struct tbv_path, tx_poll_work);
	struct tb_ring *ring = READ_ONCE(path->tx_ring);
	struct ring_frame *frame;
	u64 completed = 0;

	if (!ring)
		return;

	atomic64_inc(&path->tx_poll_calls);
	while ((frame = tb_ring_poll(ring))) {
		if (frame->callback)
			frame->callback(ring, frame, false);
		completed++;
	}
	if (completed)
		atomic64_add(completed, &path->tx_poll_completed);

	if (atomic_read(&path->tx_inflight) > 0 || completed)
		tbv_path_queue_tx_poll(path,
				       msecs_to_jiffies(TBV_TX_POLL_DELAY_MS));
}

static void tbv_path_rx_supp_poll_work(struct work_struct *work)
{
	struct tbv_path *path = container_of(to_delayed_work(work),
					     struct tbv_path,
					     rx_supp_poll_work);
	struct tb_ring *ring = READ_ONCE(path->rx_ring);
	struct ring_frame *frame;
	u64 completed = 0;

	if (!ring)
		return;

	atomic64_inc(&path->rx_supp_poll_calls);
	while ((frame = tb_ring_poll(ring))) {
		if (frame->callback)
			frame->callback(ring, frame, false);
		completed++;
	}
	if (completed)
		atomic64_add(completed, &path->rx_supp_poll_completed);

	if (completed || time_before(jiffies,
				     READ_ONCE(path->rx_supp_poll_until)))
		tbv_path_queue_delayed_work(
			path, &path->rx_supp_poll_work,
			msecs_to_jiffies(TBV_RX_SUPP_POLL_DELAY_MS));
}

static int tbv_path_post_rx_frame(struct tbv_data_frame *f);

static void tbv_path_rx_start_raw(struct tbv_path *path,
				  const struct tbv_native_data_header *hdr)
{
	path->rx_raw_opcode = hdr->opcode;
	path->rx_raw_flags = hdr->flags;
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
	struct tbv_native_data_header stream = {};
	struct tbv_native_data_header hdr = {};
	int ret;

	if (!path->rx_raw_pending || !len || len > path->rx_raw_remaining) {
		if (state)
			atomic64_inc(&state->data_rx_bad_frame);
		path->rx_raw_pending = false;
		path->rx_raw_remaining = 0;
		return;
	}

	stream.opcode = path->rx_raw_opcode;
	stream.flags = path->rx_raw_flags;
	stream.dest_qp = path->rx_raw_dest_qp;
	stream.src_qp = path->rx_raw_src_qp;
	stream.psn = path->rx_raw_psn;
	stream.length = path->rx_raw_done + path->rx_raw_remaining;
	stream.imm_data = path->rx_raw_imm_data;
	stream.remote_addr = path->rx_raw_base;
	stream.rkey = path->rx_raw_rkey;

	ret = tbv_native_data_raw_payload_header(&stream, path->rx_raw_done,
						 path->rx_raw_remaining, len,
						 &hdr);
	if (ret) {
		if (state)
			atomic64_inc(&state->data_rx_bad_frame);
		path->rx_raw_pending = false;
		path->rx_raw_remaining = 0;
		return;
	}

	if (len == path->rx_raw_remaining)
		path->rx_raw_pending = false;
	path->rx_raw_done += len;
	path->rx_raw_remaining -= len;
	if (state)
		tbv_ibdev_rx_native_frame(state, path, &hdr, payload);
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
	unsigned long flags;

	spin_lock_irqsave(&path->tx_lock, flags);
	if (packet->inflight) {
		list_del_init(&packet->node);
		packet->inflight = false;
	}
	spin_unlock_irqrestore(&path->tx_lock, flags);

	if (state) {
		if (canceled)
			atomic64_inc(&state->data_tx_canceled);
		else
			atomic64_inc(&state->data_tx_completed);
	}
	if (!canceled)
		atomic64_inc(&path->data_tx_completed);

	tbv_path_finish_raw_stream_if_needed(path, packet);
	tbv_path_tx_packet_release(packet, canceled ? -ECANCELED : 0);
	atomic_dec(&path->tx_inflight);
	tbv_path_schedule_tx(path);
}

static void tbv_path_rx_complete(struct tb_ring *ring, struct ring_frame *frame,
				 bool canceled)
{
	struct tbv_data_frame *f = container_of(frame, struct tbv_data_frame,
						frame);
	struct tbv_path *path = f->path;
	struct tbv_state *state = tbv_path_state(path);
	u32 len = tbv_frame_len(frame);
	u32 return_rx_credits = 0;
	u32 add_remote_credits = 0;
	bool was_raw_payload;

	if (canceled)
		return;
	if (state)
		atomic64_inc(&state->data_rx_completed);
	atomic64_inc(&path->data_rx_completed);

	dma_sync_single_for_cpu(tb_ring_dma_device(ring), f->dma,
				TBV_DATA_FRAME_SIZE, DMA_FROM_DEVICE);
	was_raw_payload = path->rx_raw_pending;
	if (len <= TBV_DATA_FRAME_SIZE && state) {
		struct tbv_native_data_header hdr;
		int ret;

		if (path->rail && path->rail->peer &&
		    path->rail->peer->backend == TBV_BACKEND_APPLE) {
			tbv_ibdev_rx_apple_frame(state, path, f->buf, len,
						 frame->sof, frame->eof);
		} else if (was_raw_payload) {
			return_rx_credits = 1;
			tbv_path_rx_raw_payload(path, state, f->buf, len);
		} else {
			ret = tbv_native_data_parse_header(f->buf, len, &hdr);
			if (!ret && hdr.opcode == TBV_NATIVE_DATA_OP_PATH_CREDIT) {
				if (tbv_native_data_valid_path_credit(&hdr))
					add_remote_credits = hdr.imm_data;
				else
					atomic64_inc(&state->data_rx_bad_header);
			} else if (!ret &&
				   (hdr.flags & TBV_NATIVE_DATA_F_RAW_STREAM)) {
				if (len != TBV_NATIVE_DATA_HDR_SIZE ||
				    !hdr.length ||
				    (hdr.flags & ~(TBV_NATIVE_DATA_F_LAST |
						   TBV_NATIVE_DATA_F_SOLICITED |
						   TBV_NATIVE_DATA_F_RAW_STREAM))) {
					atomic64_inc(&state->data_rx_bad_frame);
				} else {
					return_rx_credits = 1;
					tbv_path_rx_start_raw(path, &hdr);
				}
			} else {
				if (!ret &&
				    tbv_native_data_consumes_rx_credit(hdr.opcode))
					return_rx_credits = 1;
				tbv_ibdev_rx_frame(state, path, f->buf, len);
			}
		}
	} else if (state) {
		atomic64_inc(&state->data_rx_bad_frame);
	}

	if (path->state == TBV_PATH_RING_STARTED ||
	    path->state == TBV_PATH_TUNNEL_ENABLED) {
		int ret = tbv_path_post_rx_frame(f);

		if (ret) {
			pr_warn_ratelimited("RX repost failed ret=%d\n", ret);
			if (state)
				atomic64_inc(&state->data_rx_repost_failed);
			atomic64_inc(&path->data_rx_repost_failed);
		} else {
			if (return_rx_credits)
				tbv_path_return_rx_data_credit(path,
							       return_rx_credits);
			if (add_remote_credits)
				tbv_path_add_remote_rx_credits(path,
							       add_remote_credits);
		}
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
	int ret = -ENOMEM;

	if (!tbv_dma_device_ready(dma_dev)) {
		pr_warn_ratelimited("%s ring DMA device is not ready for mapping\n",
				    tx ? "TX" : "RX");
		return -EPROBE_DEFER;
	}

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
			ret = -EIO;
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
	return ret;
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
	if (!tbv_dma_device_ready(dma_dev)) {
		pr_warn_ratelimited("%s ring DMA device is not ready for unmapping\n",
				    tx ? "TX" : "RX");
		dma_dev = NULL;
	}
	for (i = 0; i < count; i++) {
		struct tbv_data_frame *f = &frames[i];

		if (!f->buf)
			continue;
		if (dma_dev)
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
	int ret;

	if (path->state != TBV_PATH_NEW && path->state != TBV_PATH_STOPPED)
		return -EBUSY;

	tx_hop = path->cfg.tx_hop;
	rx_hop = path->cfg.rx_hop;
	if (requested_transmit_path < 0)
		requested_transmit_path = path->cfg.transmit_path;

	if (path->cfg.receive_path >= 0) {
		ret = tb_xdomain_alloc_in_hopid(xd, path->cfg.receive_path);
		if (ret != path->cfg.receive_path) {
			if (ret >= 0)
				tb_xdomain_release_in_hopid(xd, ret);
			return ret < 0 ? ret : -EBUSY;
		}
		path->remote_transmit_path = ret;
	}

	path->tx_ring = tb_ring_alloc_tx(xd->tb->nhi, tx_hop,
					 path->cfg.tx_ring_size,
					 path->cfg.tx_flags);
	if (!path->tx_ring) {
		ret = -ENOMEM;
		goto err_in_hop;
	}

	transmit_path = tb_xdomain_alloc_out_hopid(xd,
						   requested_transmit_path);
	if (transmit_path < 0) {
		ret = transmit_path;
		goto err_tx_ring;
	}
	path->local_transmit_path = transmit_path;
	path->local_tx_hop = path->tx_ring->hop;

	if (path->cfg.e2e)
		e2e_tx_hop = path->tx_ring->hop;

	path->tx_poll_enabled = tbv_path_native_progress_enabled(path);
	path->rx_supp_poll_enabled = path->tx_poll_enabled;
	path->rx_ring = tb_ring_alloc_rx(xd->tb->nhi, rx_hop,
					 path->cfg.rx_ring_size,
					 path->cfg.rx_flags, e2e_tx_hop,
					 path->cfg.sof_mask,
					 path->cfg.eof_mask,
					 NULL, NULL);
	if (!path->rx_ring) {
		ret = -ENOMEM;
		goto err_out_hop;
	}
	path->local_rx_hop = path->rx_ring->hop;

	ret = tbv_path_configure_ring_throttling(path);
	if (ret)
		goto err_rx_ring;
	ret = tbv_path_alloc_frames(path, true);
	if (ret)
		goto err_rx_ring;
	ret = tbv_path_alloc_frames(path, false);
	if (ret)
		goto err_tx_frames;
	ret = tbv_path_alloc_control_packets(path);
	if (ret)
		goto err_rx_frames;
	ret = tbv_path_alloc_data_packets(path);
	if (ret)
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
err_out_hop:
	tb_xdomain_release_out_hopid(xd, path->local_transmit_path);
	path->local_transmit_path = -1;
err_tx_ring:
	tb_ring_free(path->tx_ring);
	path->tx_ring = NULL;
	path->local_tx_hop = -1;
err_in_hop:
	if (path->remote_transmit_path >= 0) {
		tb_xdomain_release_in_hopid(xd, path->remote_transmit_path);
		path->remote_transmit_path = -1;
	}
	return ret;
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
	bool in_hop_allocated = false;
	int ret;

	if (path->state != TBV_PATH_RING_STARTED)
		return -EINVAL;

	if (path->remote_transmit_path >= 0) {
		if (path->remote_transmit_path != remote_transmit_path)
			return -EBUSY;
	} else {
		ret = tb_xdomain_alloc_in_hopid(xd, remote_transmit_path);
		if (ret != remote_transmit_path) {
			if (ret >= 0)
				tb_xdomain_release_in_hopid(xd, ret);
			return ret < 0 ? ret : -EBUSY;
		}
		path->remote_transmit_path = ret;
		in_hop_allocated = true;
	}

	ret = tb_xdomain_enable_paths(xd, path->local_transmit_path,
				      path->local_tx_hop,
				      remote_transmit_path,
				      path->local_rx_hop);
	if (ret) {
		tb_xdomain_release_in_hopid(xd, path->remote_transmit_path);
		path->remote_transmit_path = -1;
		return ret;
	}

	if (!in_hop_allocated)
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
	packet->owner_ctx = done_ctx;
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
	packet->owner_ctx = done_ctx;
	packet->sof = TBV_DATA_PDF_FRAME_START;
	packet->eof = TBV_DATA_PDF_FRAME_END;
	packet->control = false;
	packet->queued = false;
	packet->zcopy = false;
	packet->unmap_dma = false;
	packet->raw_stream_start = false;
	packet->raw_stream_end = false;
	packet->raw_stream_counted = false;
	packet->start_credit_group_frames = 0;
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
	packet->owner_ctx = done_ctx;
	packet->sof = TBV_DATA_PDF_FRAME_START;
	packet->eof = TBV_DATA_PDF_FRAME_END;
	packet->zcopy = true;
	packet->unmap_dma = unmap_dma;
	packet->raw_stream_counted = false;
	return packet;
}

static int tbv_path_enqueue_control(struct tbv_path *path, const void *data,
				    u32 len, tbv_path_tx_done_fn done,
				    void *done_ctx)
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
	packet->done = done;
	packet->done_ctx = done_ctx;
	packet->owner_ctx = done_ctx;
	packet->sof = TBV_DATA_PDF_FRAME_START;
	packet->eof = TBV_DATA_PDF_FRAME_END;
	packet->pooled = pooled;
	packet->queued_jiffies = jiffies;
	packet->queued = true;
	memcpy(packet->buf, data, len);
	list_add_tail(&packet->node, &path->tx_control_queue);
	path->tx_control_queued++;
	atomic64_inc(&path->control_tx_enqueued);
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

	packet->start_credit_group_frames = 1;
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
	bool first = true;
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
		packet->start_credit_group_frames = first ? count : 0;
		packet->queued = true;
		path->tx_data_queued++;
		atomic64_inc(&path->data_tx_enqueued);
		first = false;
	}
	list_splice_tail_init(packets, &path->tx_data_queue);
	spin_unlock_irqrestore(&path->tx_lock, flags);

	if (!defer_schedule)
		tbv_path_schedule_tx(path);
	return 0;
}

static int tbv_path_enqueue_reserved_data_list(struct tbv_path *path,
					       struct list_head *packets,
					       u32 count, bool defer_schedule)
{
	struct tbv_tx_packet *packet;
	unsigned long flags;
	bool first = true;

	if (!count)
		return 0;

	spin_lock_irqsave(&path->tx_lock, flags);
	if (path->state != TBV_PATH_TUNNEL_ENABLED) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return -ENOTCONN;
	}
	if (path->tx_data_reserved < count) {
		spin_unlock_irqrestore(&path->tx_lock, flags);
		return -ENOMEM;
	}

	path->tx_data_reserved -= count;
	list_for_each_entry(packet, packets, node) {
		packet->start_credit_group_frames = first ? count : 0;
		packet->queued = true;
		path->tx_data_queued++;
		atomic64_inc(&path->data_tx_enqueued);
		first = false;
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
		bool old_raw_stream_active;
		bool old_raw_stream_end_seen;
		void *old_raw_stream_owner;
		u32 old_raw_stream_inflight;
		bool charged_data_credit;
		bool from_control_queue;
		u32 old_start_credit_group_frames;
		int ret;

		spin_lock_irqsave(&path->tx_lock, flags);
		if (path->state != TBV_PATH_TUNNEL_ENABLED ||
		    (list_empty(&path->tx_control_queue) &&
		     list_empty(&path->tx_data_queue))) {
			path->tx_scheduling = false;
			spin_unlock_irqrestore(&path->tx_lock, flags);
			return;
		}

		if (path->tx_raw_stream_active) {
			if (list_empty(&path->tx_data_queue)) {
				path->tx_scheduling = false;
				spin_unlock_irqrestore(&path->tx_lock, flags);
				return;
			}
			packet = list_first_entry(&path->tx_data_queue,
						  struct tbv_tx_packet, node);
			if (!packet->zcopy || packet->raw_stream_start ||
			    packet->owner_ctx != path->tx_raw_stream_owner) {
				path->tx_scheduling = false;
				spin_unlock_irqrestore(&path->tx_lock, flags);
				return;
			}
			from_control_queue = false;
			needs_staging = false;
		} else if (!list_empty(&path->tx_control_queue)) {
			packet = list_first_entry(&path->tx_control_queue,
						  struct tbv_tx_packet, node);
			from_control_queue = true;
			needs_staging = true;
		} else {
			if (list_empty(&path->tx_data_queue)) {
				path->tx_scheduling = false;
				spin_unlock_irqrestore(&path->tx_lock, flags);
				return;
			}
			packet = list_first_entry(&path->tx_data_queue,
						  struct tbv_tx_packet, node);
			from_control_queue = false;
			needs_staging = !packet->zcopy;
		}

		if (!from_control_queue &&
		    atomic_read(&path->tx_inflight) >=
			    TBV_DATA_TX_MAX_INFLIGHT) {
			path->tx_scheduling = false;
			spin_unlock_irqrestore(&path->tx_lock, flags);
			return;
		}

		if (needs_staging && list_empty(&path->tx_free)) {
			path->tx_scheduling = false;
			spin_unlock_irqrestore(&path->tx_lock, flags);
			return;
		}
		if (needs_staging && !from_control_queue) {
			u32 reserve = tbv_path_tx_control_frame_reserve(path);
			u32 inflight = atomic_read(&path->tx_inflight);
			u32 available = path->tx_frame_count > inflight ?
					path->tx_frame_count - inflight : 0;

			if (available <= reserve) {
				path->tx_scheduling = false;
				spin_unlock_irqrestore(&path->tx_lock, flags);
				return;
			}
		}

		charged_data_credit = false;
		old_start_credit_group_frames = packet->start_credit_group_frames;
		if (!packet->control && path->tx_remote_data_credit_max) {
			u32 start_credit_required =
				tbv_native_data_start_credit_required(
					packet->start_credit_group_frames,
					path->tx_remote_data_credit_max);

			if (path->tx_remote_data_credits <
			    max_t(u32, 1, start_credit_required)) {
				if (state)
					atomic64_inc(&state->data_tx_credit_stalls);
				atomic64_inc(&path->data_tx_credit_stalls);
				path->tx_scheduling = false;
				spin_unlock_irqrestore(&path->tx_lock, flags);
				return;
			}
			path->tx_remote_data_credits--;
			charged_data_credit = true;
		}
		packet->start_credit_group_frames = 0;

		if (from_control_queue)
			path->tx_control_queued--;
		else
			path->tx_data_queued--;
		list_del_init(&packet->node);
		packet->queued = false;
		old_raw_stream_active = path->tx_raw_stream_active;
		old_raw_stream_owner = path->tx_raw_stream_owner;
		old_raw_stream_end_seen = path->tx_raw_stream_end_seen;
		old_raw_stream_inflight = path->tx_raw_stream_inflight;
		tbv_path_count_raw_stream_locked(path, packet);

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

			spin_lock_irqsave(&path->tx_lock, flags);
			list_add_tail(&packet->node, &path->tx_zcopy_inflight);
			packet->inflight = true;
			spin_unlock_irqrestore(&path->tx_lock, flags);

			ret = tb_ring_tx(path->tx_ring, &packet->frame);
			if (!ret) {
				if (state)
					atomic64_inc(&state->data_tx_posted);
				atomic64_inc(&path->data_tx_posted);
				tbv_path_queue_tx_poll(path, 0);
				tbv_path_queue_rx_supp_poll(
					path,
					msecs_to_jiffies(
						TBV_RX_SUPP_POLL_DELAY_MS));
				continue;
			}

			if (state)
				atomic64_inc(&state->data_tx_errors);
			spin_lock_irqsave(&path->tx_lock, flags);
			if (packet->inflight) {
				list_del_init(&packet->node);
				packet->inflight = false;
			}
			path->tx_raw_stream_active = old_raw_stream_active;
			path->tx_raw_stream_owner = old_raw_stream_owner;
			path->tx_raw_stream_end_seen = old_raw_stream_end_seen;
			path->tx_raw_stream_inflight = old_raw_stream_inflight;
			packet->raw_stream_counted = false;
			if (charged_data_credit) {
				if (path->tx_remote_data_credits <
				    path->tx_remote_data_credit_max)
					path->tx_remote_data_credits++;
			}
			if (ret == -ENOMEM &&
			    path->state == TBV_PATH_TUNNEL_ENABLED &&
			    (packet->done || packet->owner_ctx)) {
				list_add(&packet->node, &path->tx_data_queue);
				path->tx_data_queued++;
				packet->queued = true;
				packet->start_credit_group_frames =
					old_start_credit_group_frames;
				path->tx_scheduling = false;
				spin_unlock_irqrestore(&path->tx_lock, flags);
				atomic_dec(&path->tx_inflight);
				return;
			}
			spin_unlock_irqrestore(&path->tx_lock, flags);
			atomic_dec(&path->tx_inflight);
			tbv_path_finish_raw_stream_if_needed(path, packet);
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
			if (packet->control)
				atomic64_inc(&path->control_tx_posted);
			else
				atomic64_inc(&path->data_tx_posted);
			tbv_path_queue_tx_poll(path, 0);
			tbv_path_queue_rx_supp_poll(
				path,
				msecs_to_jiffies(TBV_RX_SUPP_POLL_DELAY_MS));
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
		path->tx_raw_stream_active = old_raw_stream_active;
		path->tx_raw_stream_owner = old_raw_stream_owner;
		path->tx_raw_stream_end_seen = old_raw_stream_end_seen;
		path->tx_raw_stream_inflight = old_raw_stream_inflight;
		packet->raw_stream_counted = false;
		if (charged_data_credit) {
			if (path->tx_remote_data_credits <
			    path->tx_remote_data_credit_max)
				path->tx_remote_data_credits++;
		}
		if (ret == -ENOMEM &&
		    path->state == TBV_PATH_TUNNEL_ENABLED &&
		    (packet->control || packet->done || packet->owner_ctx)) {
			if (packet->control) {
				list_add(&packet->node, &path->tx_control_queue);
				path->tx_control_queued++;
			} else {
				list_add(&packet->node, &path->tx_data_queue);
				path->tx_data_queued++;
				packet->start_credit_group_frames =
					old_start_credit_group_frames;
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
		return tbv_path_enqueue_control(path, data, len, done, done_ctx);
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

static void tbv_path_release_packet_list_silent(struct list_head *packets,
						int status)
{
	while (!list_empty(packets)) {
		struct tbv_tx_packet *packet =
			list_first_entry(packets, struct tbv_tx_packet, node);

		list_del_init(&packet->node);
		packet->done = NULL;
		packet->done_ctx = NULL;
		tbv_path_tx_packet_release(packet, status);
	}
}

void tbv_path_release_prepared_list_silent(struct list_head *packets,
					   int status)
{
	tbv_path_release_packet_list_silent(packets, status);
}

struct tbv_path_cancel_done {
	tbv_path_tx_done_fn done;
	void *ctx;
};

static void tbv_path_cancel_record_done(struct tbv_path_cancel_done *done,
					u32 *done_count, u32 done_max,
					struct tbv_tx_packet *packet)
{
	if (!packet || !packet->done)
		return;
	if (*done_count >= done_max)
		return;

	done[*done_count].done = packet->done;
	done[*done_count].ctx = packet->done_ctx;
	(*done_count)++;
	packet->done = NULL;
	packet->done_ctx = NULL;
	packet->owner_ctx = NULL;
}

static void tbv_path_release_owned_frame_list(struct list_head *frames)
{
	while (!list_empty(frames)) {
		struct tbv_path_owned_frame *frame =
			list_first_entry(frames, struct tbv_path_owned_frame,
					 node);

		list_del_init(&frame->node);
		kfree(frame->data);
		kfree(frame);
	}
}

int tbv_path_prepare_owned_list(struct tbv_path *path,
				struct list_head *frames,
				struct list_head *packets,
				u32 *packet_count_out,
				unsigned int send_flags,
				tbv_path_tx_done_fn done,
				void *done_ctx)
{
	struct tbv_path_owned_frame *owned;
	struct tbv_tx_packet *packet;
	LIST_HEAD(prepared);
	u32 packet_count = 0;
	int ret;

	if (!path || !frames || !packets || !packet_count_out)
		return -EINVAL;
	*packet_count_out = 0;
	if (send_flags & ~(TBV_PATH_SEND_DEFER)) {
		tbv_path_release_owned_frame_list(frames);
		return -EINVAL;
	}

	while (!list_empty(frames)) {
		owned = list_first_entry(frames, struct tbv_path_owned_frame,
					 node);
		list_del_init(&owned->node);

		if (!owned->data || !owned->len ||
		    owned->len > TBV_DATA_FRAME_SIZE) {
			kfree(owned->data);
			kfree(owned);
			ret = -EINVAL;
			goto err_release;
		}

		packet = tbv_path_alloc_data_packet_owned(path, owned->data,
							  owned->len, done,
							  done_ctx);
		if (!packet) {
			kfree(owned->data);
			kfree(owned);
			ret = -ENOMEM;
			goto err_release;
		}
		owned->data = NULL;
		packet->sof = owned->sof;
		packet->eof = owned->eof;
		list_add_tail(&packet->node, &prepared);
		packet_count++;
		kfree(owned);
	}

	list_splice_tail_init(&prepared, packets);
	*packet_count_out = packet_count;
	return 0;

err_release:
	tbv_path_release_packet_list_silent(&prepared, ret);
	tbv_path_release_owned_frame_list(frames);
	return ret;
}

int tbv_path_enqueue_prepared_reserved(struct tbv_path *path,
				       struct list_head *packets,
				       u32 packet_count,
				       unsigned int send_flags)
{
	int ret;

	if (!path || !packets)
		return -EINVAL;
	if (send_flags & ~(TBV_PATH_SEND_DEFER)) {
		tbv_path_release_packet_list_silent(packets, -EINVAL);
		return -EINVAL;
	}

	ret = tbv_path_enqueue_reserved_data_list(
		path, packets, packet_count, send_flags & TBV_PATH_SEND_DEFER);
	if (ret)
		tbv_path_release_packet_list_silent(packets, ret);
	return ret;
}

int tbv_path_send_owned_list_reserved(struct tbv_path *path,
				      struct list_head *frames,
				      unsigned int send_flags,
				      tbv_path_tx_done_fn done,
				      void *done_ctx)
{
	LIST_HEAD(packets);
	u32 packet_count;
	int ret;

	ret = tbv_path_prepare_owned_list(path, frames, &packets,
					  &packet_count, send_flags, done,
					  done_ctx);
	if (ret)
		return ret;

	return tbv_path_enqueue_prepared_reserved(path, &packets,
						  packet_count, send_flags);
}

int tbv_path_send_page_stream(struct tbv_path *path,
			      const struct tbv_native_data_header *hdr,
			      u32 total_length, unsigned int send_flags,
			      tbv_path_tx_done_fn meta_done,
			      void *meta_done_ctx,
			      tbv_path_next_page_fn next, void *next_ctx)
{
	struct device *dma_dev;
	LIST_HEAD(packets);
	u32 prepared = 0;
	u32 packet_count = 0;
	struct tbv_tx_packet *packet;
	u32 max_raw_payload;
	struct tbv_native_data_header stream_hdr;
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
	if (hdr->opcode == TBV_NATIVE_DATA_OP_RDMA_WRITE ||
	    hdr->opcode == TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM)
		max_raw_payload = TBV_DATA_FRAME_SIZE;
	else
		max_raw_payload = TBV_NATIVE_DATA_MAX_PAYLOAD;

	dma_dev = tb_ring_dma_device(path->tx_ring);
	if (!tbv_dma_device_ready(dma_dev)) {
		ret = -EPROBE_DEFER;
		goto err_meta_done;
	}

	hdr_buf = kzalloc(TBV_NATIVE_DATA_HDR_SIZE, GFP_KERNEL);
	if (!hdr_buf) {
		ret = -ENOMEM;
		goto err_release;
	}

	stream_hdr = *hdr;
	stream_hdr.length = total_length;
	stream_hdr.flags |= TBV_NATIVE_DATA_F_LAST |
			    TBV_NATIVE_DATA_F_RAW_STREAM;
	ret = tbv_native_data_build_header(hdr_buf, TBV_NATIVE_DATA_HDR_SIZE,
					   &stream_hdr);
	if (ret < 0) {
		kfree(hdr_buf);
		goto err_release;
	}

	packet = tbv_path_alloc_data_packet_owned(path, hdr_buf,
						  TBV_NATIVE_DATA_HDR_SIZE,
						  meta_done, meta_done_ctx);
	if (!packet) {
		kfree(hdr_buf);
		ret = -ENOMEM;
		goto err_release;
	}
	packet->raw_stream_start = true;
	list_add_tail(&packet->node, &packets);
	packet_count++;

	while (prepared < total_length) {
		tbv_path_tx_done_fn done = NULL;
		struct page *page = NULL;
		void *done_ctx = NULL;
		bool last;
		dma_addr_t dma;
		u32 page_off = 0;
		u32 len = 0;

		ret = next(next_ctx, &page, &page_off, &len, &done, &done_ctx);
		if (ret)
			goto err_release;
		if (!page || !len || len > max_raw_payload ||
		    len > total_length - prepared ||
		    page_off > PAGE_SIZE || len > PAGE_SIZE - page_off) {
			if (done)
				done(done_ctx, -EINVAL);
			ret = -EINVAL;
			goto err_release;
		}

		last = prepared + len == total_length;

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
		packet->owner_ctx = meta_done_ctx ? meta_done_ctx : done_ctx;
		packet->raw_stream_end = last;
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
	if (!packet_count && meta_done)
		meta_done(meta_done_ctx, ret);
	tbv_path_release_packet_list(&packets, ret);
	return ret;

err_meta_done:
	if (meta_done)
		meta_done(meta_done_ctx, ret);
	return ret;
}

static bool tbv_path_packet_matches(const struct tbv_tx_packet *packet,
				    tbv_path_tx_done_fn done, void *done_ctx,
				    void *owner_ctx)
{
	if (owner_ctx && packet->owner_ctx == owner_ctx)
		return true;
	return done && done_ctx && packet->done == done &&
	       packet->done_ctx == done_ctx;
}

static void tbv_path_cancel_data_match(struct tbv_path *path,
				       tbv_path_tx_done_fn done, void *done_ctx,
				       void *owner_ctx)
{
	struct tbv_tx_packet *packet;
	struct tbv_tx_packet *tmp;
	struct tbv_path_cancel_done *done_list;
	LIST_HEAD(cancel);
	unsigned long flags;
	u32 i;
	u32 done_count = 0;
	u32 done_max;
	bool raw_stream_canceled = false;

	if ((!done || !done_ctx) && !owner_ctx)
		return;

	done_max = max_t(u32, path->tx_frame_count, 1);
	done_list = kvcalloc(done_max, sizeof(*done_list), GFP_KERNEL);
	if (!done_list)
		return;

	spin_lock_irqsave(&path->tx_lock, flags);
	if (owner_ctx && path->tx_raw_stream_active &&
	    path->tx_raw_stream_owner == owner_ctx) {
		path->tx_raw_stream_active = false;
		path->tx_raw_stream_owner = NULL;
		path->tx_raw_stream_end_seen = false;
		path->tx_raw_stream_inflight = 0;
		raw_stream_canceled = true;
	}
	list_for_each_entry_safe(packet, tmp, &path->tx_data_queue, node) {
		if (!tbv_path_packet_matches(packet, done, done_ctx,
					     owner_ctx))
			continue;
		list_del_init(&packet->node);
		packet->queued = false;
			if (path->tx_data_queued)
				path->tx_data_queued--;
			packet->owner_ctx = NULL;
			list_add_tail(&packet->node, &cancel);
	}

	for (i = 0; i < path->tx_frame_count; i++) {
		struct tbv_data_frame *f = &path->tx_frames[i];

		packet = f->packet;
		if (!packet || packet->control ||
		    !tbv_path_packet_matches(packet, done, done_ctx,
					     owner_ctx))
			continue;
		tbv_path_cancel_record_done(done_list, &done_count, done_max,
					    packet);
		f->done = NULL;
		f->done_ctx = NULL;
	}

	list_for_each_entry_safe(packet, tmp, &path->tx_zcopy_inflight, node) {
		if (!tbv_path_packet_matches(packet, done, done_ctx,
					     owner_ctx))
			continue;
		list_del_init(&packet->node);
		packet->inflight = false;
		tbv_path_cancel_record_done(done_list, &done_count, done_max,
					    packet);
	}
	spin_unlock_irqrestore(&path->tx_lock, flags);

	while (!list_empty(&cancel)) {
		packet = list_first_entry(&cancel, struct tbv_tx_packet, node);
		list_del_init(&packet->node);
		tbv_path_tx_packet_release(packet, -ECANCELED);
	}

	for (i = 0; i < done_count; i++)
		done_list[i].done(done_list[i].ctx, -ECANCELED);

	kvfree(done_list);

	if (raw_stream_canceled)
		tbv_path_schedule_tx(path);
}

void tbv_path_cancel_data_done_ctx(struct tbv_path *path,
				   tbv_path_tx_done_fn done, void *done_ctx)
{
	tbv_path_cancel_data_match(path, done, done_ctx, NULL);
}

void tbv_path_cancel_data_owner_ctx(struct tbv_path *path, void *owner_ctx)
{
	tbv_path_cancel_data_match(path, NULL, NULL, owner_ctx);
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
	path->tx_raw_stream_active = false;
	path->tx_raw_stream_owner = NULL;
	path->tx_raw_stream_end_seen = false;
	path->tx_raw_stream_inflight = 0;
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
	bool tunnel_enabled = path->state == TBV_PATH_TUNNEL_ENABLED;
	bool rings_started = tunnel_enabled ||
			     path->state == TBV_PATH_RING_STARTED;

	if (rings_started) {
		if (path->rx_ring)
			tb_ring_stop(path->rx_ring);
		if (path->tx_ring)
			tb_ring_stop(path->tx_ring);
		path->state = TBV_PATH_RING_ALLOCATED;
	}
	cancel_delayed_work_sync(&path->tx_poll_work);
	cancel_delayed_work_sync(&path->rx_supp_poll_work);

	if (tunnel_enabled) {
		tb_xdomain_disable_paths(xd, path->local_transmit_path,
					 path->local_tx_hop,
					 path->remote_transmit_path,
					 path->local_rx_hop);
		tb_xdomain_release_in_hopid(xd, path->remote_transmit_path);
		path->remote_transmit_path = -1;
		path->state = TBV_PATH_RING_ALLOCATED;
	}
	if (!tunnel_enabled && path->remote_transmit_path >= 0) {
		tb_xdomain_release_in_hopid(xd, path->remote_transmit_path);
		path->remote_transmit_path = -1;
	}

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
