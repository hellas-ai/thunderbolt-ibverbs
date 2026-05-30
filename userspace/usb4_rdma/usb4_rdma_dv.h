/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef USB4_RDMA_DV_H
#define USB4_RDMA_DV_H

#include <linux/types.h>

/*
 * USB4 RDMA Direct Verbs / GDA ABI, v2.
 *
 * ABI version policy:
 * - abi_version 0 is invalid.
 * - v1 is the first experimental queue ABI.
 * - v2 adds RDMA_READ and software-backed 64-bit atomics for rocSHMEM
 *   signal/counter operations. Atomics are executed by the target kernel
 *   against the target MR under provider-side serialization; USB4 does not
 *   expose hardware RNIC atomics.
 * - v2 also defines USB4_RDMA_DV_WQE_F_LOCAL_LOOPBACK for provider-known
 *   self-target atomics. When set on an atomic WQE, the kernel must validate
 *   that the destination QP and target MR are local and then execute the atomic
 *   locally instead of emitting a native-data packet. This is required for
 *   rocSHMEM self-polling barriers because the selected RoCE GID may be an
 *   IPv4/IPv6 netdev GID rather than the driver's synthetic link-local GID.
 * - A method input carrying abi_version must exactly match a version returned
 *   by QUERY_CAPS unless that method explicitly documents compatible older
 *   input.
 * - New struct fields may be appended behind existing reserved fields only if
 *   zero/default semantics keep old userspace working. Any reinterpretation of
 *   existing fields requires an ABI version bump.
 *
 * This is not a hardware RNIC ABI. The GPU produces software-RNIC WQEs into
 * host-visible memory. The kernel owns the Thunderbolt/NHI rings, consumes
 * these WQEs, performs the existing native SEND/WRITE data path, and produces
 * CQEs back into host-visible memory.
 *
 * Queue-memory requirements:
 * - SQ, CQ, and doorbell pages must be host-visible and coherent for CPU/GPU
 *   system-scope atomics. On ROCm the validated queue-memory allocators are
 *   hipMallocManaged, hipHostMalloc(..., hipHostMallocMapped),
 *   hipHostMallocCoherent, hipHostMallocUncached, and HSA CPU-location
 *   fine-grained or extended-scope fine-grained pools with CPU/GPU agent access
 *   granted. hipHostMallocNonCoherent is not valid queue memory; it can
 *   silently corrupt producer/consumer handshakes. HSA GPU-location pools are
 *   not valid queue memory for v1 unless they can also be registered through
 *   the RDMA MR path.
 * - v1 queue memory is allocated by userspace and pinned by the kernel during
 *   CREATE_QUEUE. The kernel does not allocate or mmap queue pages in v1.
 * - The 32-bit queue head/tail words must live in a dedicated doorbell page,
 *   separate from WQE/CQE arrays. The kernel poller watches SQ tail words, not
 *   descriptor cachelines. v1 uses one 128-byte doorbell record per QP inside a
 *   dedicated 4 KiB page-sized mapping.
 * - The doorbell record is split into two 64-byte cachelines. The producer line
 *   contains GPU-written/kernel-read fields. The consumer line contains
 *   kernel-written/GPU-read fields. Do not move fields across those lines
 *   without an ABI bump.
 *
 * Producer rules for GPU -> kernel SQ:
 *   store WQE generation matching the current QP generation
 *   store WQE fields
 *   system-scope release fence/atomic
 *   system-scope atomic store of producer.sq_tail with the same generation
 *
 * Consumer rules for kernel reading SQ:
 *   smp_load_acquire(producer.sq_tail)
 *   reject if the packed generation or producer.generation is stale
 *   read WQE fields
 *   reject any WQE whose generation is stale
 *   smp_store_release(consumer.sq_head) after consuming WQEs
 *
 * Producer rules for kernel -> GPU CQ:
 *   store CQE fields
 *   smp_store_release(consumer.cq_tail)
 *
 * Consumer rules for GPU reading CQ:
 *   system-scope acquire load of consumer.cq_tail
 *   read CQE fields
 *   system-scope release store of producer.cq_head after consuming CQEs
 *
 * Generation protocol:
 * - CREATE_QUEUE returns generation X and initializes consumer.generation to X.
 * - Before producing WQEs, userspace must mirror X into producer.generation.
 * - All SQ/CQ head and tail words use usb4_rdma_dv_tail_pack(). The index part
 *   is a monotonically increasing producer/consumer counter modulo 2^24; the
 *   ring slot is index % queue_depth. The generation part is X.
 * - The kernel rejects producer.sq_tail, producer.cq_head, or any WQE whose
 *   embedded generation differs from X.
 * - DESTROY_QUEUE bumps consumer.generation before teardown completes, so stale
 *   producer writes from an old QP lifetime are detectable by later drain/poll
 *   attempts.
 * - DESTROY_QUEUE is a teardown operation, not a drain operation. v1 discards
 *   in-flight or not-yet-completed DV WQEs without producing FLUSH_ERR CQEs.
 *   Consumers that require completions must stop producing, wait for SQ head
 *   and CQ tail to catch up, and only then destroy the queue.
 *
 * v1 does not define a GPU-produced receive queue. DV owns only the
 * GPU-produced send queue and DV completion queue surface. The receive queue
 * stays kernel-owned, and the peer may post receive WQEs through standard
 * verbs while the QP has an active DV queue. SEND/SEND_IMM and
 * RDMA_WRITE_WITH_IMM consume peer receive WQEs and complete on the peer's
 * normal verbs receive CQ. RDMA_WRITE, RDMA_READ, and atomics do not consume
 * a peer receive WQE.
 *
 * CQE ordering and errors:
 * - v2 produces CQEs in SQ WQE order. Internal native SEND/WRITE/READ/ATOMIC
 *   completions may arrive out of order, but the kernel buffers them until
 *   prior WQEs have completed or been skipped as unsignaled successes.
 * - Transient transport admission pressure is not a WQE error. If the native
 *   path cannot currently reserve local TX queue/ring resources, the kernel
 *   leaves SQ head on the blocked WQE, produces no CQE, and retries on a later
 *   poll scan. Later WQEs on the same QP are not consumed until that WQE is
 *   admitted, preserving per-QP SEND/WRITE ordering. Other QPs may continue to
 *   drain in round-robin order.
 * - USB4_RDMA_DV_WQE_F_FENCE is a software admission fence. A fenced WQE is
 *   not admitted to the native transport until all earlier DV WQEs on the same
 *   QP have completed and the ordered DV completion stream has advanced past
 *   them. This provides an explicit software ordering primitive for future
 *   compound protocols; it is not intended to make a high-rate producer spin on
 *   sender-side admission retries.
 * - A WQE generation mismatch is observable: the kernel writes a CQE with
 *   USB4_RDMA_DV_CQE_STALE_GEN, advances SQ head past that WQE, and leaves the
 *   QP live.
 * - CQ overflow is fatal for the DV queue. The kernel moves the QP to ERR,
 *   writes consumer.qp_state = USB4_RDMA_DV_QP_ERR if the doorbell page is
 *   still mapped, and stops posting DV CQEs. If there is no CQ slot available,
 *   no sentinel CQE is guaranteed.
 */

