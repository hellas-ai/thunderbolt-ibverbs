// SPDX-License-Identifier: GPL-2.0
/*
 * Experimental raw-NHI ring adapter.
 *
 * This intentionally still allocates/starts rings through the in-tree
 * thunderbolt core. We only bypass the exported enqueue/poll helpers for
 * frames that can be posted immediately, using the public tb_ring/tb_nhi
 * state plus the descriptor/register layout from drivers/thunderbolt/nhi.c
 * and nhi_regs.h. That keeps hop ownership, IRQ setup, suspend teardown, and
 * E2E programming owned by thunderbolt while letting usb4_rdma A/B the direct
 * descriptor path.
 */

#define pr_fmt(fmt) "usb4_rdma/nhi_raw: " fmt

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/thunderbolt.h>

#include "nhi_raw.h"

/* The drivers/thunderbolt private headers are not exported to external
 * modules. Keep the small subset we need local and assert the layout. */
#define U4_NHI_REG_TX_RING_BASE		0x00000
#define U4_NHI_REG_RX_RING_BASE		0x08000
#define U4_NHI_RING_SLOT_SIZE		16

#define U4_NHI_DESC_LENGTH_MASK		GENMASK(11, 0)
#define U4_NHI_DESC_EOF_SHIFT		12
#define U4_NHI_DESC_EOF_MASK		GENMASK(15, 12)
#define U4_NHI_DESC_SOF_SHIFT		16
#define U4_NHI_DESC_SOF_MASK		GENMASK(19, 16)
#define U4_NHI_DESC_FLAGS_SHIFT		20
#define U4_NHI_DESC_FLAGS_MASK		GENMASK(31, 20)

struct u4_nhi_ring_desc {
	u64 phys;
	u32 meta;
	u32 time;
} __packed;

static_assert(sizeof(struct u4_nhi_ring_desc) == 16);

static bool raw_nhi;
module_param(raw_nhi, bool, 0444);
MODULE_PARM_DESC(raw_nhi,
		 "bypass tb_ring enqueue/poll helpers and write NHI descriptors directly when possible (experimental, default: false)");

static bool raw_nhi_desc_irq = true;
module_param(raw_nhi_desc_irq, bool, 0444);
MODULE_PARM_DESC(raw_nhi_desc_irq,
		 "set RING_DESC_INTERRUPT on raw-NHI descriptors (default true; disable only when a poller drains completions)");

static bool raw_nhi_count_empty = true;
module_param(raw_nhi_count_empty, bool, 0644);
MODULE_PARM_DESC(raw_nhi_count_empty,
		 "count empty raw-NHI polls in debug stats (default true; disable for tight busy-poll benchmarks)");

static atomic64_t raw_tx_posted;
static atomic64_t raw_rx_posted;
static atomic64_t raw_tx_fallback;
static atomic64_t raw_rx_fallback;
static atomic64_t raw_poll_hit;
static atomic64_t raw_poll_empty;

bool usb4_rdma_nhi_raw_enabled(void)
{
	return READ_ONCE(raw_nhi);
}

static void __iomem *u4_nhi_ring_desc_base(const struct tb_ring *ring)
{
	void __iomem *io = ring->nhi->iobase;

	io += ring->is_tx ? U4_NHI_REG_TX_RING_BASE :
			    U4_NHI_REG_RX_RING_BASE;
	io += ring->hop * U4_NHI_RING_SLOT_SIZE;
	return io;
}

static void u4_nhi_ring_write_index(const struct tb_ring *ring, u16 index)
{
	void __iomem *reg = u4_nhi_ring_desc_base(ring) + 8;

	/* The peer half of this register is read-only, and writes to it are
	 * ignored by the controller. This matches drivers/thunderbolt/nhi.c. */
	if (ring->is_tx)
		iowrite32((u32)index << 16, reg);
	else
		iowrite32(index, reg);
}

static bool u4_nhi_ring_full(const struct tb_ring *ring)
{
	return ((ring->head + 1) % ring->size) == ring->tail;
}

static bool u4_nhi_ring_empty(const struct tb_ring *ring)
{
	return ring->head == ring->tail;
}

