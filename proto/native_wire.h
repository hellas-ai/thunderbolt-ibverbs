/* SPDX-License-Identifier: GPL-2.0 */
#ifndef TBV_NATIVE_WIRE_H
#define TBV_NATIVE_WIRE_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
typedef u8 tbv_wire_u8;
typedef u16 tbv_wire_u16;
typedef u32 tbv_wire_u32;
typedef u64 tbv_wire_u64;
#else
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t tbv_wire_u8;
typedef uint16_t tbv_wire_u16;
typedef uint32_t tbv_wire_u32;
typedef uint64_t tbv_wire_u64;
#endif

#define TBV_NATIVE_WIRE_MAGIC		0x31564254u /* "TBV1" little-endian */
#define TBV_NATIVE_WIRE_VERSION		1u
#define TBV_NATIVE_WIRE_XDOMAIN_HDR_SIZE 32u
#define TBV_NATIVE_WIRE_HDR_SIZE	16u
#define TBV_NATIVE_WIRE_HELLO_SIZE	40u
#define TBV_NATIVE_WIRE_HELLO_MSG_SIZE \
	(TBV_NATIVE_WIRE_XDOMAIN_HDR_SIZE + TBV_NATIVE_WIRE_HDR_SIZE + \
	 TBV_NATIVE_WIRE_HELLO_SIZE)

static const tbv_wire_u8 tbv_native_wire_uuid[16] = {
	0x7c, 0x2c, 0x8f, 0x1e, 0x5b, 0x4d, 0x4a, 0x01,
	0x9f, 0x3a, 0x2b, 0x8e, 0x6d, 0x4c, 0x1a, 0x07,
};

enum tbv_native_wire_op {
	TBV_NATIVE_WIRE_OP_HELLO = 1,
	TBV_NATIVE_WIRE_OP_HELLO_ACK = 2,
	TBV_NATIVE_WIRE_OP_READY = 3,
	TBV_NATIVE_WIRE_OP_READY_ACK = 4,
};

enum tbv_native_wire_cap {
	TBV_NATIVE_WIRE_CAP_UC = 1u << 0,
	TBV_NATIVE_WIRE_CAP_RC = 1u << 1,
	TBV_NATIVE_WIRE_CAP_MULTI_RAIL = 1u << 2,
};

enum tbv_native_wire_path_flag {
	TBV_NATIVE_WIRE_PATH_FRAME = 1u << 0,
	TBV_NATIVE_WIRE_PATH_E2E = 1u << 1,
};

struct tbv_native_wire_info {
	tbv_wire_u16 op;
	tbv_wire_u16 flags;
	tbv_wire_u8 xdomain_sequence;
	tbv_wire_u32 seq;
	tbv_wire_u64 route;
};

struct tbv_native_wire_hello {
	tbv_wire_u32 capabilities;
	tbv_wire_u32 rail_id;
	tbv_wire_u64 route;
	tbv_wire_u32 tx_hop;
	tbv_wire_u32 rx_hop;
	tbv_wire_u32 transmit_path;
	tbv_wire_u32 tx_ring_size;
	tbv_wire_u32 rx_ring_size;
	tbv_wire_u32 path_flags;
};

static inline void tbv_wire_put_le16(tbv_wire_u8 *p, tbv_wire_u16 value)
{
	p[0] = value & 0xffu;
	p[1] = value >> 8;
}

static inline void tbv_wire_put_le32(tbv_wire_u8 *p, tbv_wire_u32 value)
{
	p[0] = value & 0xffu;
	p[1] = (value >> 8) & 0xffu;
	p[2] = (value >> 16) & 0xffu;
	p[3] = value >> 24;
}

static inline void tbv_wire_put_le64(tbv_wire_u8 *p, tbv_wire_u64 value)
{
	tbv_wire_put_le32(p, value & 0xffffffffull);
	tbv_wire_put_le32(p + 4, value >> 32);
}

