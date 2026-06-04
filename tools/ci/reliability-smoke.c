// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include <stdio.h>
#include <string.h>

#include "proto/reliability.h"

#define CHECK(cond)                                                            \
	do {                                                                   \
		if (!(cond)) {                                                 \
			fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, \
				__LINE__, #cond);                               \
			return 1;                                              \
		}                                                              \
	} while (0)

static int send_all(struct tbv_rel_tx_op *tx,
		    struct tbv_rel_data_frame *frames, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		CHECK(tbv_rel_tx_next_frame(tx, &frames[i]) == 0);

	return 0;
}

static int test_no_success_before_ack(void)
{
	struct tbv_rel_tx_op tx;
	struct tbv_rel_data_frame frame;
	struct tbv_rel_completion comp;

	CHECK(tbv_rel_tx_start(&tx, 0x1111, 1, TBV_REL_OP_SEND, 32, 16, 1,
			       1) == 0);
	CHECK(tbv_rel_tx_next_frame(&tx, &frame) == 0);
	CHECK(tbv_rel_tx_next_frame(&tx, &frame) == 0);
	CHECK(tbv_rel_tx_next_frame(&tx, &frame) == -ENODATA);
	CHECK(tx.state == TBV_REL_TX_IN_FLIGHT);
	CHECK(tx.completion_count == 0);

	CHECK(tbv_rel_tx_on_timeout(&tx, &comp) == 0);
	CHECK(!comp.valid);
	CHECK(tx.state == TBV_REL_TX_IN_FLIGHT);
	CHECK(tx.retry_generation == 1);
	CHECK(tx.completion_count == 0);

	return 0;
}

static int test_lost_middle_fragment_retry_exactly_once(void)
{
	struct tbv_rel_tx_op tx;
	struct tbv_rel_rx_op rx;
	struct tbv_rel_data_frame first[3];
	struct tbv_rel_data_frame retry[3];
	struct tbv_rel_rx_event event;
	struct tbv_rel_completion comp;

	CHECK(tbv_rel_tx_start(&tx, 0x2222, 7, TBV_REL_OP_SEND, 48, 16, 2,
			       0) == 0);
	tbv_rel_rx_init(&rx, 0x2222, 16);

	CHECK(send_all(&tx, first, 3) == 0);
	CHECK(tbv_rel_rx_on_data(&rx, &first[0], true, &event) == 0);
	CHECK(!event.ack_valid && !event.completion_valid);
	CHECK(tbv_rel_rx_on_data(&rx, &first[2], true, &event) == 0);
	CHECK(!event.ack_valid && !event.completion_valid);

	CHECK(tbv_rel_tx_on_timeout(&tx, &comp) == 0);
	CHECK(!comp.valid);
	CHECK(send_all(&tx, retry, 3) == 0);

	CHECK(tbv_rel_rx_on_data(&rx, &retry[0], true, &event) == 0);
	CHECK(event.duplicate && !event.completion_valid);
	CHECK(tbv_rel_rx_on_data(&rx, &retry[1], true, &event) == 0);
	CHECK(event.ack_valid && event.completion_valid);
	CHECK(event.ack.status == TBV_REL_ACK_OK);
	CHECK(rx.completion_count == 1);

	CHECK(tbv_rel_tx_on_ack(&tx, &event.ack, &comp) == 0);
	CHECK(comp.valid && comp.status == TBV_REL_COMP_OK);
	CHECK(tx.completion_count == 1);

	CHECK(tbv_rel_rx_on_data(&rx, &retry[2], true, &event) == 0);
	CHECK(event.duplicate && event.ack_valid);
	CHECK(!event.completion_valid);
	CHECK(rx.completion_count == 1);

	CHECK(tbv_rel_tx_on_ack(&tx, &event.ack, &comp) == 0);
	CHECK(!comp.valid);
	CHECK(tx.completion_count == 1);

	return 0;
}