static u32 u4_nhi_desc_meta(const struct ring_frame *frame, bool tx)
{
	u32 flags = RING_DESC_POSTED;
	u32 meta;

	if (READ_ONCE(raw_nhi_desc_irq))
		flags |= RING_DESC_INTERRUPT;
	meta = flags << U4_NHI_DESC_FLAGS_SHIFT;

	if (tx) {
		meta |= frame->size & U4_NHI_DESC_LENGTH_MASK;
		meta |= (frame->eof << U4_NHI_DESC_EOF_SHIFT) &
			U4_NHI_DESC_EOF_MASK;
		meta |= (frame->sof << U4_NHI_DESC_SOF_SHIFT) &
			U4_NHI_DESC_SOF_MASK;
	}

	return meta;
}

static u32 u4_nhi_desc_length(u32 meta)
{
	return meta & U4_NHI_DESC_LENGTH_MASK;
}

static u32 u4_nhi_desc_eof(u32 meta)
{
	return (meta & U4_NHI_DESC_EOF_MASK) >> U4_NHI_DESC_EOF_SHIFT;
}

static u32 u4_nhi_desc_sof(u32 meta)
{
	return (meta & U4_NHI_DESC_SOF_MASK) >> U4_NHI_DESC_SOF_SHIFT;
}

static u32 u4_nhi_desc_flags(u32 meta)
{
	return (meta & U4_NHI_DESC_FLAGS_MASK) >> U4_NHI_DESC_FLAGS_SHIFT;
}

static int u4_nhi_raw_enqueue(struct tb_ring *ring, struct ring_frame *frame,
			      bool *fallback, bool allow_fallback)
{
	struct u4_nhi_ring_desc *descs =
		(struct u4_nhi_ring_desc *)ring->descriptors;
	unsigned long flags;
	int ret = 0;

	*fallback = false;
	if (!READ_ONCE(raw_nhi)) {
		if (!allow_fallback)
			return -EOPNOTSUPP;
		*fallback = true;
		return __tb_ring_enqueue(ring, frame);
	}

	spin_lock_irqsave(&ring->lock, flags);
	if (!ring->running) {
		ret = -ESHUTDOWN;
		goto out_unlock;
	}

	/* Preserve tb_ring ordering and unbounded queue semantics. Once the
	 * core helper has queued anything, let it own descriptor progression
	 * until its queue drains. */
	if (u4_nhi_ring_full(ring) || !list_empty(&ring->queue)) {
		ret = -EAGAIN;
		goto out_unlock;
	}

	list_add_tail(&frame->list, &ring->in_flight);
	descs[ring->head].phys = frame->buffer_phy;
	descs[ring->head].meta = u4_nhi_desc_meta(frame, ring->is_tx);
	descs[ring->head].time = 0;

	dma_wmb();
	ring->head = (ring->head + 1) % ring->size;
	u4_nhi_ring_write_index(ring, ring->head);

out_unlock:
	spin_unlock_irqrestore(&ring->lock, flags);

	if (ret == -EAGAIN) {
		*fallback = true;
		if (!allow_fallback)
			return ret;
		return __tb_ring_enqueue(ring, frame);
	}
	return ret;
}

static int u4_nhi_raw_enqueue_batch(struct tb_ring *ring,
				    struct list_head *frames,
				    bool allow_fallback,
				    unsigned int *direct,
				    unsigned int *fallback)
{
	struct u4_nhi_ring_desc *descs =
		(struct u4_nhi_ring_desc *)ring->descriptors;
	struct ring_frame *frame;
	unsigned long flags;
	int ret = 0;

	*direct = 0;
	*fallback = 0;
	if (list_empty(frames))
		return 0;

	if (!READ_ONCE(raw_nhi))
		goto fallback_all;

	spin_lock_irqsave(&ring->lock, flags);
	if (!ring->running) {
		ret = -ESHUTDOWN;
		goto out_unlock;
	}

	while (!list_empty(frames)) {
		if (u4_nhi_ring_full(ring) || !list_empty(&ring->queue))
			break;

		frame = list_first_entry(frames, struct ring_frame, list);
		list_del_init(&frame->list);
		list_add_tail(&frame->list, &ring->in_flight);

		descs[ring->head].phys = frame->buffer_phy;
		descs[ring->head].meta = u4_nhi_desc_meta(frame,
							  ring->is_tx);
		descs[ring->head].time = 0;
		ring->head = (ring->head + 1) % ring->size;
		(*direct)++;
	}

	if (*direct) {
		dma_wmb();
		u4_nhi_ring_write_index(ring, ring->head);
	}

out_unlock:
	spin_unlock_irqrestore(&ring->lock, flags);
	if (ret)
		return ret;
	if (list_empty(frames))
		return 0;
	if (!allow_fallback)
		return -EAGAIN;

fallback_all:
	while (!list_empty(frames)) {
		frame = list_first_entry(frames, struct ring_frame, list);
		list_del_init(&frame->list);
		ret = __tb_ring_enqueue(ring, frame);
		if (ret) {
			list_add(&frame->list, frames);
			return ret;
		}
		(*fallback)++;
	}
	return 0;
}

