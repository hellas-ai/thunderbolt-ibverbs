/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef USB4_RDMA_H
#define USB4_RDMA_H

#include <inttypes.h>
#include <stddef.h>
#include <pthread.h>

#include <infiniband/driver.h>
#include <infiniband/kern-abi.h>

#include "usb4_rdma_abi.h"

struct usb4_rdma_device {
	struct verbs_device base_dev;
};

struct usb4_rdma_context {
	struct verbs_context base;
};

struct usb4_rdma_pd {
	struct ibv_pd base;
};

#endif /* USB4_RDMA_H */
