// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "proto/apple_tx.h"
#include "proto/native_data.h"
#include "proto/tbnet.h"

#define APPLE_RAW_CHUNK_SIZE 4096u
#define APPLE_RAW_TAIL_USER_SIZE 240u
#define APPLE_RAW_TAIL_WIRE_SIZE (APPLE_RAW_TAIL_USER_SIZE + sizeof(uint32_t))

static uint32_t smoke_crc32c(uint32_t seed, const void *data, size_t len)
{
	const unsigned char *p = data;
	uint32_t crc = seed;
	size_t i;

	while (len--) {
		crc ^= *p++;
		for (i = 0; i < 8; i++)
			crc = (crc >> 1) ^ (0x82f63b78u & -(crc & 1u));
	}

	return crc;
}

static void put_le32(unsigned char *dst, uint32_t value)
{
	dst[0] = (unsigned char)value;
	dst[1] = (unsigned char)(value >> 8);
	dst[2] = (unsigned char)(value >> 16);
	dst[3] = (unsigned char)(value >> 24);
}

static uint32_t get_le32(const unsigned char *src)
{
	return (uint32_t)src[0] |
	       ((uint32_t)src[1] << 8) |
	       ((uint32_t)src[2] << 16) |
	       ((uint32_t)src[3] << 24);
}

static int apple_raw_verify_crc_model(const unsigned char *assembled,
				      unsigned int delivered,
				      const unsigned char *tail_wire,
				      unsigned int tail_wire_len)
{
	uint32_t expected;
	uint32_t actual;

	if (tail_wire_len < sizeof(expected) ||
	    delivered < APPLE_RAW_CHUNK_SIZE ||
	    delivered % APPLE_RAW_CHUNK_SIZE)
		return -1;

	expected = get_le32(tail_wire + tail_wire_len - sizeof(expected));
	actual = smoke_crc32c(~0u, assembled + delivered -
			      APPLE_RAW_CHUNK_SIZE,
			      APPLE_RAW_CHUNK_SIZE) ^ ~0u;

	return actual == expected ? 0 : -1;
}

static int hello_equal(const struct tbv_native_wire_hello *a,
		       const struct tbv_native_wire_hello *b)
{
	return a->capabilities == b->capabilities &&
	       a->rail_id == b->rail_id &&
	       a->route == b->route &&
	       a->tx_hop == b->tx_hop &&
	       a->rx_hop == b->rx_hop &&
	       a->transmit_path == b->transmit_path &&
	       a->tx_ring_size == b->tx_ring_size &&
	       a->rx_ring_size == b->rx_ring_size &&
	       a->path_flags == b->path_flags;
}

static int data_header_equal(const struct tbv_native_data_header *a,
			     const struct tbv_native_data_header *b)
{
	return a->opcode == b->opcode &&
	       a->flags == b->flags &&
	       a->dest_qp == b->dest_qp &&
	       a->src_qp == b->src_qp &&
	       a->psn == b->psn &&
	       a->length == b->length &&
	       a->imm_data == b->imm_data &&
	       a->remote_addr == b->remote_addr &&
	       a->rkey == b->rkey &&
	       a->frag_offset == b->frag_offset;
}

static unsigned int native_data_frags(unsigned int len)
{
	return (len + TBV_NATIVE_DATA_MAX_PAYLOAD - 1u) /
	       TBV_NATIVE_DATA_MAX_PAYLOAD;
}