static int u4_nhi_raw_enqueue_batch_unlocked(struct tb_ring *ring,
					     struct list_head *frames)
{
	struct u4_nhi_ring_desc *descs =
		(struct u4_nhi_ring_desc *)ring->descriptors;
	struct ring_frame *frame;
	bool posted = false;

	if (list_empty(frames))
		return 0;
	if (!READ_ONCE(raw_nhi))
		return -EOPNOTSUPP;
	if (!ring->running)
		return -ESHUTDOWN;
	if (!list_empty(&ring->queue))
		return -EAGAIN;

	while (!list_empty(frames)) {
		if (u4_nhi_ring_full(ring))
			break;

		frame = list_first_entry(frames, struct ring_frame, list);
		list_del_init(&frame->list);
		list_add_tail(&frame->list, &ring->in_flight);

		descs[ring->head].phys = frame->buffer_phy;
		descs[ring->head].meta = u4_nhi_desc_meta(frame,
							  ring->is_tx);
		descs[ring->head].time = 0;
		ring->head = (ring->head + 1) % ring->size;
		posted = true;
	}

	if (posted) {
		dma_wmb();
		u4_nhi_ring_write_index(ring, ring->head);
	}

	return list_empty(frames) ? 0 : -EAGAIN;
}

int usb4_rdma_nhi_raw_tx(struct tb_ring *ring, struct ring_frame *frame)
{
	bool fallback;
	int ret = u4_nhi_raw_enqueue(ring, frame, &fallback, true);

	if (READ_ONCE(raw_nhi)) {
		if (fallback)
			atomic64_inc(&raw_tx_fallback);
		else if (ret)
			atomic64_inc(&raw_tx_fallback);
		else
			atomic64_inc(&raw_tx_posted);
	}
	return ret;
}

int usb4_rdma_nhi_raw_tx_atomic(struct tb_ring *ring, struct ring_frame *frame)
{
	bool fallback;
	int ret = u4_nhi_raw_enqueue(ring, frame, &fallback, false);

	if (READ_ONCE(raw_nhi)) {
		if (ret)
			atomic64_inc(&raw_tx_fallback);
		else
			atomic64_inc(&raw_tx_posted);
	}
	return ret;
}

int usb4_rdma_nhi_raw_tx_batch(struct tb_ring *ring, struct list_head *frames)
{
	unsigned int direct, fallback;
	int ret = u4_nhi_raw_enqueue_batch(ring, frames, true, &direct,
					   &fallback);

	if (READ_ONCE(raw_nhi)) {
		if (direct)
			atomic64_add(direct, &raw_tx_posted);
		if (fallback)
			atomic64_add(fallback, &raw_tx_fallback);
		if (ret)
			atomic64_inc(&raw_tx_fallback);
	}
	return ret;
}

int usb4_rdma_nhi_raw_tx_batch_atomic(struct tb_ring *ring,
				      struct list_head *frames)
{
	unsigned int direct, fallback;
	int ret = u4_nhi_raw_enqueue_batch(ring, frames, false, &direct,
					   &fallback);

	if (READ_ONCE(raw_nhi)) {
		if (direct)
			atomic64_add(direct, &raw_tx_posted);
		if (fallback)
			atomic64_add(fallback, &raw_tx_fallback);
		if (ret && ret != -EAGAIN)
			atomic64_inc(&raw_tx_fallback);
	}
	return ret;
}

int usb4_rdma_nhi_raw_tx_batch_unlocked(struct tb_ring *ring,
					struct list_head *frames)
{
	return u4_nhi_raw_enqueue_batch_unlocked(ring, frames);
}