static int test_stale_connection_id_ignored(void)
{
	struct tbv_rel_rx_op rx;
	struct tbv_rel_data_frame frame = {
		.conn_id = 0x3333,
		.op_id = 1,
		.frame_seq = 0,
		.retry_generation = 0,
		.offset = 0,
		.length = 8,
		.frag_index = 0,
		.frag_count = 1,
		.op = TBV_REL_OP_SEND,
	};
	struct tbv_rel_rx_event event;

	tbv_rel_rx_init(&rx, 0x4444, 16);
	CHECK(tbv_rel_rx_on_data(&rx, &frame, true, &event) == 0);
	CHECK(event.stale);
	CHECK(!event.ack_valid && !event.completion_valid);
	CHECK(!rx.active && !rx.accepted && rx.completion_count == 0);

	return 0;
}

static int test_rnr_is_not_success(void)
{
	struct tbv_rel_tx_op tx;
	struct tbv_rel_rx_op rx;
	struct tbv_rel_data_frame frame;
	struct tbv_rel_rx_event event;
	struct tbv_rel_completion comp;

	CHECK(tbv_rel_tx_start(&tx, 0x5555, 9, TBV_REL_OP_SEND, 8, 16, 1,
			       1) == 0);
	tbv_rel_rx_init(&rx, 0x5555, 16);

	CHECK(tbv_rel_tx_next_frame(&tx, &frame) == 0);
	CHECK(tbv_rel_rx_on_data(&rx, &frame, false, &event) == 0);
	CHECK(event.rnr && event.ack_valid);
	CHECK(event.ack.status == TBV_REL_ACK_RNR);
	CHECK(!event.completion_valid);
	CHECK(rx.completion_count == 0);

	CHECK(tbv_rel_tx_on_ack(&tx, &event.ack, &comp) == 0);
	CHECK(!comp.valid);
	CHECK(tx.state == TBV_REL_TX_READY);
	CHECK(tx.completion_count == 0);

	CHECK(tbv_rel_tx_next_frame(&tx, &frame) == 0);
	CHECK(tbv_rel_rx_on_data(&rx, &frame, true, &event) == 0);
	CHECK(event.ack_valid && event.completion_valid);
	CHECK(event.ack.status == TBV_REL_ACK_OK);
	CHECK(rx.completion_count == 1);

	CHECK(tbv_rel_tx_on_ack(&tx, &event.ack, &comp) == 0);
	CHECK(comp.valid && comp.status == TBV_REL_COMP_OK);
	CHECK(tx.completion_count == 1);

	return 0;
}

static int test_verbs_rnr_retry_7_is_infinite(void)
{
	struct tbv_rel_tx_op tx;
	struct tbv_rel_ack_frame ack = {
		.conn_id = 0x5656,
		.op_id = 10,
		.status = TBV_REL_ACK_RNR,
	};
	struct tbv_rel_completion comp;
	unsigned int i;

	CHECK(tbv_rel_decode_verbs_rnr_retry(0) == 0);
	CHECK(tbv_rel_decode_verbs_rnr_retry(6) == 6);
	CHECK(tbv_rel_decode_verbs_rnr_retry(7) == TBV_REL_RETRY_INFINITE);
	CHECK(tbv_rel_decode_verbs_rnr_retry(15) == TBV_REL_RETRY_INFINITE);

	CHECK(tbv_rel_tx_start(&tx, ack.conn_id, ack.op_id, TBV_REL_OP_SEND,
			       8, 16, 1,
			       tbv_rel_decode_verbs_rnr_retry(7)) == 0);

	for (i = 0; i < 32; i++) {
		CHECK(tbv_rel_tx_on_ack(&tx, &ack, &comp) == 0);
		CHECK(!comp.valid);
		CHECK(tx.state == TBV_REL_TX_READY);
		CHECK(tx.rnr_budget == TBV_REL_RETRY_INFINITE);
		CHECK(tx.completion_count == 0);
	}

	return 0;
}

static int test_wrap_safe_sequence_helpers(void)
{
	CHECK(tbv_rel_u32_before(1, 2));
	CHECK(tbv_rel_u32_after(2, 1));
	CHECK(tbv_rel_u32_before(0xffffffffu, 1));
	CHECK(tbv_rel_u32_after(1, 0xffffffffu));
	CHECK(!tbv_rel_u32_before(7, 7));
	CHECK(!tbv_rel_u32_after(7, 7));

	return 0;
}

