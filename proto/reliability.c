// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include "reliability.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

static bool tbv_rel_valid_op(tbv_rel_u8 op)
{
	return op == TBV_REL_OP_SEND || op == TBV_REL_OP_SEND_IMM ||
	       op == TBV_REL_OP_RDMA_WRITE ||
	       op == TBV_REL_OP_RDMA_WRITE_IMM ||
	       op == TBV_REL_OP_RDMA_READ_RESP;
}

static void tbv_rel_zero_completion(struct tbv_rel_completion *completion)
{
	if (completion)
		memset(completion, 0, sizeof(*completion));
}

static void tbv_rel_complete(struct tbv_rel_completion *completion,
			     tbv_rel_u32 op_id, tbv_rel_u8 status)
{
	if (!completion)
		return;

	completion->valid = true;
	completion->op_id = op_id;
	completion->status = status;
}

static void tbv_rel_build_ack(const struct tbv_rel_data_frame *frame,
			      tbv_rel_u8 status,
			      struct tbv_rel_ack_frame *ack)
{
	memset(ack, 0, sizeof(*ack));
	ack->conn_id = frame->conn_id;
	ack->op_id = frame->op_id;
	ack->retry_generation = frame->retry_generation;
	ack->status = status;
}

int tbv_rel_tx_start(struct tbv_rel_tx_op *tx, tbv_rel_u64 conn_id,
		     tbv_rel_u32 op_id, tbv_rel_u8 op,
		     tbv_rel_u32 total_len, tbv_rel_u32 max_payload,
		     tbv_rel_u32 retry_budget, tbv_rel_u32 rnr_budget)
{
	tbv_rel_u32 frame_count;

	if (!tx || !tbv_rel_valid_op(op) || !max_payload)
		return -EINVAL;

	frame_count = total_len ? (total_len + max_payload - 1u) / max_payload :
				  1u;
	if (!frame_count || frame_count > TBV_REL_MAX_FRAGS)
		return -EMSGSIZE;

	memset(tx, 0, sizeof(*tx));
	tx->conn_id = conn_id;
	tx->op_id = op_id;
	tx->op = op;
	tx->total_len = total_len;
	tx->max_payload = max_payload;
	tx->frame_count = frame_count;
	tx->retry_budget = retry_budget;
	tx->rnr_budget = rnr_budget;
	tx->state = TBV_REL_TX_READY;

	return 0;
}

int tbv_rel_tx_next_frame(struct tbv_rel_tx_op *tx,
			  struct tbv_rel_data_frame *frame)
{
	tbv_rel_u32 offset;
	tbv_rel_u32 remaining;
	tbv_rel_u32 len;

	if (!tx || !frame)
		return -EINVAL;
	if (tx->state != TBV_REL_TX_READY &&
	    tx->state != TBV_REL_TX_IN_FLIGHT)
		return -EINVAL;
	if (tx->next_frame >= tx->frame_count)
		return -ENODATA;

	offset = tx->next_frame * tx->max_payload;
	remaining = tx->total_len > offset ? tx->total_len - offset : 0u;
	len = remaining > tx->max_payload ? tx->max_payload : remaining;

	memset(frame, 0, sizeof(*frame));
	frame->conn_id = tx->conn_id;
	frame->op_id = tx->op_id;
	frame->frame_seq = tx->next_frame;
	frame->retry_generation = tx->retry_generation;
	frame->offset = offset;
	frame->length = len;
	frame->frag_index = tx->next_frame;
	frame->frag_count = tx->frame_count;
	frame->op = tx->op;

	tx->next_frame++;
	tx->local_tx_frames++;
	tx->state = TBV_REL_TX_IN_FLIGHT;

	return 0;
}

int tbv_rel_tx_on_timeout(struct tbv_rel_tx_op *tx,
			  struct tbv_rel_completion *completion)
{
	if (!tx)
		return -EINVAL;

	tbv_rel_zero_completion(completion);
	if (tx->state != TBV_REL_TX_IN_FLIGHT)
		return -EINVAL;

	if (!tx->retry_budget) {
		tx->state = TBV_REL_TX_FAILED;
		tx->completion_count++;
		tbv_rel_complete(completion, tx->op_id,
				 TBV_REL_COMP_RETRY_EXHAUSTED);
		return 0;
	}

	tx->retry_budget--;
	tx->retry_generation++;
	tx->next_frame = 0;

	return 0;
}

int tbv_rel_tx_on_ack(struct tbv_rel_tx_op *tx,
		      const struct tbv_rel_ack_frame *ack,
		      struct tbv_rel_completion *completion)
{
	if (!tx || !ack)
		return -EINVAL;

