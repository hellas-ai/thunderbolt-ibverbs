/* SPDX-License-Identifier: GPL-2.0 */
#include <assert.h>
#include "../kernel/ring_diag.h"

int main(void)
{
	assert(tbv_diag_hw_index(0x1234abcdU, 1) == 0xabcdU);
	assert(tbv_diag_hw_index(0x1234abcdU, 0) == 0x1234U);
	assert(tbv_diag_desc_flags(0xabc12345U) == 0xabcU);
	assert(tbv_diag_e2e_hop(0x10555000U) == 0x555U);
	assert(tbv_diag_hw_index(0xffffffffU, 1) == 65535U);
	assert(tbv_diag_hw_index(0, 0) == 0);
	return 0;
}