static int test_retry_interval_uses_retry_budget(void)
{
	CHECK(tbv_rel_retry_interval(0, 7) == 0);
	CHECK(tbv_rel_retry_interval(5000, 0) == 5000);
	CHECK(tbv_rel_retry_interval(5000, 7) == 5000);
	CHECK(tbv_rel_retry_interval(5, 7) == 5);

	return 0;
}

static int test_ack_timeout_encoding(void)
{
	CHECK(tbv_rel_ack_timeout_ns(0) == 4096);
	CHECK(tbv_rel_ack_timeout_ns(14) == 67108864);
	CHECK(tbv_rel_ack_timeout_ns(31) == 8796093022208ULL);
	CHECK(tbv_rel_ack_timeout_ns(32) == 8796093022208ULL);

	return 0;
}

static int test_ordered_completion_queue(void)
{
	struct tbv_rel_order_queue q;
	struct tbv_rel_completion out[3] = {};
	tbv_rel_u32 drained = 99;

	CHECK(tbv_rel_order_init(&q, 4) == 0);
	CHECK(tbv_rel_order_push(&q, 1) == 0);
	CHECK(tbv_rel_order_push(&q, 2) == 0);
	CHECK(tbv_rel_order_push(&q, 3) == 0);

	CHECK(tbv_rel_order_mark(&q, 2, TBV_REL_COMP_OK) == 0);
	CHECK(tbv_rel_order_drain(&q, out, 3, &drained) == 0);
	CHECK(drained == 0);

	CHECK(tbv_rel_order_mark(&q, 1, TBV_REL_COMP_OK) == 0);
	CHECK(tbv_rel_order_drain(&q, out, 3, &drained) == 0);
	CHECK(drained == 2);
	CHECK(out[0].valid && out[0].op_id == 1 &&
	      out[0].status == TBV_REL_COMP_OK);
	CHECK(out[1].valid && out[1].op_id == 2 &&
	      out[1].status == TBV_REL_COMP_OK);

	CHECK(tbv_rel_order_mark(&q, 3, TBV_REL_COMP_REMOTE_ERROR) == 0);
	CHECK(tbv_rel_order_drain(&q, out, 3, &drained) == 0);
	CHECK(drained == 1);
	CHECK(out[0].valid && out[0].op_id == 3 &&
	      out[0].status == TBV_REL_COMP_REMOTE_ERROR);

	return 0;
}

static int test_ordered_terminal_failure_does_not_overtake(void)
{
	struct tbv_rel_order_queue q;
	struct tbv_rel_completion out[2] = {};
	tbv_rel_u32 drained;

	CHECK(tbv_rel_order_init(&q, 4) == 0);
	CHECK(tbv_rel_order_push(&q, 10) == 0);
	CHECK(tbv_rel_order_push(&q, 11) == 0);

	CHECK(tbv_rel_order_mark(&q, 11,
				 TBV_REL_COMP_RETRY_EXHAUSTED) == 0);
	CHECK(tbv_rel_order_drain(&q, out, 2, &drained) == 0);
	CHECK(drained == 0);

	CHECK(tbv_rel_order_flush(&q, TBV_REL_COMP_FLUSHED) == 0);
	CHECK(tbv_rel_order_drain(&q, out, 2, &drained) == 0);
	CHECK(drained == 2);
	CHECK(out[0].valid && out[0].op_id == 10 &&
	      out[0].status == TBV_REL_COMP_FLUSHED);
	CHECK(out[1].valid && out[1].op_id == 11 &&
	      out[1].status == TBV_REL_COMP_RETRY_EXHAUSTED);

	return 0;
}