static inline tbv_wire_u16 tbv_wire_get_le16(const tbv_wire_u8 *p)
{
	return (tbv_wire_u16)p[0] | ((tbv_wire_u16)p[1] << 8);
}

static inline tbv_wire_u32 tbv_wire_get_le32(const tbv_wire_u8 *p)
{
	return (tbv_wire_u32)p[0] |
	       ((tbv_wire_u32)p[1] << 8) |
	       ((tbv_wire_u32)p[2] << 16) |
	       ((tbv_wire_u32)p[3] << 24);
}

static inline tbv_wire_u64 tbv_wire_get_le64(const tbv_wire_u8 *p)
{
	return (tbv_wire_u64)tbv_wire_get_le32(p) |
	       ((tbv_wire_u64)tbv_wire_get_le32(p + 4) << 32);
}

static inline void tbv_native_wire_put_header(tbv_wire_u8 *p,
					      tbv_wire_u16 op,
					      tbv_wire_u16 length,
					      tbv_wire_u16 flags,
					      tbv_wire_u32 seq)
{
	tbv_wire_put_le32(p, TBV_NATIVE_WIRE_MAGIC);
	tbv_wire_put_le16(p + 4, TBV_NATIVE_WIRE_VERSION);
	tbv_wire_put_le16(p + 6, op);
	tbv_wire_put_le16(p + 8, length);
	tbv_wire_put_le16(p + 10, flags);
	tbv_wire_put_le32(p + 12, seq);
}

static inline void tbv_native_wire_put_xdomain_header(tbv_wire_u8 *p,
						     tbv_wire_u64 route,
						     tbv_wire_u8 sequence,
						     tbv_wire_u32 type,
						     tbv_wire_u16 size)
{
	tbv_wire_u32 length_sn = (size - 12u) / 4u;

	length_sn |= ((tbv_wire_u32)sequence & 0x3u) << 27;

	tbv_wire_put_le32(p, route >> 32);
	tbv_wire_put_le32(p + 4, route & 0xffffffffull);
	tbv_wire_put_le32(p + 8, length_sn);
	memcpy(p + 12, tbv_native_wire_uuid, sizeof(tbv_native_wire_uuid));
	tbv_wire_put_le32(p + 28, type);
}

static inline int
tbv_native_wire_build_hello(void *buf, size_t size,
			    const struct tbv_native_wire_hello *hello,
			    tbv_wire_u16 op, tbv_wire_u16 flags,
			    tbv_wire_u32 seq, tbv_wire_u64 route,
			    tbv_wire_u8 xdomain_sequence)
{
	tbv_wire_u8 *p = buf;

	if (!p || !hello)
		return -EINVAL;

	if (op != TBV_NATIVE_WIRE_OP_HELLO &&
	    op != TBV_NATIVE_WIRE_OP_HELLO_ACK &&
	    op != TBV_NATIVE_WIRE_OP_READY &&
	    op != TBV_NATIVE_WIRE_OP_READY_ACK)
		return -EINVAL;

	if (size < TBV_NATIVE_WIRE_HELLO_MSG_SIZE)
		return -ENOSPC;

	memset(p, 0, TBV_NATIVE_WIRE_HELLO_MSG_SIZE);
	tbv_native_wire_put_xdomain_header(p, route, xdomain_sequence, op,
					   TBV_NATIVE_WIRE_HELLO_MSG_SIZE);
	p += TBV_NATIVE_WIRE_XDOMAIN_HDR_SIZE;
	tbv_native_wire_put_header(p, op, TBV_NATIVE_WIRE_HELLO_MSG_SIZE,
				   flags, seq);

	p += TBV_NATIVE_WIRE_HDR_SIZE;
	tbv_wire_put_le32(p, hello->capabilities);
	tbv_wire_put_le32(p + 4, hello->rail_id);
	tbv_wire_put_le64(p + 8, hello->route);
	tbv_wire_put_le32(p + 16, hello->tx_hop);
	tbv_wire_put_le32(p + 20, hello->rx_hop);
	tbv_wire_put_le32(p + 24, hello->transmit_path);
	tbv_wire_put_le32(p + 28, hello->tx_ring_size);
	tbv_wire_put_le32(p + 32, hello->rx_ring_size);
	tbv_wire_put_le32(p + 36, hello->path_flags);

	return TBV_NATIVE_WIRE_HELLO_MSG_SIZE;
}