#define USB4_RDMA_DV_ABI_VERSION 2

#define USB4_RDMA_DV_MIN_QUEUE_ENTRIES 2
#define USB4_RDMA_DV_MAX_SQ_ENTRIES 1024
#define USB4_RDMA_DV_MAX_CQ_ENTRIES 4096
#define USB4_RDMA_DV_DEFAULT_SQ_ENTRIES 256
#define USB4_RDMA_DV_DEFAULT_CQ_ENTRIES 512
#define USB4_RDMA_DV_WQE_SIZE 64 /* bytes */
#define USB4_RDMA_DV_CQE_SIZE 64 /* bytes */
#define USB4_RDMA_DV_DOORBELL_LINE_SIZE 64 /* bytes */
#define USB4_RDMA_DV_DOORBELL_RECORD_SIZE \
	(2 * USB4_RDMA_DV_DOORBELL_LINE_SIZE) /* bytes */
#define USB4_RDMA_DV_DOORBELL_PAGE_SIZE 4096 /* bytes */

/*
 * Private uverbs ids. These use the RDMA core driver namespace bit
 * (UVERBS_ID_DRIVER_NS / UVERBS_API_NS_FLAG == 1 << 12), but keep the value
 * local so this header can be included by small standalone probes without
 * pulling in rdma-core's private ioctl helper headers.
 */