int usb4_rdma_nhi_raw_rx(struct tb_ring *ring, struct ring_frame *frame)
{
	bool fallback;
	int ret = u4_nhi_raw_enqueue(ring, frame, &fallback, true);

	if (READ_ONCE(raw_nhi)) {
		if (fallback)
			atomic64_inc(&raw_rx_fallback);
		else if (ret)
			atomic64_inc(&raw_rx_fallback);
		else
			atomic64_inc(&raw_rx_posted);
	}
	return ret;
}

int usb4_rdma_nhi_raw_rx_batch(struct tb_ring *ring, struct list_head *frames)
{
	unsigned int direct, fallback;
	int ret = u4_nhi_raw_enqueue_batch(ring, frames, true, &direct,
					   &fallback);

	if (READ_ONCE(raw_nhi)) {
		if (direct)
			atomic64_add(direct, &raw_rx_posted);
		if (fallback)
			atomic64_add(fallback, &raw_rx_fallback);
		if (ret)
			atomic64_inc(&raw_rx_fallback);
	}
	return ret;
}

int usb4_rdma_nhi_raw_rx_batch_atomic(struct tb_ring *ring,
				      struct list_head *frames)
{
	unsigned int direct, fallback;
	int ret = u4_nhi_raw_enqueue_batch(ring, frames, false, &direct,
					   &fallback);

	if (READ_ONCE(raw_nhi)) {
		if (direct)
			atomic64_add(direct, &raw_rx_posted);
		if (fallback)
			atomic64_add(fallback, &raw_rx_fallback);
		if (ret && ret != -EAGAIN)
			atomic64_inc(&raw_rx_fallback);
	}
	return ret;
}

int usb4_rdma_nhi_raw_rx_batch_unlocked(struct tb_ring *ring,
					struct list_head *frames)
{
	return u4_nhi_raw_enqueue_batch_unlocked(ring, frames);
}

struct ring_frame *usb4_rdma_nhi_raw_poll(struct tb_ring *ring)
{
	struct u4_nhi_ring_desc *descs =
		(struct u4_nhi_ring_desc *)ring->descriptors;
	struct ring_frame *frame = NULL;
	unsigned long flags;
	u32 meta;

	if (!READ_ONCE(raw_nhi))
		return tb_ring_poll(ring);

	spin_lock_irqsave(&ring->lock, flags);
	if (!ring->running || u4_nhi_ring_empty(ring))
		goto out_empty;

	meta = READ_ONCE(descs[ring->tail].meta);
	if (!(u4_nhi_desc_flags(meta) & RING_DESC_COMPLETED))
		goto out_empty;

	frame = list_first_entry(&ring->in_flight, typeof(*frame), list);
	list_del_init(&frame->list);

	if (!ring->is_tx) {
		frame->size = u4_nhi_desc_length(meta);
		frame->eof = u4_nhi_desc_eof(meta);
		frame->sof = u4_nhi_desc_sof(meta);
		frame->flags = u4_nhi_desc_flags(meta);
	}

	ring->tail = (ring->tail + 1) % ring->size;
	atomic64_inc(&raw_poll_hit);
	spin_unlock_irqrestore(&ring->lock, flags);
	return frame;

out_empty:
	spin_unlock_irqrestore(&ring->lock, flags);
	if (READ_ONCE(raw_nhi_count_empty))
		atomic64_inc(&raw_poll_empty);
	return NULL;
}

unsigned int usb4_rdma_nhi_raw_poll_batch(struct tb_ring *ring,
					  struct list_head *frames,
					  unsigned int max)
{
	struct u4_nhi_ring_desc *descs =
		(struct u4_nhi_ring_desc *)ring->descriptors;
	struct ring_frame *frame;
	unsigned long flags;
	unsigned int done = 0;
	u32 meta;

	if (!max)
		return 0;
	if (!READ_ONCE(raw_nhi)) {
		while (done < max && (frame = tb_ring_poll(ring))) {
			INIT_LIST_HEAD(&frame->list);
			list_add_tail(&frame->list, frames);
			done++;
		}
		return done;
	}

	spin_lock_irqsave(&ring->lock, flags);
	while (done < max) {
		if (!ring->running || u4_nhi_ring_empty(ring))
			break;

		meta = READ_ONCE(descs[ring->tail].meta);
		if (!(u4_nhi_desc_flags(meta) & RING_DESC_COMPLETED))
			break;

		frame = list_first_entry(&ring->in_flight,
					 struct ring_frame, list);
		list_del_init(&frame->list);

		if (!ring->is_tx) {
			frame->size = u4_nhi_desc_length(meta);
			frame->eof = u4_nhi_desc_eof(meta);
			frame->sof = u4_nhi_desc_sof(meta);
			frame->flags = u4_nhi_desc_flags(meta);
		}

		ring->tail = (ring->tail + 1) % ring->size;
		list_add_tail(&frame->list, frames);
		done++;
	}
	spin_unlock_irqrestore(&ring->lock, flags);

	if (done)
		atomic64_add(done, &raw_poll_hit);
	else if (READ_ONCE(raw_nhi_count_empty))
		atomic64_inc(&raw_poll_empty);
	return done;
}

