/* SPDX-License-Identifier: GPL-2.0 */
/*
 * usb4_rdma on-wire frame format.
 *
 * Each tb_xdomain ring frame carries a fixed-size header followed by
 * payload. We size the payload so total frame fits in a 4 KiB DMA
 * buffer (header + payload <= 4096), matching what dma_test and
 * thunderbolt_net use as the ring frame size.
 *
 * The header is laid out for natural 8-byte alignment. All multi-byte
 * fields are little-endian (we control both ends of the wire and run
 * on little-endian x86; explicit __le* makes the intent visible if
 * we ever land big-endian).
 *
 * Versioning: bump U4_WIRE_VERSION when the header layout changes.
 * Both peers refuse frames that don't match.
 */

#ifndef _USB4_RDMA_WIRE_H
#define _USB4_RDMA_WIRE_H

#include <linux/types.h>

#define U4_WIRE_MAGIC    0x55344452U  /* 'U4DR' */
#define U4_WIRE_VERSION  1

#define U4_FRAME_SIZE    4096
#define U4_MAX_PAYLOAD   (U4_FRAME_SIZE - sizeof(struct u4_wire_hdr))

enum u4_wire_op {
	U4_OP_SEND     = 1,  /* RC SEND (data delivered to peer's recv queue) */
	U4_OP_SEND_ACK = 2,  /* ACK from receiver back to sender */
	/* Future: U4_OP_RDMA_WRITE, U4_OP_RDMA_READ_REQ/RESP, U4_OP_NAK, ... */
};

enum u4_wire_flag {
	U4_F_LAST    = 1 << 0,  /* last fragment of a multi-frame message */
	U4_F_SOLICITED = 1 << 1,  /* receiver should generate a completion */
};

struct u4_wire_hdr {
	__le32 magic;       /* U4_WIRE_MAGIC for sanity */
	__u8   version;     /* U4_WIRE_VERSION */
	__u8   opcode;      /* enum u4_wire_op */
	__u8   flags;       /* enum u4_wire_flag */
	__u8   reserved8;
	__le32 dest_qp;     /* peer's local QP num */
	__le32 src_qp;      /* sender's local QP num */
	__le32 psn;         /* packet sequence number (per-QP) */
	__le32 length;      /* payload length, bytes */
	__le32 imm_data;    /* immediate data (unused for SEND) */
	__le32 reserved32;  /* pad to 32-byte header */
};

#define U4_HDR_SIZE  ((u32)sizeof(struct u4_wire_hdr))

static inline void u4_wire_hdr_init(struct u4_wire_hdr *h, u8 opcode,
				    u32 dest_qp, u32 src_qp, u32 psn,
				    u32 length, u8 flags)
{
	h->magic    = cpu_to_le32(U4_WIRE_MAGIC);
	h->version  = U4_WIRE_VERSION;
	h->opcode   = opcode;
	h->flags    = flags;
	h->reserved8 = 0;
	h->dest_qp  = cpu_to_le32(dest_qp);
	h->src_qp   = cpu_to_le32(src_qp);
	h->psn      = cpu_to_le32(psn);
	h->length   = cpu_to_le32(length);
	h->imm_data = 0;
	h->reserved32 = 0;
}

/* Returns true if the header looks valid (magic + version match). */
static inline bool u4_wire_hdr_ok(const struct u4_wire_hdr *h)
{
	return le32_to_cpu(h->magic) == U4_WIRE_MAGIC &&
	       h->version == U4_WIRE_VERSION;
}

#endif /* _USB4_RDMA_WIRE_H */
