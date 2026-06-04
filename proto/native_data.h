/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef TBV_NATIVE_DATA_H
#define TBV_NATIVE_DATA_H

#include "native_wire.h"

#define TBV_NATIVE_DATA_MAGIC		0x31445654u /* "TVD1" little-endian */
#define TBV_NATIVE_DATA_VERSION		1u
#define TBV_NATIVE_DATA_HDR_SIZE	48u
#define TBV_NATIVE_DATA_FRAME_SIZE	4096u
#define TBV_NATIVE_DATA_MAX_PAYLOAD \
	(TBV_NATIVE_DATA_FRAME_SIZE - TBV_NATIVE_DATA_HDR_SIZE)
#define TBV_NATIVE_DATA_MAX_MSG_SIZE	(16u * 1024u * 1024u)
#define TBV_NATIVE_DATA_CREDIT_BATCH	32u
#define TBV_NATIVE_DATA_MAX_FRAGS \
	((TBV_NATIVE_DATA_MAX_MSG_SIZE + TBV_NATIVE_DATA_MAX_PAYLOAD - 1u) / \
	 TBV_NATIVE_DATA_MAX_PAYLOAD)

enum tbv_native_data_op {
	TBV_NATIVE_DATA_OP_SEND = 1,
	TBV_NATIVE_DATA_OP_SEND_ACK = 2,
	TBV_NATIVE_DATA_OP_RDMA_WRITE = 3,
	TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM = 4,
	TBV_NATIVE_DATA_OP_RECV_CREDIT = 5,
	TBV_NATIVE_DATA_OP_SEND_IMM = 6,
	TBV_NATIVE_DATA_OP_RDMA_READ_REQ = 7,
	TBV_NATIVE_DATA_OP_RDMA_READ_RESP = 8,
	TBV_NATIVE_DATA_OP_PATH_CREDIT = 9,
	TBV_NATIVE_DATA_OP_RDMA_READ_ACK = 10,
	TBV_NATIVE_DATA_OP_MAD = 11,
	TBV_NATIVE_DATA_OP_MAX = TBV_NATIVE_DATA_OP_MAD,
};

enum tbv_native_data_flag {
	TBV_NATIVE_DATA_F_LAST = 1u << 0,
	TBV_NATIVE_DATA_F_SOLICITED = 1u << 1,
	TBV_NATIVE_DATA_F_RAW_STREAM = 1u << 2,
};

enum tbv_native_read_ack_status {
	TBV_NATIVE_READ_ACK_OK = 0,
	TBV_NATIVE_READ_ACK_RETRY = 1,
	TBV_NATIVE_READ_ACK_ERROR = 2,
};

enum tbv_native_send_ack_status {
	TBV_NATIVE_SEND_ACK_OK = 0,
	TBV_NATIVE_SEND_ACK_RNR = 1,
	TBV_NATIVE_SEND_ACK_ERROR = 2,
};

struct tbv_native_data_header {
	tbv_wire_u8 opcode;
	tbv_wire_u8 flags;
	tbv_wire_u32 dest_qp;
	tbv_wire_u32 src_qp;
	tbv_wire_u32 psn;
	/*
	 * length is this fragment's payload bytes. frag_offset is this
	 * fragment's byte offset within the operation. For SEND/SEND_IMM,
	 * imm_data is total message bytes, remote_addr is zero, and rkey carries
	 * immediate data for SEND_IMM. For RDMA_WRITE/RDMA_WRITE_IMM,
	 * remote_addr is the base remote IOVA, rkey is the remote key, and
	 * imm_data carries immediate data only for RDMA_WRITE_IMM. For
	 * SEND_ACK, imm_data is enum tbv_native_send_ack_status.
	 */
	tbv_wire_u32 length;
	tbv_wire_u32 imm_data;
	tbv_wire_u64 remote_addr;
	tbv_wire_u32 rkey;
	tbv_wire_u32 frag_offset;
};

static inline tbv_wire_u32
tbv_native_data_credit_return_threshold(tbv_wire_u32 credit_window)
{
	if (credit_window && credit_window < TBV_NATIVE_DATA_CREDIT_BATCH)
		return credit_window;
	return TBV_NATIVE_DATA_CREDIT_BATCH;
}

static inline tbv_wire_u32
tbv_native_data_start_credit_required(tbv_wire_u32 frames,
				      tbv_wire_u32 credit_window)
{
	tbv_wire_u32 threshold;

	if (!frames)
		return 0;

	threshold = tbv_native_data_credit_return_threshold(credit_window);
	return frames < threshold ? frames : threshold;
}

