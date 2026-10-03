/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef TBV_CQ_SHM_H
#define TBV_CQ_SHM_H

/*
 * Shared header for the userspace-mapped completion queue (Phase 1 of the
 * userspace-mapped-queues work, 2026-09-12).
 *
 * WHY THIS EXISTS. Every post_send and every poll_cq on this driver used to be
 * a uverbs kernel crossing, and perftest's poll loop made ~96 ioctls per
 * pingpong iteration (measured with perf syscall tracepoints). The fix is the
 * standard userspace-mapped-queue design: the kernel publishes completions
 * into a page shared with the provider, which polls by reading memory instead
 * of issuing ioctls.
 *
 * THE PROTOCOL. Two monotonic totals, one writer each:
 *
 *   Producer (kernel, tcq->lock held):
 *     if (produced - consumed >= cqe) { ovf_seq++; drop; }
 *     entries[tail] = wc;  shm_ring[tail] = uverbs_wc(wc);
 *     smp_wmb();
 *     WRITE_ONCE(shm->produced, produced + 1);
 *     tail = (tail + 1) % cqe;
 *
 *   Consumer (one polling thread, or the kernel's ioctl poll -- never both at
 *   once for a given CQ):
 *     produced = load_acquire(&shm->produced);
 *     while (polled < n && produced - consumed > 0) { out[polled++] = ring[head]; head=(head+1)%cqe; consumed++; }
 *     store_release(&shm->consumed, consumed);
 *
 * WHY TOTALS AND NOT head/tail. The first version published `head` and `tail`
 * and derived occupancy, which is a ring that cannot distinguish full from
 * empty and therefore has to reserve a slot. That is merely inconvenient at
 * cqe == 512 and BROKEN at cqe == 1, which is what perftest actually asks for
 * on its send CQ (measured: `requested=1`): `(tail + 1) % 1 == head` is a
 * tautology, so the CQ declared overflow before storing a single completion.
 * Occupancy is now `produced - consumed` -- exact, unambiguous, and correct at
 * every capacity including one. The subtraction is unsigned, and the ring
 * never lets the difference reach 2^31, so wrap-around is not a hazard.
 *
 * `ovf_seq` is monotonic rather than a sticky flag on purpose. An overflow
 * means a completion was dropped; the consumer must learn that, and it must
 * learn it ONCE. A latched flag makes the CQ permanently dead -- perftest's
 * `do { } while (ne == 0)` spins on it forever -- which is how a single
 * dropped completion became an unkillable `poll on Send CQ failed -1`.
 *
 * Layout: page 0 is this header; the ib_wc ring starts at page 1. The whole
 * buffer is one vmalloc_user() allocation, mapped by the driver's .mmap op.
 */

/*
 * Both sides of this ABI are Linux, and both take the fixed-width types from
 * the same place: the kernel directly, and the userspace provider because
 * rdma-core's <infiniband/kern-abi.h> includes this header too.
 *
 * What this replaced was a userspace branch that declared its own `__u32` and
 * `__aligned_u64`. That is not a shadow, it is a hard error -- rdma-core
 * already defines both (`__aligned_u64` as an aligned `__u64`), so the
 * redefinition collides on `__u64` and the FIRST userspace build of this
 * header failed outright. A header that hand-rolls names the platform already
 * owns cannot be the wire format for a kernel/userspace boundary.
 */
#include <linux/types.h>

#define TBV_CQ_SHM_MAGIC 0x54425643u /* "TBVC" */
#define TBV_CQ_SHM_ABI   2u

struct tbv_cq_shm {
	__u32 magic;       /* TBV_CQ_SHM_MAGIC -- provider sanity-checks this */
	__u32 abi;         /* TBV_CQ_SHM_ABI */
	__u32 cqe;         /* ring capacity in entries */
	__u32 entry_size;  /* sizeof(struct ib_uverbs_wc) -- sanity */
	__u32 ring_offset; /* byte offset of the ring within the mapping */
	__u32 produced;    /* total completions ever queued  (KERNEL writes) */
	__u32 consumed;    /* total completions ever taken  (CONSUMER writes) */
	__u32 ovf_seq;     /* times the kernel dropped an entry (KERNEL writes) */
	__u32 map_len;     /* total bytes the consumer must map (kernel-set) */
	__u32 reserved[39]; /* pad to 192 bytes; room to grow without an abi bump */
};

/* Vendor tail of create_cq's response (after the generic ib_uverbs resp).
 *
 * The kernel fills it only when the provider's outbuf is big enough, so an old
 * provider sees nothing (and never mmaps); a new provider on an old kernel
 * reads all-zero from its own calloc and takes the fallback.
 *
 * `shm_abi` is the offer FLAG, and that is deliberate. The first version keyed
 * the decision on `cq_mmap_offset != 0`, but 0 is a legitimate offset that
 * rdma_user_mmap_entry_insert can hand out (measured: the first CQ on this leg
 * was offered offset 0), so a nonzero test silently discards a real offer.
 * `shm_abi` is zero in a response the kernel never filled and equal to
 * TBV_CQ_SHM_ABI in one it did -- which is exactly the question being asked.
 *
 * `map_len` is published here so the consumer can map the whole buffer in ONE
 * mmap. The first version mapped a single page to discover the length and then
 * remapped; the kernel rejected that first mmap (measured: EINVAL, errno
 * "Invalid argument") and the feature silently fell back to ioctls.
 */
struct tbv_uresp_create_cq {
	__aligned_u64 cq_mmap_offset; /* mmap() offset; may legitimately be 0 */
	__u32 cqe;
	__u32 shm_abi;                /* TBV_CQ_SHM_ABI = the kernel offered */
	__u32 reserved;
	__u32 pad;
	__aligned_u64 map_len;        /* bytes to map; 0 = nothing offered */
};

#endif /* TBV_CQ_SHM_H */
