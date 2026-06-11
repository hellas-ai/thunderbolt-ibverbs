/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef TBV_TBNET_H
#define TBV_TBNET_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u32 tbv_tbnet_u32;
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t tbv_tbnet_u32;
#endif

enum tbv_tbnet_prtcstns {
	TBV_TBNET_PRTCSTNS_E2E = 1u << 0,
	TBV_TBNET_PRTCSTNS_MATCH_FRAGS_ID = 1u << 1,
	TBV_TBNET_PRTCSTNS_64K_FRAMES = 1u << 2,
};

#define TBV_TBNET_MINIMAL_BASE_PRTCSTNS \
	(TBV_TBNET_PRTCSTNS_MATCH_FRAGS_ID | TBV_TBNET_PRTCSTNS_64K_FRAMES)

static inline tbv_tbnet_u32 tbv_tbnet_minimal_prtcstns(bool local_e2e)
{
	tbv_tbnet_u32 prtcstns = TBV_TBNET_MINIMAL_BASE_PRTCSTNS;

	if (local_e2e)
		prtcstns |= TBV_TBNET_PRTCSTNS_E2E;

	return prtcstns;
}

static inline bool tbv_tbnet_minimal_e2e_enabled(bool local_e2e,
						 tbv_tbnet_u32 peer_prtcstns)
{
	return local_e2e &&
	       (peer_prtcstns & TBV_TBNET_PRTCSTNS_E2E) != 0;
}

#endif /* TBV_TBNET_H */