static inline int
tbv_native_data_build_header(void *buf, size_t size,
			     const struct tbv_native_data_header *hdr)
{
	tbv_wire_u8 *p = buf;

	if (!p || !hdr)
		return -EINVAL;
	if (size < TBV_NATIVE_DATA_HDR_SIZE)
		return -ENOSPC;
	if (!hdr->opcode || hdr->opcode > TBV_NATIVE_DATA_OP_MAX)
		return -EINVAL;
	if (hdr->length > ((hdr->flags & TBV_NATIVE_DATA_F_RAW_STREAM) ?
			   TBV_NATIVE_DATA_MAX_MSG_SIZE :
			   TBV_NATIVE_DATA_MAX_PAYLOAD))
		return -EMSGSIZE;

	memset(p, 0, TBV_NATIVE_DATA_HDR_SIZE);
	tbv_wire_put_le32(p, TBV_NATIVE_DATA_MAGIC);
	tbv_wire_put_le16(p + 4, TBV_NATIVE_DATA_VERSION);
	tbv_wire_put_le16(p + 6, TBV_NATIVE_DATA_HDR_SIZE);
	p[8] = hdr->opcode;
	p[9] = hdr->flags;
	tbv_wire_put_le32(p + 12, hdr->dest_qp);
	tbv_wire_put_le32(p + 16, hdr->src_qp);
	tbv_wire_put_le32(p + 20, hdr->psn);
	tbv_wire_put_le32(p + 24, hdr->length);
	tbv_wire_put_le32(p + 28, hdr->imm_data);
	tbv_wire_put_le64(p + 32, hdr->remote_addr);
	tbv_wire_put_le32(p + 40, hdr->rkey);
	tbv_wire_put_le32(p + 44, hdr->frag_offset);

	return TBV_NATIVE_DATA_HDR_SIZE;
}

static inline int
tbv_native_data_parse_header(const void *buf, size_t size,
			     struct tbv_native_data_header *hdr)
{
	const tbv_wire_u8 *p = buf;
	tbv_wire_u16 version;
	tbv_wire_u16 header_size;

	if (!p || !hdr)
		return -EINVAL;
	if (size < TBV_NATIVE_DATA_HDR_SIZE)
		return -EINVAL;
	if (tbv_wire_get_le32(p) != TBV_NATIVE_DATA_MAGIC)
		return -EINVAL;

	version = tbv_wire_get_le16(p + 4);
	header_size = tbv_wire_get_le16(p + 6);
	if (version != TBV_NATIVE_DATA_VERSION ||
	    header_size != TBV_NATIVE_DATA_HDR_SIZE)
		return -EINVAL;

	memset(hdr, 0, sizeof(*hdr));
	hdr->opcode = p[8];
	hdr->flags = p[9];
	hdr->dest_qp = tbv_wire_get_le32(p + 12);
	hdr->src_qp = tbv_wire_get_le32(p + 16);
	hdr->psn = tbv_wire_get_le32(p + 20);
	hdr->length = tbv_wire_get_le32(p + 24);
	hdr->imm_data = tbv_wire_get_le32(p + 28);
	hdr->remote_addr = tbv_wire_get_le64(p + 32);
	hdr->rkey = tbv_wire_get_le32(p + 40);
	hdr->frag_offset = tbv_wire_get_le32(p + 44);

	if (!hdr->opcode || hdr->opcode > TBV_NATIVE_DATA_OP_MAX)
		return -EINVAL;
	if (hdr->length > ((hdr->flags & TBV_NATIVE_DATA_F_RAW_STREAM) ?
			   TBV_NATIVE_DATA_MAX_MSG_SIZE :
			   TBV_NATIVE_DATA_MAX_PAYLOAD))
		return -EMSGSIZE;

	return 0;
}

static inline int
tbv_native_data_raw_payload_header(const struct tbv_native_data_header *stream,
				   tbv_wire_u32 done,
				   tbv_wire_u32 remaining,
				   tbv_wire_u32 payload_len,
				   struct tbv_native_data_header *payload)
{
	if (!stream || !payload)
		return -EINVAL;
	if (!(stream->flags & TBV_NATIVE_DATA_F_RAW_STREAM))
		return -EINVAL;
	if (!payload_len || payload_len > remaining)
		return -EINVAL;

	*payload = *stream;
	payload->flags &= ~(TBV_NATIVE_DATA_F_RAW_STREAM |
			    TBV_NATIVE_DATA_F_LAST |
			    TBV_NATIVE_DATA_F_SOLICITED);
	if (payload_len == remaining)
		payload->flags |= stream->flags &
				  (TBV_NATIVE_DATA_F_LAST |
				   TBV_NATIVE_DATA_F_SOLICITED);
	payload->length = payload_len;
	payload->frag_offset = done;
	return 0;
}

static inline int
tbv_native_data_fragment_shape(tbv_wire_u32 total_len,
			       tbv_wire_u32 max_payload,
			       tbv_wire_u32 max_frags,
			       tbv_wire_u32 offset,
			       tbv_wire_u32 len,
			       bool last,
			       tbv_wire_u32 *frag_idx,
			       tbv_wire_u32 *frag_count)
{
	tbv_wire_u32 idx;
	tbv_wire_u32 count;
	tbv_wire_u32 expected_len;

	if (!max_payload || !max_frags || !frag_idx || !frag_count)
		return -EINVAL;

	if (!total_len) {
		if (offset || len || !last)
			return -EINVAL;
		*frag_idx = 0;
		*frag_count = 1;
		return 0;
	}

	if (offset % max_payload)
		return -EINVAL;
	idx = offset / max_payload;
	count = (total_len + max_payload - 1u) / max_payload;
	if (idx >= count || count > max_frags)
		return -EINVAL;

	expected_len = idx == count - 1 ?
			      total_len - idx * max_payload : max_payload;
	if (len != expected_len)
		return -EINVAL;
	if (last != (idx == count - 1))
		return -EINVAL;

	*frag_idx = idx;
	*frag_count = count;
	return 0;
}

#endif /* TBV_NATIVE_DATA_H */