#define USB4_RDMA_DV_DRIVER_NS (1u << 12)

enum usb4_rdma_dv_objects {
	USB4_RDMA_DV_OBJECT_DEVICE = USB4_RDMA_DV_DRIVER_NS,
};

enum usb4_rdma_dv_methods {
	USB4_RDMA_DV_METHOD_QUERY_CAPS = USB4_RDMA_DV_DRIVER_NS,
	USB4_RDMA_DV_METHOD_CREATE_QUEUE,
	USB4_RDMA_DV_METHOD_DESTROY_QUEUE,
	USB4_RDMA_DV_METHOD_KICK,
};

enum usb4_rdma_dv_query_caps_attrs {
	USB4_RDMA_DV_ATTR_QUERY_CAPS_RESP = USB4_RDMA_DV_DRIVER_NS,
};

enum usb4_rdma_dv_create_queue_attrs {
	USB4_RDMA_DV_ATTR_CREATE_QUEUE_QP = USB4_RDMA_DV_DRIVER_NS,
	USB4_RDMA_DV_ATTR_CREATE_QUEUE_REQ,
	USB4_RDMA_DV_ATTR_CREATE_QUEUE_RESP,
};

enum usb4_rdma_dv_destroy_queue_attrs {
	USB4_RDMA_DV_ATTR_DESTROY_QUEUE_QP = USB4_RDMA_DV_DRIVER_NS,
};

enum usb4_rdma_dv_kick_attrs {
	USB4_RDMA_DV_ATTR_KICK_QP = USB4_RDMA_DV_DRIVER_NS,
	USB4_RDMA_DV_ATTR_KICK_REQ,
};

#define USB4_RDMA_DV_TAIL_INDEX_BITS 24
#define USB4_RDMA_DV_TAIL_INDEX_MASK ((1u << USB4_RDMA_DV_TAIL_INDEX_BITS) - 1)
#define USB4_RDMA_DV_TAIL_GEN_SHIFT USB4_RDMA_DV_TAIL_INDEX_BITS
#define USB4_RDMA_DV_TAIL_GENERATION_BITS \
	(32 - USB4_RDMA_DV_TAIL_INDEX_BITS)

static inline __u32 usb4_rdma_dv_tail_pack(__u32 index, __u8 generation)
{
	return (index & USB4_RDMA_DV_TAIL_INDEX_MASK) |
	       ((__u32)generation << USB4_RDMA_DV_TAIL_GEN_SHIFT);
}

static inline __u32 usb4_rdma_dv_tail_index(__u32 packed)
{
	return packed & USB4_RDMA_DV_TAIL_INDEX_MASK;
}

static inline __u8 usb4_rdma_dv_tail_generation(__u32 packed)
{
	return packed >> USB4_RDMA_DV_TAIL_GEN_SHIFT;
}

enum usb4_rdma_dv_caps {
	USB4_RDMA_DV_CAP_SEND = 1u << 0,
	USB4_RDMA_DV_CAP_SEND_IMM = 1u << 1,
	USB4_RDMA_DV_CAP_WRITE = 1u << 2,
	USB4_RDMA_DV_CAP_WRITE_IMM = 1u << 3,
	USB4_RDMA_DV_CAP_FENCE = 1u << 4,
	USB4_RDMA_DV_CAP_READ = 1u << 5,
	USB4_RDMA_DV_CAP_ATOMIC_FETCH_ADD = 1u << 6,
	USB4_RDMA_DV_CAP_ATOMIC_SWAP = 1u << 7,
	USB4_RDMA_DV_CAP_ATOMIC_CMP_SWAP = 1u << 8,
};

