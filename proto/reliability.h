/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef TBV_RELIABILITY_H
#define TBV_RELIABILITY_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/types.h>
typedef u8 tbv_rel_u8;
typedef u16 tbv_rel_u16;
typedef u32 tbv_rel_u32;
typedef u64 tbv_rel_u64;
#else
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t tbv_rel_u8;
typedef uint16_t tbv_rel_u16;
typedef uint32_t tbv_rel_u32;
typedef uint64_t tbv_rel_u64;
#endif

#define TBV_REL_MAX_FRAGS 64u
#define TBV_REL_ORDER_MAX 128u
#define TBV_REL_RETRY_INFINITE ((tbv_rel_u32)~0u)
#define TBV_REL_VERBS_RNR_RETRY_INFINITE 7u

enum tbv_rel_op_kind {
	TBV_REL_OP_SEND = 1,
	TBV_REL_OP_SEND_IMM = 2,
	TBV_REL_OP_RDMA_WRITE = 3,
	TBV_REL_OP_RDMA_WRITE_IMM = 4,
	TBV_REL_OP_RDMA_READ_RESP = 5,
};

enum tbv_rel_ack_status {
	TBV_REL_ACK_OK = 0,
	TBV_REL_ACK_RNR = 1,
	TBV_REL_ACK_ERROR = 2,
};

enum tbv_rel_completion_status {
	TBV_REL_COMP_OK = 0,
	TBV_REL_COMP_RNR_RETRY_EXHAUSTED = 1,
	TBV_REL_COMP_RETRY_EXHAUSTED = 2,
	TBV_REL_COMP_REMOTE_ERROR = 3,
	TBV_REL_COMP_FLUSHED = 4,
};

enum tbv_rel_tx_state {
	TBV_REL_TX_EMPTY = 0,
	TBV_REL_TX_READY = 1,
	TBV_REL_TX_IN_FLIGHT = 2,
	TBV_REL_TX_COMPLETE = 3,
	TBV_REL_TX_FAILED = 4,
};

struct tbv_rel_data_frame {
	tbv_rel_u64 conn_id;
	tbv_rel_u32 op_id;
	tbv_rel_u32 frame_seq;
	tbv_rel_u32 retry_generation;
	tbv_rel_u32 offset;
	tbv_rel_u32 length;
	tbv_rel_u16 frag_index;
	tbv_rel_u16 frag_count;
	tbv_rel_u8 op;
};

struct tbv_rel_ack_frame {
	tbv_rel_u64 conn_id;
	tbv_rel_u32 op_id;
	tbv_rel_u32 retry_generation;
	tbv_rel_u8 status;
};

struct tbv_rel_completion {
	bool valid;
	tbv_rel_u32 op_id;
	tbv_rel_u8 status;
};

struct tbv_rel_rx_event {
	bool ack_valid;
	bool completion_valid;
	bool duplicate;
	bool stale;
	bool rnr;
	struct tbv_rel_ack_frame ack;
	struct tbv_rel_completion completion;
};

struct tbv_rel_tx_op {
	tbv_rel_u64 conn_id;
	tbv_rel_u32 op_id;
	tbv_rel_u32 total_len;
	tbv_rel_u32 max_payload;
	tbv_rel_u32 frame_count;
	tbv_rel_u32 next_frame;
	tbv_rel_u32 retry_generation;
	tbv_rel_u32 retry_budget;
	tbv_rel_u32 rnr_budget;
	tbv_rel_u32 local_tx_frames;
	tbv_rel_u32 completion_count;
	tbv_rel_u8 op;
	tbv_rel_u8 state;
};

struct tbv_rel_rx_op {
	tbv_rel_u64 conn_id;
	tbv_rel_u32 max_payload;
	tbv_rel_u32 op_id;
	tbv_rel_u32 total_len;
	tbv_rel_u32 frame_count;
	tbv_rel_u32 received_count;
	tbv_rel_u32 completion_count;
	tbv_rel_u64 received_bitmap;
	struct tbv_rel_ack_frame cached_ack;
	tbv_rel_u8 op;
	bool active;
	bool accepted;
};

struct tbv_rel_order_entry {
	tbv_rel_u32 op_id;
	tbv_rel_u8 status;
	bool occupied;
	bool ready;
};

struct tbv_rel_order_queue {
	tbv_rel_u32 capacity;
	tbv_rel_u32 head;
	tbv_rel_u32 count;
	struct tbv_rel_order_entry entries[TBV_REL_ORDER_MAX];
};

static inline bool tbv_rel_u32_before(tbv_rel_u32 a, tbv_rel_u32 b)
{
	return a != b && (tbv_rel_u32)(a - b) > 0x80000000u;
}

static inline bool tbv_rel_u32_after(tbv_rel_u32 a, tbv_rel_u32 b)
{
	return tbv_rel_u32_before(b, a);
}

int tbv_rel_tx_start(struct tbv_rel_tx_op *tx, tbv_rel_u64 conn_id,
		     tbv_rel_u32 op_id, tbv_rel_u8 op,
		     tbv_rel_u32 total_len, tbv_rel_u32 max_payload,
		     tbv_rel_u32 retry_budget, tbv_rel_u32 rnr_budget);
int tbv_rel_tx_next_frame(struct tbv_rel_tx_op *tx,
			  struct tbv_rel_data_frame *frame);
int tbv_rel_tx_on_timeout(struct tbv_rel_tx_op *tx,
			  struct tbv_rel_completion *completion);
int tbv_rel_tx_on_ack(struct tbv_rel_tx_op *tx,
		      const struct tbv_rel_ack_frame *ack,
		      struct tbv_rel_completion *completion);
tbv_rel_u64 tbv_rel_retry_interval(tbv_rel_u64 ack_timeout,
				   tbv_rel_u32 retry_budget);
tbv_rel_u64 tbv_rel_ack_timeout_ns(tbv_rel_u8 timeout);
tbv_rel_u32 tbv_rel_decode_verbs_rnr_retry(tbv_rel_u8 rnr_retry);

int tbv_rel_order_init(struct tbv_rel_order_queue *q, tbv_rel_u32 capacity);
int tbv_rel_order_push(struct tbv_rel_order_queue *q, tbv_rel_u32 op_id);
int tbv_rel_order_mark(struct tbv_rel_order_queue *q, tbv_rel_u32 op_id,
		       tbv_rel_u8 status);
int tbv_rel_order_flush(struct tbv_rel_order_queue *q, tbv_rel_u8 status);
int tbv_rel_order_drain(struct tbv_rel_order_queue *q,
			struct tbv_rel_completion *out, tbv_rel_u32 max,
			tbv_rel_u32 *drained);

void tbv_rel_rx_init(struct tbv_rel_rx_op *rx, tbv_rel_u64 conn_id,
		     tbv_rel_u32 max_payload);
int tbv_rel_rx_on_data(struct tbv_rel_rx_op *rx,
		       const struct tbv_rel_data_frame *frame,
		       bool recv_available, struct tbv_rel_rx_event *event);

#endif /* TBV_RELIABILITY_H */