static int test_ordered_rnr_exhaustion_waits_for_prior_wr(void)
{
	struct tbv_rel_order_queue q;
	struct tbv_rel_completion out[2] = {};
	tbv_rel_u32 drained;

	CHECK(tbv_rel_order_init(&q, 4) == 0);
	CHECK(tbv_rel_order_push(&q, 20) == 0);
	CHECK(tbv_rel_order_push(&q, 21) == 0);

	CHECK(tbv_rel_order_mark(&q, 21,
				 TBV_REL_COMP_RNR_RETRY_EXHAUSTED) == 0);
	CHECK(tbv_rel_order_drain(&q, out, 2, &drained) == 0);
	CHECK(drained == 0);

	CHECK(tbv_rel_order_mark(&q, 20, TBV_REL_COMP_OK) == 0);
	CHECK(tbv_rel_order_drain(&q, out, 2, &drained) == 0);
	CHECK(drained == 2);
	CHECK(out[0].valid && out[0].op_id == 20 &&
	      out[0].status == TBV_REL_COMP_OK);
	CHECK(out[1].valid && out[1].op_id == 21 &&
	      out[1].status == TBV_REL_COMP_RNR_RETRY_EXHAUSTED);

	return 0;
}

static int test_ordered_completion_ring_wrap(void)
{
	struct tbv_rel_order_queue q;
	struct tbv_rel_completion out[2] = {};
	tbv_rel_u32 drained;

	CHECK(tbv_rel_order_init(&q, 2) == 0);
	CHECK(tbv_rel_order_push(&q, 30) == 0);
	CHECK(tbv_rel_order_push(&q, 31) == 0);
	CHECK(tbv_rel_order_push(&q, 32) == -ENOSPC);
	CHECK(tbv_rel_order_mark(&q, 30, TBV_REL_COMP_OK) == 0);
	CHECK(tbv_rel_order_drain(&q, out, 1, &drained) == 0);
	CHECK(drained == 1 && out[0].op_id == 30);

	CHECK(tbv_rel_order_push(&q, 32) == 0);
	CHECK(tbv_rel_order_mark(&q, 32, TBV_REL_COMP_OK) == 0);
	CHECK(tbv_rel_order_drain(&q, out, 2, &drained) == 0);
	CHECK(drained == 0);

	CHECK(tbv_rel_order_mark(&q, 31, TBV_REL_COMP_OK) == 0);
	CHECK(tbv_rel_order_drain(&q, out, 2, &drained) == 0);
	CHECK(drained == 2);
	CHECK(out[0].op_id == 31 && out[1].op_id == 32);

	return 0;
}

#define READ_RESP_TEST_TOTAL 48u
#define READ_RESP_TEST_CHUNK 16u
#define READ_RESP_TEST_FRAGS \
	(READ_RESP_TEST_TOTAL / READ_RESP_TEST_CHUNK)

struct read_resp_test_frag {
	unsigned int offset;
	unsigned int len;
	unsigned char data[READ_RESP_TEST_CHUNK];
	bool valid;
};

struct read_resp_test_rx {
	unsigned int received;
	unsigned int retry_acks;
	unsigned int ok_acks;
	unsigned char out[READ_RESP_TEST_TOTAL];
	struct read_resp_test_frag frags[READ_RESP_TEST_FRAGS];
};

static struct read_resp_test_frag *
read_resp_test_find_frag(struct read_resp_test_rx *rx, unsigned int offset)
{
	unsigned int i;

	for (i = 0; i < READ_RESP_TEST_FRAGS; i++) {
		if (rx->frags[i].valid && rx->frags[i].offset == offset)
			return &rx->frags[i];
	}

	return NULL;
}

static int read_resp_test_store_frag(struct read_resp_test_rx *rx,
				     unsigned int offset,
				     const unsigned char *payload,
				     unsigned int len)
{
	struct read_resp_test_frag *frag;
	unsigned int i;

	frag = read_resp_test_find_frag(rx, offset);
	if (frag) {
		if (frag->len != len || memcmp(frag->data, payload, len))
			return -EIO;
		return 0;
	}

	for (i = 0; i < READ_RESP_TEST_FRAGS; i++) {
		frag = &rx->frags[i];
		if (frag->valid)
			continue;
		frag->offset = offset;
		frag->len = len;
		memcpy(frag->data, payload, len);
		frag->valid = true;
		return 0;
	}

	return -ENOSPC;
}

static int read_resp_test_drain(struct read_resp_test_rx *rx)
{
	struct read_resp_test_frag *frag;

	for (;;) {
		frag = read_resp_test_find_frag(rx, rx->received);
		if (!frag)
			return 0;

		memcpy(rx->out + frag->offset, frag->data, frag->len);
		rx->received = frag->offset + frag->len;
		frag->valid = false;
	}
}

