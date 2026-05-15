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
#define TBV_NATIVE_DATA_MAX_MSG_SIZE \
	(256u * TBV_NATIVE_DATA_MAX_PAYLOAD)

enum tbv_native_data_op {
	TBV_NATIVE_DATA_OP_SEND = 1,
	TBV_NATIVE_DATA_OP_SEND_ACK = 2,
	TBV_NATIVE_DATA_OP_RDMA_WRITE = 3,
	TBV_NATIVE_DATA_OP_RDMA_WRITE_IMM = 4,
	TBV_NATIVE_DATA_OP_RECV_CREDIT = 5,
	TBV_NATIVE_DATA_OP_SEND_IMM = 6,
	TBV_NATIVE_DATA_OP_MAX = TBV_NATIVE_DATA_OP_SEND_IMM,
};

enum tbv_native_data_flag {
	TBV_NATIVE_DATA_F_LAST = 1u << 0,
	TBV_NATIVE_DATA_F_SOLICITED = 1u << 1,
};

struct tbv_native_data_header {
	tbv_wire_u8 opcode;
	tbv_wire_u8 flags;
	tbv_wire_u32 dest_qp;
	tbv_wire_u32 src_qp;
	tbv_wire_u32 psn;
	/* For SEND/SEND_IMM, length is this fragment, imm_data is total
	 * message bytes, remote_addr is the fragment offset, and rkey carries
	 * immediate data for SEND_IMM. For ACK, imm_data is status.
	 */
	tbv_wire_u32 length;
	tbv_wire_u32 imm_data;
	tbv_wire_u64 remote_addr;
	tbv_wire_u32 rkey;
};

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
	if (hdr->length > TBV_NATIVE_DATA_MAX_PAYLOAD)
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

	if (!hdr->opcode || hdr->opcode > TBV_NATIVE_DATA_OP_MAX)
		return -EINVAL;
	if (hdr->length > TBV_NATIVE_DATA_MAX_PAYLOAD)
		return -EMSGSIZE;

	return 0;
}

#endif /* TBV_NATIVE_DATA_H */