unsigned int usb4_rdma_nhi_raw_poll_batch_unlocked(struct tb_ring *ring,
						   struct list_head *frames,
						   unsigned int max)
{
	struct u4_nhi_ring_desc *descs =
		(struct u4_nhi_ring_desc *)ring->descriptors;
	struct ring_frame *frame;
	unsigned int done = 0;
	u32 meta;

	if (!max || !READ_ONCE(raw_nhi))
		return 0;

	while (done < max) {
		if (!ring->running || u4_nhi_ring_empty(ring))
			break;

		meta = READ_ONCE(descs[ring->tail].meta);
		if (!(u4_nhi_desc_flags(meta) & RING_DESC_COMPLETED))
			break;

		frame = list_first_entry(&ring->in_flight,
					 struct ring_frame, list);
		list_del_init(&frame->list);

		if (!ring->is_tx) {
			frame->size = u4_nhi_desc_length(meta);
			frame->eof = u4_nhi_desc_eof(meta);
			frame->sof = u4_nhi_desc_sof(meta);
			frame->flags = u4_nhi_desc_flags(meta);
		}

		ring->tail = (ring->tail + 1) % ring->size;
		list_add_tail(&frame->list, frames);
		done++;
	}

	return done;
}

void usb4_rdma_nhi_raw_poll_complete(struct tb_ring *ring)
{
	tb_ring_poll_complete(ring);
}

void usb4_rdma_nhi_raw_stats_show(struct seq_file *m)
{
	seq_printf(m, "raw_nhi:              %u\n", READ_ONCE(raw_nhi));
	seq_printf(m, "raw_nhi_tx_posted:    %lld\n",
		   (long long)atomic64_read(&raw_tx_posted));
	seq_printf(m, "raw_nhi_rx_posted:    %lld\n",
		   (long long)atomic64_read(&raw_rx_posted));
	seq_printf(m, "raw_nhi_tx_fallback:  %lld\n",
		   (long long)atomic64_read(&raw_tx_fallback));
	seq_printf(m, "raw_nhi_rx_fallback:  %lld\n",
		   (long long)atomic64_read(&raw_rx_fallback));
	seq_printf(m, "raw_nhi_poll_hit:     %lld\n",
		   (long long)atomic64_read(&raw_poll_hit));
	seq_printf(m, "raw_nhi_poll_empty:   %lld\n",
		   (long long)atomic64_read(&raw_poll_empty));
}

void usb4_rdma_nhi_raw_ring_show(struct seq_file *m, const char *name,
				 struct tb_ring *ring)
{
	struct ring_frame *frame;
	unsigned int queued = 0;
	unsigned int inflight = 0;
	unsigned long flags;
	u32 index = 0xffffffff;
	int head;
	int tail;

	if (!ring) {
		seq_printf(m, "  %s_ring:            (none)\n", name);
		return;
	}

	if (ring->nhi && ring->nhi->iobase)
		index = ioread32(u4_nhi_ring_desc_base(ring) + 8);
	spin_lock_irqsave(&ring->lock, flags);
	head = ring->head;
	tail = ring->tail;
	list_for_each_entry(frame, &ring->queue, list)
		queued++;
	list_for_each_entry(frame, &ring->in_flight, list)
		inflight++;
	spin_unlock_irqrestore(&ring->lock, flags);

	seq_printf(m,
		   "  %s_ring_raw:        head=%d tail=%d hw_index=0x%08x queued=%u inflight=%u\n",
		   name, head, tail, index, queued, inflight);
}