static int test_raw_stream_payload_header(void)
{
	struct tbv_native_data_header stream = {
		.opcode = TBV_NATIVE_DATA_OP_RDMA_WRITE,
		.flags = TBV_NATIVE_DATA_F_LAST |
			 TBV_NATIVE_DATA_F_SOLICITED |
			 TBV_NATIVE_DATA_F_RAW_STREAM,
		.dest_qp = 0x900,
		.src_qp = 0x901,
		.psn = 42,
		.length = 8192,
		.imm_data = 8192,
		.remote_addr = 0x100000000ull,
		.rkey = 0x1234,
	};
	struct tbv_native_data_header frag;
	int ret;

	ret = tbv_native_data_raw_payload_header(&stream, 0, 8192, 4096,
						 &frag);
	if (ret)
		return 1;
	if (frag.remote_addr != stream.remote_addr || frag.frag_offset != 0 ||
	    frag.length != 4096 || frag.flags)
		return 2;

	ret = tbv_native_data_raw_payload_header(&stream, 4096, 4096, 4096,
						 &frag);
	if (ret)
		return 3;
	if (frag.remote_addr != stream.remote_addr ||
	    frag.frag_offset != 4096 ||
	    frag.length != 4096 ||
	    frag.flags != (TBV_NATIVE_DATA_F_LAST |
			   TBV_NATIVE_DATA_F_SOLICITED))
		return 4;

	if (tbv_native_data_raw_payload_header(&stream, 4096, 4096, 4097,
					       &frag) != -EINVAL)
		return 5;

	stream.flags &= ~TBV_NATIVE_DATA_F_RAW_STREAM;
	if (tbv_native_data_raw_payload_header(&stream, 0, 4096, 4096,
					       &frag) != -EINVAL)
		return 6;

	return 0;
}

static int test_fragment_shapes(void)
{
	unsigned int idx;
	unsigned int count;

	if (tbv_native_data_fragment_shape(8192, TBV_NATIVE_DATA_MAX_PAYLOAD,
					   TBV_NATIVE_DATA_MAX_FRAGS, 0,
					   TBV_NATIVE_DATA_MAX_PAYLOAD, false,
					   &idx, &count))
		return 1;
	if (idx != 0 || count != 3)
		return 2;
	if (tbv_native_data_fragment_shape(8192, TBV_NATIVE_DATA_MAX_PAYLOAD,
					   TBV_NATIVE_DATA_MAX_FRAGS,
					   TBV_NATIVE_DATA_MAX_PAYLOAD,
					   TBV_NATIVE_DATA_MAX_PAYLOAD, false,
					   &idx, &count))
		return 3;
	if (idx != 1 || count != 3)
		return 4;
	if (tbv_native_data_fragment_shape(8192, TBV_NATIVE_DATA_MAX_PAYLOAD,
					   TBV_NATIVE_DATA_MAX_FRAGS, 8096,
					   96, true, &idx, &count))
		return 5;
	if (idx != 2 || count != 3)
		return 6;

	if (tbv_native_data_fragment_shape(8192, TBV_NATIVE_DATA_FRAME_SIZE,
					   TBV_NATIVE_DATA_MAX_FRAGS, 0,
					   TBV_NATIVE_DATA_FRAME_SIZE, false,
					   &idx, &count))
		return 7;
	if (idx != 0 || count != 2)
		return 8;
	if (tbv_native_data_fragment_shape(8192, TBV_NATIVE_DATA_FRAME_SIZE,
					   TBV_NATIVE_DATA_MAX_FRAGS, 4096,
					   TBV_NATIVE_DATA_FRAME_SIZE, true,
					   &idx, &count))
		return 9;
	if (idx != 1 || count != 2)
		return 10;
	if (tbv_native_data_fragment_shape(8192, TBV_NATIVE_DATA_MAX_PAYLOAD,
					   TBV_NATIVE_DATA_MAX_FRAGS, 4096,
					   TBV_NATIVE_DATA_FRAME_SIZE, true,
					   &idx, &count) != -EINVAL)
		return 11;

	return 0;
}

static int test_minimal_tbnet_e2e_policy(void)
{
	tbv_tbnet_u32 peer_e2e =
		TBV_TBNET_MINIMAL_BASE_PRTCSTNS | TBV_TBNET_PRTCSTNS_E2E;

	if (tbv_tbnet_minimal_prtcstns(false) !=
	    TBV_TBNET_MINIMAL_BASE_PRTCSTNS)
		return 1;
	if (tbv_tbnet_minimal_prtcstns(true) != peer_e2e)
		return 2;
	if (tbv_tbnet_minimal_e2e_enabled(false, peer_e2e))
		return 3;
	if (tbv_tbnet_minimal_e2e_enabled(true,
					  TBV_TBNET_MINIMAL_BASE_PRTCSTNS))
		return 4;
	if (!tbv_tbnet_minimal_e2e_enabled(true, peer_e2e))
		return 5;

	return 0;
}

