/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef USB4_RDMA_ABI_H
#define USB4_RDMA_ABI_H

#include <infiniband/kern-abi.h>

/* No driver-specific uapi structs yet; we ride entirely on the
 * generic ib_uverbs_* messages defined in <rdma/ib_user_verbs.h>.
 * When we add custom ops (e.g., GID/UUID exchange, peer addressing)
 * a kernel-side <rdma/usb4_rdma_user.h> will land in include/uapi/rdma/
 * and we'll mirror it here. */

#define USB4_RDMA_ABI_VERSION 1

#endif /* USB4_RDMA_ABI_H */
