// SPDX-License-Identifier: GPL-2.0

#include <linux/jhash.h>
#include <linux/string.h>

#include "tbv.h"

void tbv_rail_key_init(struct tbv_rail_key *key, u64 route,
		       u32 local_adapter, u32 remote_adapter, u32 path_id)
{
	memset(key, 0, sizeof(*key));
	key->route = route;
	key->local_adapter = local_adapter;
	key->remote_adapter = remote_adapter;
	key->path_id = path_id;
}

int tbv_rail_key_cmp(const struct tbv_rail_key *a,
		     const struct tbv_rail_key *b)
{
	if (a->route < b->route)
		return -1;
	if (a->route > b->route)
		return 1;
	if (a->local_adapter < b->local_adapter)
		return -1;
	if (a->local_adapter > b->local_adapter)
		return 1;
	if (a->remote_adapter < b->remote_adapter)
		return -1;
	if (a->remote_adapter > b->remote_adapter)
		return 1;
	if (a->path_id < b->path_id)
		return -1;
	if (a->path_id > b->path_id)
		return 1;
	return 0;
}

u32 tbv_rail_key_hash(const struct tbv_rail_key *key)
{
	u32 words[4];

	words[0] = lower_32_bits(key->route);
	words[1] = upper_32_bits(key->route);
	words[2] = key->local_adapter ^ (key->remote_adapter << 16);
	words[3] = key->path_id;

	return jhash2(words, ARRAY_SIZE(words), 0);
}