static int test_apple_raw_crc_model(void)
{
	unsigned char assembled[APPLE_RAW_CHUNK_SIZE * 2];
	unsigned char tail[APPLE_RAW_TAIL_WIRE_SIZE];
	uint32_t crc;
	unsigned int i;

	for (i = 0; i < sizeof(assembled); i++)
		assembled[i] = (unsigned char)((i * 17u + 3u) & 0xffu);
	memcpy(tail, assembled + APPLE_RAW_CHUNK_SIZE -
	       APPLE_RAW_TAIL_USER_SIZE, APPLE_RAW_TAIL_USER_SIZE);
	crc = smoke_crc32c(~0u, assembled, APPLE_RAW_CHUNK_SIZE) ^ ~0u;
	put_le32(tail + APPLE_RAW_TAIL_USER_SIZE, crc);

	if (apple_raw_verify_crc_model(assembled, APPLE_RAW_CHUNK_SIZE, tail,
				       sizeof(tail)))
		return 1;

	tail[APPLE_RAW_TAIL_USER_SIZE] ^= 0x01;
	if (!apple_raw_verify_crc_model(assembled, APPLE_RAW_CHUNK_SIZE, tail,
					sizeof(tail)))
		return 2;
	tail[APPLE_RAW_TAIL_USER_SIZE] ^= 0x01;

	if (!apple_raw_verify_crc_model(assembled, APPLE_RAW_CHUNK_SIZE - 1,
					tail, sizeof(tail)))
		return 3;
	if (!apple_raw_verify_crc_model(assembled, APPLE_RAW_CHUNK_SIZE, tail,
					sizeof(uint32_t) - 1))
		return 4;

	crc = smoke_crc32c(~0u, assembled + APPLE_RAW_CHUNK_SIZE,
			   APPLE_RAW_CHUNK_SIZE) ^ ~0u;
	put_le32(tail + APPLE_RAW_TAIL_USER_SIZE, crc);
	if (apple_raw_verify_crc_model(assembled, APPLE_RAW_CHUNK_SIZE * 2,
				       tail, sizeof(tail)))
		return 5;

	return 0;
}

static int test_apple_tx_window_policy(void)
{
	tbv_apple_tx_u32 single_charge = tbv_apple_tx_frame_charge(1, 4);
	tbv_apple_tx_u32 multi_charge = tbv_apple_tx_frame_charge(17, 4);

	if (tbv_apple_tx_requires_exclusive_window(0))
		return 1;
	if (!tbv_apple_tx_requires_exclusive_window(1))
		return 2;
	if (!tbv_apple_tx_requires_exclusive_window(17))
		return 3;

	if (single_charge != 1)
		return 4;
	if (multi_charge != 4)
		return 5;
	if (tbv_apple_tx_frame_charge(17, 0) != 0)
		return 6;

	if (!tbv_apple_tx_window_ok(0, 0, true, 4, 4, single_charge))
		return 7;
	if (tbv_apple_tx_window_ok(1, 0, true, 4, 4, single_charge))
		return 8;
	if (tbv_apple_tx_window_ok(0, 1, true, 4, 4, single_charge))
		return 9;
	if (!tbv_apple_tx_window_ok(0, 0, true, 0, 0, 0))
		return 10;

	if (!tbv_apple_tx_window_ok(0, 0, true, 4, 4, multi_charge))
		return 11;
	if (tbv_apple_tx_window_ok(0, 1, true, 4, 4, multi_charge))
		return 12;

	if (!tbv_apple_tx_window_ok(3, 0, false, 4, 4, 0))
		return 13;
	if (tbv_apple_tx_window_ok(4, 0, false, 4, 4, 0))
		return 14;
	if (tbv_apple_tx_window_ok(-1, 0, true, 4, 4, single_charge))
		return 15;
	if (tbv_apple_tx_window_ok(0, -1, true, 4, 4, single_charge))
		return 16;

	return 0;
}