	tbv_rel_zero_completion(completion);
	if (ack->conn_id != tx->conn_id || ack->op_id != tx->op_id)
		return 0;
	if (tx->state == TBV_REL_TX_COMPLETE || tx->state == TBV_REL_TX_FAILED)
		return 0;

	switch (ack->status) {
	case TBV_REL_ACK_OK:
		tx->state = TBV_REL_TX_COMPLETE;
		tx->completion_count++;
		tbv_rel_complete(completion, tx->op_id, TBV_REL_COMP_OK);
		return 0;
	case TBV_REL_ACK_RNR:
		if (!tx->rnr_budget) {
			tx->state = TBV_REL_TX_FAILED;
			tx->completion_count++;
			tbv_rel_complete(completion, tx->op_id,
					 TBV_REL_COMP_RNR_RETRY_EXHAUSTED);
			return 0;
		}
		tx->rnr_budget--;
		tx->retry_generation++;
		tx->next_frame = 0;
		tx->state = TBV_REL_TX_READY;
		return 0;
	case TBV_REL_ACK_ERROR:
		tx->state = TBV_REL_TX_FAILED;
		tx->completion_count++;
		tbv_rel_complete(completion, tx->op_id,
				 TBV_REL_COMP_REMOTE_ERROR);
		return 0;
	default:
		return -EINVAL;
	}
}

void tbv_rel_rx_init(struct tbv_rel_rx_op *rx, tbv_rel_u64 conn_id,
		     tbv_rel_u32 max_payload)
{
	memset(rx, 0, sizeof(*rx));
	rx->conn_id = conn_id;
	rx->max_payload = max_payload;
}

static int tbv_rel_rx_validate_frame(const struct tbv_rel_rx_op *rx,
				     const struct tbv_rel_data_frame *frame)
{
	tbv_rel_u32 expected_offset;

	if (!rx || !frame || !tbv_rel_valid_op(frame->op))
		return -EINVAL;
	if (!rx->max_payload || !frame->frag_count ||
	    frame->frag_count > TBV_REL_MAX_FRAGS ||
	    frame->frag_index >= frame->frag_count)
		return -EINVAL;

	expected_offset = (tbv_rel_u32)frame->frag_index * rx->max_payload;
	if (frame->offset != expected_offset)
		return -EINVAL;
	if (frame->length > rx->max_payload)
		return -EMSGSIZE;

	return 0;
}

int tbv_rel_rx_on_data(struct tbv_rel_rx_op *rx,
		       const struct tbv_rel_data_frame *frame,
		       bool recv_available, struct tbv_rel_rx_event *event)
{
	tbv_rel_u64 bit;
	int ret;

	if (!event)
		return -EINVAL;

	memset(event, 0, sizeof(*event));
	ret = tbv_rel_rx_validate_frame(rx, frame);
	if (ret)
		return ret;

	if (frame->conn_id != rx->conn_id) {
		event->stale = true;
		return 0;
	}

	if (rx->accepted && frame->op_id == rx->op_id) {
		event->duplicate = true;
		event->ack_valid = true;
		event->ack = rx->cached_ack;
		return 0;
	}

	if (!rx->active) {
		if (!recv_available) {
			event->rnr = true;
			event->ack_valid = true;
			tbv_rel_build_ack(frame, TBV_REL_ACK_RNR, &event->ack);
			return 0;
		}

		rx->active = true;
		rx->op_id = frame->op_id;
		rx->op = frame->op;
		rx->frame_count = frame->frag_count;
		rx->received_count = 0;
		rx->received_bitmap = 0;
		rx->total_len = 0;
	}

	if (frame->op_id != rx->op_id || frame->op != rx->op ||
	    frame->frag_count != rx->frame_count)
		return -EBUSY;

	bit = 1ull << frame->frag_index;
	if (rx->received_bitmap & bit) {
		event->duplicate = true;
	} else {
		rx->received_bitmap |= bit;
		rx->received_count++;
		rx->total_len += frame->length;
	}

	if (rx->received_count == rx->frame_count) {
		rx->accepted = true;
		rx->active = false;
		rx->completion_count++;
		tbv_rel_build_ack(frame, TBV_REL_ACK_OK, &rx->cached_ack);
		event->ack_valid = true;
		event->ack = rx->cached_ack;
		event->completion_valid = true;
		tbv_rel_complete(&event->completion, frame->op_id,
				 TBV_REL_COMP_OK);
	}

	return 0;
}