#define USB4_RDMA_DV_CAPS_V2 \
	(USB4_RDMA_DV_CAP_SEND | \
	 USB4_RDMA_DV_CAP_SEND_IMM | \
	 USB4_RDMA_DV_CAP_WRITE | \
	 USB4_RDMA_DV_CAP_WRITE_IMM | \
	 USB4_RDMA_DV_CAP_FENCE | \
	 USB4_RDMA_DV_CAP_READ | \
	 USB4_RDMA_DV_CAP_ATOMIC_FETCH_ADD | \
	 USB4_RDMA_DV_CAP_ATOMIC_SWAP | \
	 USB4_RDMA_DV_CAP_ATOMIC_CMP_SWAP)

#define USB4_RDMA_DV_CAPS_V1 \
	(USB4_RDMA_DV_CAP_SEND | \
	 USB4_RDMA_DV_CAP_SEND_IMM | \
	 USB4_RDMA_DV_CAP_WRITE | \
	 USB4_RDMA_DV_CAP_WRITE_IMM | \
	 USB4_RDMA_DV_CAP_FENCE)

enum usb4_rdma_dv_wqe_opcode {
	USB4_RDMA_DV_WQE_NOP = 0,
	USB4_RDMA_DV_WQE_SEND = 1,
	USB4_RDMA_DV_WQE_SEND_IMM = 2,
	USB4_RDMA_DV_WQE_RDMA_WRITE = 3,
	USB4_RDMA_DV_WQE_RDMA_WRITE_IMM = 4,
	USB4_RDMA_DV_WQE_RDMA_READ = 5,
	USB4_RDMA_DV_WQE_ATOMIC_FETCH_ADD = 6,
	USB4_RDMA_DV_WQE_ATOMIC_SWAP = 7,
	USB4_RDMA_DV_WQE_ATOMIC_CMP_SWAP = 8,
};

enum usb4_rdma_dv_wqe_flags {
	USB4_RDMA_DV_WQE_F_SIGNALED = 1u << 0,
	USB4_RDMA_DV_WQE_F_SOLICITED = 1u << 1,
	USB4_RDMA_DV_WQE_F_FENCE = 1u << 2,
	USB4_RDMA_DV_WQE_F_LOCAL_LOOPBACK = 1u << 3,
};

enum usb4_rdma_dv_cqe_status {
	USB4_RDMA_DV_CQE_SUCCESS = 0,
	USB4_RDMA_DV_CQE_WR_FLUSH_ERR = 1,
	USB4_RDMA_DV_CQE_LOCAL_LEN_ERR = 2,
	USB4_RDMA_DV_CQE_LOCAL_PROT_ERR = 3,
	USB4_RDMA_DV_CQE_REMOTE_ACCESS_ERR = 4,
	USB4_RDMA_DV_CQE_RETRY_EXC_ERR = 5,
	USB4_RDMA_DV_CQE_STALE_GEN = 6,
	USB4_RDMA_DV_CQE_GENERAL_ERR = 255,
};

enum usb4_rdma_dv_qp_state {
	USB4_RDMA_DV_QP_LIVE = 0,
	USB4_RDMA_DV_QP_ERR = 1,
	USB4_RDMA_DV_QP_DEAD = 2,
};

/*
 * v2 WQE format:
 * - exactly one SGE
 * - no inline data
 * - SEND, SEND_WITH_IMM, RDMA_WRITE, RDMA_WRITE_WITH_IMM, RDMA_READ
 * - 64-bit ATOMIC_FETCH_ADD, ATOMIC_SWAP, and ATOMIC_CMP_SWAP
 *
 * For atomics, length must be 8 and local_addr/lkey identify an 8-byte local
 * result slot. FETCH_ADD and SWAP use reserved1[0] as the add/swap value.
 * CMP_SWAP uses reserved1[0] as the swap value and reserved1[1] as the compare
 * value. The returned value is the original remote value. Users that do not
 * need the returned value must still provide a valid 8-byte local result slot;
 * providers may reuse a scratch MR for that case.
 */
struct usb4_rdma_dv_wqe {
	__u16 opcode;
	__u16 flags;
	__u32 length;
	__u64 wr_id;
	__u64 local_addr;
	__u32 lkey;
	__u32 rkey;
	__u64 remote_addr;
	__u32 imm_data;
	__u32 generation;
	__u64 reserved1[2];
} __attribute__((packed, aligned(64)));

