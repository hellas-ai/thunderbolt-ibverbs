/* SPDX-License-Identifier: GPL-2.0 */
#ifndef TBV_RING_DIAG_H
#define TBV_RING_DIAG_H

/* PCI NHI layout: Linux drivers/thunderbolt/nhi_regs.h. */
static inline unsigned int tbv_diag_hw_index(unsigned int raw, int is_tx)
{
	return is_tx ? raw & 0xffffU : raw >> 16;
}

static inline unsigned int tbv_diag_desc_flags(unsigned int ctrl)
{
	return ctrl >> 20;
}

static inline unsigned int tbv_diag_e2e_hop(unsigned int options)
{
	return (options >> 12) & 0x7ffU;
}

#endif
