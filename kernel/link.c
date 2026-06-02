// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: link: " fmt

#include <linux/errno.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "tbv.h"
#include "proto/config.h"

static struct tbv_configured_link *
tbv_link_find_locked(struct tbv_state *state, u32 link_id)
{
	struct tbv_configured_link *link;

	list_for_each_entry(link, &state->configured_links, node) {
		if (link->link_id == link_id)
			return link;
	}

	return NULL;
}

int tbv_link_activate_config(struct tbv_state *state, const char *name,
			     enum tbv_backend_type backend,
			     const struct tbv_cfg_link *cfg)
{
	struct tbv_configured_link *link;

	if (!cfg->app_selection.valid)
		return -EINVAL;

	link = kzalloc(sizeof(*link), GFP_KERNEL);
	if (!link)
		return -ENOMEM;

	link->link_id = cfg->link_id;
	link->backend = backend;
	link->app_selection = cfg->app_selection;
	strscpy(link->name, name, sizeof(link->name));

	mutex_lock(&state->lock);
	if (tbv_link_find_locked(state, cfg->link_id)) {
		mutex_unlock(&state->lock);
		kfree(link);
		return -EEXIST;
	}
	list_add_tail(&link->node, &state->configured_links);
	state->configured_link_count++;
	mutex_unlock(&state->lock);

	pr_info("published %s link=%u backend=%s dev=%u gid=%u\n",
		link->name, link->link_id, tbv_backend_name(link->backend),
		link->app_selection.device_id, link->app_selection.gid_index);
	return 0;
}

void tbv_link_deactivate_config(struct tbv_state *state, u32 link_id)
{
	struct tbv_configured_link *link;

	mutex_lock(&state->lock);
	link = tbv_link_find_locked(state, link_id);
	if (link) {
		list_del(&link->node);
		state->configured_link_count--;
	}
	mutex_unlock(&state->lock);

	if (!link)
		return;

	pr_info("unpublished %s link=%u backend=%s\n",
		link->name, link->link_id, tbv_backend_name(link->backend));
	kfree(link);
}

u32 tbv_link_count(struct tbv_state *state)
{
	u32 count;

	mutex_lock(&state->lock);
	count = state->configured_link_count;
	mutex_unlock(&state->lock);

	return count;
}

void tbv_link_debugfs_show(struct seq_file *s, struct tbv_state *state)
{
	struct tbv_configured_link *link;

	mutex_lock(&state->lock);
	list_for_each_entry(link, &state->configured_links, node) {
		const struct tbv_id_selection *sel = &link->app_selection;

		seq_printf(s,
			   "link %u name=%s backend=%s dev=%u port=%u gid=%u gid_type=%u addr=%u.%u.%u.%u\n",
			   link->link_id, link->name,
			   tbv_backend_name(link->backend),
			   sel->device_id, sel->port, sel->gid_index,
			   sel->gid_type, sel->addr.bytes[12],
			   sel->addr.bytes[13], sel->addr.bytes[14],
			   sel->addr.bytes[15]);
	}
	mutex_unlock(&state->lock);
}