struct usb4_rdma_dv_cqe {
	__u64 wr_id;
	__u32 status;
	__u32 opcode;
	__u32 byte_len;
	__u32 imm_data;
	__u32 vendor_err;
	__u32 qp_state;
	__u64 reserved[4];
} __attribute__((packed, aligned(64)));

/*
 * Doorbell page layout for one queue pair. Each 32-bit word is independently
 * polled with acquire semantics by its consumer. A stale GPU write from a torn
 * down QP is detected by the explicit generation fields and by the generation
 * bits in packed queue indexes.
 */
struct usb4_rdma_dv_doorbell_producer_line {
	__u32 sq_tail;
	__u32 cq_head;
	__u32 generation;
	__u32 reserved0;
	__u64 reserved1[6];
} __attribute__((packed, aligned(64)));

struct usb4_rdma_dv_doorbell_consumer_line {
	__u32 sq_head;
	__u32 cq_tail;
	__u32 qp_state;
	__u32 generation;
	__u64 reserved1[6];
} __attribute__((packed, aligned(64)));

struct usb4_rdma_dv_doorbell {
	struct usb4_rdma_dv_doorbell_producer_line producer;
	struct usb4_rdma_dv_doorbell_consumer_line consumer;
} __attribute__((packed, aligned(64)));

/*
 * Capability fields are ceilings and recommendations, not fixed allocations.
 * Userspace requests actual depths in CREATE_QUEUE. The kernel rejects depths
 * below USB4_RDMA_DV_MIN_QUEUE_ENTRIES or above max_*_entries.
 *
 * On the current two-rail USB4 software-RNIC path, 256 SQ entries and 512 CQ
 * entries are the recommended starting point. Deeper queues are exposed as a
 * cap for consumers that deliberately batch or multiplex more work, not
 * because the Thunderbolt wire needs 1024 WQEs in flight to fill its BDP.
 */
struct usb4_rdma_dv_query_caps_resp {
	__u32 abi_version;
	__u32 caps;
	__u32 max_sq_entries;
	__u32 max_cq_entries;
	__u32 default_sq_entries;
	__u32 default_cq_entries;
	__u32 wqe_size;
	__u32 cqe_size;
	__u32 doorbell_record_size;
	__u32 doorbell_page_size;
	__u32 tail_index_bits;
	__u32 tail_generation_bits;
	__u32 reserved[4];
};

/*
 * Queue addresses are userspace virtual addresses of host-visible coherent
 * memory. The kernel pins these ranges during CREATE_QUEUE and releases them
 * during DESTROY_QUEUE/QP destroy; v1 does not require separate MR keys for
 * SQ/CQ/doorbell queue memory.
 * sq_stride/cq_stride are in bytes and must equal the caps-reported WQE/CQE
 * sizes for v1. doorbell_addr is page-aligned; the first record in that page
 * is a struct usb4_rdma_dv_doorbell.
 */
struct usb4_rdma_dv_queue_create {
	__u32 abi_version;
	__u32 flags;
	__u64 sq_addr;
	__u64 cq_addr;
	__u64 doorbell_addr;
	__u32 sq_entries;
	__u32 cq_entries;
	__u32 sq_stride;
	__u32 cq_stride;
	__u32 reserved[4];
};

struct usb4_rdma_dv_queue_resp {
	__u32 qp_num;
	__u32 generation;
	__u32 flags;
	__u32 reserved0;
};

/*
 * Optional explicit-kick v1 path. KICK is a drain trigger: the handler
 * consumes all WQEs between the current SQ head and sq_tail, then returns. If
 * SQ head already equals sq_tail, KICK is a cheap no-op. Kernel-poll mode uses
 * the same drain operation after observing producer.sq_tail directly; KICK is
 * not required when polling is enabled. sq_tail is the packed tail value
 * userspace already published to producer.sq_tail.
 */
struct usb4_rdma_dv_kick {
	__u32 sq_tail;
	__u32 flags;
	__u32 reserved[2];
};

#endif /* USB4_RDMA_DV_H */
