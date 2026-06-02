/* SPDX-License-Identifier: GPL-2.0 */
#ifndef TBV_TRANSPORT_H
#define TBV_TRANSPORT_H

#include "tbv.h"
#include "proto/config.h"

struct tbv_transport_ops {
	enum tbv_backend_type type;
	const char *name;
	int (*validate_config)(const struct tbv_cfg_link *link);
	int (*activate)(struct tbv_state *state, const struct tbv_cfg_link *link);
	void (*deactivate)(struct tbv_state *state,
			   const struct tbv_cfg_link *link);
};

extern const struct tbv_backend_ops tbv_native_backend_ops;
extern const struct tbv_backend_ops tbv_apple_backend_ops;
extern const struct tbv_transport_ops tbv_native_transport_ops;
extern const struct tbv_transport_ops tbv_apple_transport_ops;

const struct tbv_transport_ops *tbv_transport_get(enum tbv_backend_type type);

#endif /* TBV_TRANSPORT_H */