int main(void)
{
	unsigned char hello_buf[TBV_NATIVE_WIRE_HELLO_MSG_SIZE];
	unsigned char data_buf[TBV_NATIVE_DATA_HDR_SIZE];
	struct tbv_native_wire_hello hello = {
		.capabilities = TBV_NATIVE_WIRE_CAP_UC |
				TBV_NATIVE_WIRE_CAP_RC |
				TBV_NATIVE_WIRE_CAP_MULTI_RAIL,
		.rail_id = 7,
		.route = 0x1122334455667788ull,
		.tx_hop = 8,
		.rx_hop = 9,
		.transmit_path = 10,
		.tx_ring_size = 128,
		.rx_ring_size = 256,
		.path_flags = TBV_NATIVE_WIRE_PATH_FRAME |
			      TBV_NATIVE_WIRE_PATH_E2E,
	};
	struct tbv_native_wire_hello parsed_hello;
	struct tbv_native_wire_info info;
	struct tbv_native_data_header hdr = {
		.opcode = TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM,
		.flags = TBV_NATIVE_DATA_F_LAST | TBV_NATIVE_DATA_F_SOLICITED,
		.dest_qp = 11,
		.src_qp = 12,
		.psn = 13,
		.length = 64,
		.imm_data = 0x58494f01u,
		.remote_addr = 0x8877665544332211ull,
		.rkey = 14,
		.frag_offset = 4096,
	};
	struct tbv_native_data_header parsed_hdr;
	int ret;

	ret = tbv_native_wire_build_hello(hello_buf, sizeof(hello_buf), &hello,
					  TBV_NATIVE_WIRE_OP_HELLO, 0, 123,
					  0xaabbccddeeff0011ull, 2);
	if (ret != (int)sizeof(hello_buf))
		return 1;

	memset(&parsed_hello, 0, sizeof(parsed_hello));
	memset(&info, 0, sizeof(info));
	ret = tbv_native_wire_parse_hello(hello_buf, sizeof(hello_buf),
					  &parsed_hello, &info);
	if (ret)
		return 2;
	if (!hello_equal(&hello, &parsed_hello))
		return 3;
	if (info.op != TBV_NATIVE_WIRE_OP_HELLO || info.seq != 123 ||
	    info.xdomain_sequence != 2)
		return 4;

	ret = tbv_native_data_build_header(data_buf, sizeof(data_buf), &hdr);
	if (ret != (int)sizeof(data_buf))
		return 5;

	memset(&parsed_hdr, 0, sizeof(parsed_hdr));
	ret = tbv_native_data_parse_header(data_buf, sizeof(data_buf),
					   &parsed_hdr);
	if (ret)
		return 6;
	if (!data_header_equal(&hdr, &parsed_hdr))
		return 7;

	if (tbv_native_data_credit_return_threshold(768) !=
	    TBV_NATIVE_DATA_CREDIT_BATCH)
		return 8;
	if (tbv_native_data_credit_return_threshold(16) != 16)
		return 9;
	if (tbv_native_data_start_credit_required(
		    native_data_frags(65536), 768) <= 15)
		return 10;
	if (tbv_native_data_start_credit_required(
		    native_data_frags(32768), 768) <= 7)
		return 11;
	if (tbv_native_data_start_credit_required(65, 768) !=
	    TBV_NATIVE_DATA_CREDIT_BATCH)
		return 12;
	if (tbv_native_data_start_credit_required(65, 16) != 16)
		return 13;
	if (test_raw_stream_payload_header())
		return 14;
	if (test_fragment_shapes())
		return 15;
	if (test_minimal_tbnet_e2e_policy())
		return 16;
	if (test_apple_raw_crc_model())
		return 17;
	if (test_apple_tx_window_policy())
		return 18;

	puts("protocol header smoke OK");
	return 0;
}
