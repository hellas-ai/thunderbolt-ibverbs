// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include <stdio.h>

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

int main(void)
{
	CHECK(test_no_success_before_ack() == 0);
	CHECK(test_lost_middle_fragment_retry_exactly_once() == 0);
	CHECK(test_stale_connection_id_ignored() == 0);
	CHECK(test_rnr_is_not_success() == 0);
	CHECK(test_wrap_safe_sequence_helpers() == 0);

	puts("reliability smoke OK");
	return 0;
}