static int read_resp_test_deliver(struct read_resp_test_rx *rx,
				  unsigned int offset,
				  const unsigned char *payload,
				  unsigned int len)
{
	if (offset + len > READ_RESP_TEST_TOTAL || len > READ_RESP_TEST_CHUNK)
		return -EINVAL;
	if (offset < rx->received)
		return 0;
	if (offset != rx->received) {
		int ret = read_resp_test_store_frag(rx, offset, payload, len);

		if (ret)
			return ret;
		rx->retry_acks++;
		return 0;
	}

	memcpy(rx->out + offset, payload, len);
	rx->received = offset + len;
	CHECK(read_resp_test_drain(rx) == 0);
	if (rx->received == READ_RESP_TEST_TOTAL)
		rx->ok_acks++;
	return 0;
}

static int test_read_response_retry_uses_stable_snapshot(void)
{
	unsigned char live[READ_RESP_TEST_TOTAL];
	unsigned char snapshot[READ_RESP_TEST_TOTAL];
	struct read_resp_test_rx rx = {};
	struct read_resp_test_rx inconsistent = {};
	unsigned int i;

	for (i = 0; i < READ_RESP_TEST_TOTAL; i++)
		live[i] = (unsigned char)(i + 1);
	memcpy(snapshot, live, sizeof(snapshot));
	for (i = 0; i < READ_RESP_TEST_TOTAL; i++)
		live[i] = (unsigned char)(0xa0u + i);

	CHECK(read_resp_test_deliver(&rx, 32, snapshot + 32, 16) == 0);
	CHECK(rx.retry_acks == 1 && rx.ok_acks == 0 && rx.received == 0);
	CHECK(read_resp_test_deliver(&rx, 0, snapshot, 16) == 0);
	CHECK(rx.received == 16 && rx.ok_acks == 0);
	CHECK(read_resp_test_deliver(&rx, 16, snapshot + 16, 16) == 0);
	CHECK(rx.received == READ_RESP_TEST_TOTAL && rx.ok_acks == 1);
	CHECK(memcmp(rx.out, snapshot, sizeof(snapshot)) == 0);
	CHECK(memcmp(rx.out, live, sizeof(live)) != 0);

	CHECK(read_resp_test_deliver(&inconsistent, 32, snapshot + 32, 16) == 0);
	CHECK(read_resp_test_deliver(&inconsistent, 32, live + 32, 16) == -EIO);

	return 0;
}