static inline int
tbv_native_wire_parse_hello(const void *buf, size_t size,
			    struct tbv_native_wire_hello *hello,
			    struct tbv_native_wire_info *info)
{
	const tbv_wire_u8 *p = buf;
	tbv_wire_u16 op;
	tbv_wire_u16 length;
	tbv_wire_u32 length_sn;

	if (!p || !hello)
		return -EINVAL;

	if (size < TBV_NATIVE_WIRE_HELLO_MSG_SIZE)
		return -EINVAL;

	length_sn = tbv_wire_get_le32(p + 8);
	if ((length_sn & 0x3fu) != (TBV_NATIVE_WIRE_HELLO_MSG_SIZE - 12u) / 4u)
		return -EINVAL;

	if (memcmp(p + 12, tbv_native_wire_uuid,
		   sizeof(tbv_native_wire_uuid)) != 0)
		return -EINVAL;

	if (tbv_wire_get_le32(p + 28) != TBV_NATIVE_WIRE_OP_HELLO &&
	    tbv_wire_get_le32(p + 28) != TBV_NATIVE_WIRE_OP_HELLO_ACK &&
	    tbv_wire_get_le32(p + 28) != TBV_NATIVE_WIRE_OP_READY &&
	    tbv_wire_get_le32(p + 28) != TBV_NATIVE_WIRE_OP_READY_ACK)
		return -EINVAL;

	if (info) {
		info->xdomain_sequence = (length_sn >> 27) & 0x3u;
		info->route = ((tbv_wire_u64)tbv_wire_get_le32(p) << 32) |
			      tbv_wire_get_le32(p + 4);
		info->route &= ~(1ull << 63);
	}

	p += TBV_NATIVE_WIRE_XDOMAIN_HDR_SIZE;
	if (tbv_wire_get_le32(p) != TBV_NATIVE_WIRE_MAGIC)
		return -EINVAL;

	if (tbv_wire_get_le16(p + 4) != TBV_NATIVE_WIRE_VERSION)
		return -EINVAL;

	op = tbv_wire_get_le16(p + 6);
	if (op != TBV_NATIVE_WIRE_OP_HELLO &&
	    op != TBV_NATIVE_WIRE_OP_HELLO_ACK &&
	    op != TBV_NATIVE_WIRE_OP_READY &&
	    op != TBV_NATIVE_WIRE_OP_READY_ACK)
		return -EINVAL;

	if (op != tbv_wire_get_le32((const tbv_wire_u8 *)buf + 28))
		return -EINVAL;

	length = tbv_wire_get_le16(p + 8);
	if (length != TBV_NATIVE_WIRE_HELLO_MSG_SIZE || size < length)
		return -EINVAL;

	if (info) {
		info->op = op;
		info->flags = tbv_wire_get_le16(p + 10);
		info->seq = tbv_wire_get_le32(p + 12);
	}

	p += TBV_NATIVE_WIRE_HDR_SIZE;
	hello->capabilities = tbv_wire_get_le32(p);
	hello->rail_id = tbv_wire_get_le32(p + 4);
	hello->route = tbv_wire_get_le64(p + 8);
	hello->tx_hop = tbv_wire_get_le32(p + 16);
	hello->rx_hop = tbv_wire_get_le32(p + 20);
	hello->transmit_path = tbv_wire_get_le32(p + 24);
	hello->tx_ring_size = tbv_wire_get_le32(p + 28);
	hello->rx_ring_size = tbv_wire_get_le32(p + 32);
	hello->path_flags = tbv_wire_get_le32(p + 36);

	return 0;
}

#endif
