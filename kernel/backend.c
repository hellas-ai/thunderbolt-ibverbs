// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/errno.h>

#include "transport.h"

const struct tbv_backend_ops *tbv_backend_get(enum tbv_backend_type type)
{
	switch (type) {
	case TBV_BACKEND_NATIVE:
		return &tbv_native_backend_ops;
	case TBV_BACKEND_APPLE:
		return &tbv_apple_backend_ops;
	default:
		return NULL;
	}
}

const struct tbv_transport_ops *tbv_transport_get(enum tbv_backend_type type)
{
	switch (type) {
	case TBV_BACKEND_NATIVE:
		return &tbv_native_transport_ops;
	case TBV_BACKEND_APPLE:
		return &tbv_apple_transport_ops;
	default:
		return NULL;
	}
}