static unsigned int next_rand(unsigned int *state)
{
	unsigned int x = *state;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

static void shuffle_frames(struct tbv_rel_data_frame *frames,
			   unsigned int count, unsigned int *rng)
{
	unsigned int i;

	for (i = count; i > 1; i--) {
		unsigned int j = next_rand(rng) % i;
		struct tbv_rel_data_frame tmp = frames[i - 1];

		frames[i - 1] = frames[j];
		frames[j] = tmp;
	}
}

static int deliver_one(struct tbv_rel_tx_op *tx, struct tbv_rel_rx_op *rx,
		       const struct tbv_rel_data_frame *frame,
		       bool recv_available, bool *rx_completed)
{
	struct tbv_rel_rx_event event;
	struct tbv_rel_completion comp;

	CHECK(tbv_rel_rx_on_data(rx, frame, recv_available, &event) == 0);
	CHECK(rx->completion_count <= 1);
	if (event.completion_valid) {
		CHECK(!*rx_completed);
		*rx_completed = true;
		CHECK(event.completion.status == TBV_REL_COMP_OK);
	}
	if (event.ack_valid) {
		CHECK(tbv_rel_tx_on_ack(tx, &event.ack, &comp) == 0);
		if (comp.valid) {
			CHECK(*rx_completed);
			CHECK(comp.status == TBV_REL_COMP_OK ||
			      comp.status == TBV_REL_COMP_RNR_RETRY_EXHAUSTED);
		}
	}

	return 0;
}

static int test_generated_loss_duplicate_reorder_schedules_for_op(
	tbv_rel_u8 op, unsigned int seed_base, bool may_rnr)
{
	unsigned int seed;

	for (seed = 1; seed <= 128; seed++) {
		struct tbv_rel_tx_op tx;
		struct tbv_rel_rx_op rx;
		struct tbv_rel_data_frame frames[TBV_REL_MAX_FRAGS];
		struct tbv_rel_completion comp;
		unsigned int rng = seed * 0x9e3779b9u;
		unsigned int total_len = 1 + (next_rand(&rng) % 96u);
		unsigned int attempts = 0;
		bool rx_completed = false;

		CHECK(tbv_rel_tx_start(&tx, 0x6000u + seed_base + seed,
				       seed_base + seed, op, total_len, 16, 128,
				       8) == 0);
		tbv_rel_rx_init(&rx, 0x6000u + seed_base + seed, 16);

		while (tx.state != TBV_REL_TX_COMPLETE && attempts < 128) {
			unsigned int count = 0;
			unsigned int i;
			bool force_deliver = attempts > 24;
			bool rnr_first = may_rnr && !rx.active && !rx.accepted &&
					 attempts < 4 &&
					 (next_rand(&rng) & 7u) == 0;

			while (tbv_rel_tx_next_frame(&tx, &frames[count]) == 0)
				count++;
			CHECK(count == tx.frame_count);
			shuffle_frames(frames, count, &rng);

			for (i = 0; i < count; i++) {
				bool drop = !force_deliver &&
					    (next_rand(&rng) & 3u) == 0;
				bool dup = (next_rand(&rng) & 3u) == 0;
				bool recv_available = !rnr_first || i != 0;

				if (drop)
					continue;

				CHECK(deliver_one(&tx, &rx, &frames[i],
						  recv_available,
						  &rx_completed) == 0);
				if (dup)
					CHECK(deliver_one(&tx, &rx, &frames[i],
							  true,
							  &rx_completed) == 0);
			}

			if (tx.state == TBV_REL_TX_COMPLETE)
				break;
			if (tx.state == TBV_REL_TX_READY) {
				attempts++;
				continue;
			}

			CHECK(tbv_rel_tx_on_timeout(&tx, &comp) == 0);
			CHECK(!comp.valid);
			attempts++;
		}

		CHECK(tx.state == TBV_REL_TX_COMPLETE);
		CHECK(tx.completion_count == 1);
		CHECK(rx.completion_count == 1);
		CHECK(rx_completed);
	}

	return 0;
}

static int test_generated_loss_duplicate_reorder_schedules(void)
{
	CHECK(test_generated_loss_duplicate_reorder_schedules_for_op(
		      TBV_REL_OP_SEND, 0x1000u, true) == 0);
	CHECK(test_generated_loss_duplicate_reorder_schedules_for_op(
		      TBV_REL_OP_RDMA_WRITE, 0x2000u, false) == 0);
	CHECK(test_generated_loss_duplicate_reorder_schedules_for_op(
		      TBV_REL_OP_RDMA_READ_RESP, 0x3000u, false) == 0);

	return 0;
}

int main(void)
{
	CHECK(test_no_success_before_ack() == 0);
	CHECK(test_lost_middle_fragment_retry_exactly_once() == 0);
	CHECK(test_stale_connection_id_ignored() == 0);
	CHECK(test_rnr_is_not_success() == 0);
	CHECK(test_verbs_rnr_retry_7_is_infinite() == 0);
	CHECK(test_wrap_safe_sequence_helpers() == 0);
	CHECK(test_retry_interval_uses_retry_budget() == 0);
	CHECK(test_ack_timeout_encoding() == 0);
	CHECK(test_ordered_completion_queue() == 0);
	CHECK(test_ordered_terminal_failure_does_not_overtake() == 0);
	CHECK(test_ordered_rnr_exhaustion_waits_for_prior_wr() == 0);
	CHECK(test_ordered_completion_ring_wrap() == 0);
	CHECK(test_read_response_retry_uses_stable_snapshot() == 0);
	CHECK(test_generated_loss_duplicate_reorder_schedules() == 0);

	puts("reliability smoke OK");
	return 0;
}
