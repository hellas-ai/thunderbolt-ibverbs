/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
#ifndef TBV_APPLE_TX_H
#define TBV_APPLE_TX_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u32 tbv_apple_tx_u32;
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t tbv_apple_tx_u32;
#endif

static inline tbv_apple_tx_u32
tbv_apple_tx_frame_charge(tbv_apple_tx_u32 frames,
			  tbv_apple_tx_u32 max_frames)
{
	if (!max_frames)
		return 0;
	return frames < max_frames ? frames : max_frames;
}

static inline bool
tbv_apple_tx_requires_exclusive_window(tbv_apple_tx_u32 frames)
{
	/*
	 * Apple FA57 RX frames carry SOF/EOF but no message sequence or
	 * incarnation. Keep every non-empty SEND on an exclusive per-QP window
	 * so Linux does not emit concurrent messages that macOS can
	 * mis-associate or drop under short-message bursts.
	 */
	return frames > 0;
}

static inline bool
tbv_apple_tx_window_ok(int cur_wr, int cur_frames, bool exclusive,
		       unsigned int max_wr, unsigned int max_frames,
		       tbv_apple_tx_u32 frame_charge)
{
	if (cur_wr < 0 || cur_frames < 0)
		return false;

	if (exclusive) {
		if (cur_wr || cur_frames)
			return false;
	} else if (max_wr && cur_wr >= (int)max_wr) {
		return false;
	}

	if (max_frames &&
	    (tbv_apple_tx_u32)cur_frames + frame_charge > max_frames)
		return false;

	return true;
}

#endif /* TBV_APPLE_TX_H */
