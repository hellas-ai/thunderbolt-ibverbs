// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: configfs: " fmt

#include <linux/atomic.h>
#include <linux/configfs.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "transport.h"

struct tbv_cfgfs_link {
	struct config_item item;
	struct mutex lock;
	struct tbv_state *state;
	struct tbv_cfg_link cfg;
	struct tbv_id_route staged_route;
	bool local_src_set;
	bool peer_set;
};

static struct configfs_subsystem tbv_cfgfs_subsys;
static atomic_t tbv_cfgfs_next_link_id = ATOMIC_INIT(0);
static struct tbv_state *tbv_cfgfs_state;

static struct tbv_cfgfs_link *tbv_cfgfs_to_link(struct config_item *item)
{
	return container_of(item, struct tbv_cfgfs_link, item);
}

static const char *tbv_cfgfs_state_name(u8 state)
{
	switch (state) {
	case TBV_CFG_EMPTY:
		return "empty";
	case TBV_CFG_DRAFT:
		return "draft";
	case TBV_CFG_SEALED:
		return "sealed";
	case TBV_CFG_ACTIVE:
		return "active";
	default:
		return "unknown";
	}
}

static const char *tbv_cfgfs_backend_name(u8 backend)
{
	switch (backend) {
	case TBV_CFG_BACKEND_NATIVE:
		return "native";
	case TBV_CFG_BACKEND_APPLE:
		return "apple";
	default:
		return "unset";
	}
}

static const char *tbv_cfgfs_gid_type_name(u8 gid_type)
{
	switch (gid_type) {
	case TBV_ID_GID_ROCE_V1:
		return "roce-v1";
	case TBV_ID_GID_ROCE_V2:
		return "roce-v2";
	case TBV_ID_GID_IB:
		return "ib";
	default:
		return "unknown";
	}
}

static int tbv_cfgfs_parse_ipv4(const char *buf, struct tbv_id_addr *addr)
{
	unsigned int a, b, c, d;

	if (sscanf(buf, "%u.%u.%u.%u", &a, &b, &c, &d) != 4)
		return -EINVAL;
	if (a > 255 || b > 255 || c > 255 || d > 255)
		return -EINVAL;

	tbv_id_addr_v4(addr, a, b, c, d);
	return 0;
}

static ssize_t tbv_cfgfs_format_ipv4(char *buf, const struct tbv_id_addr *addr)
{
	if (addr->family != TBV_ID_AF_INET)
		return sysfs_emit(buf, "<unset>\n");

	return sysfs_emit(buf, "%u.%u.%u.%u\n",
			  addr->bytes[12], addr->bytes[13],
			  addr->bytes[14], addr->bytes[15]);
}

static int tbv_cfgfs_apply_route_locked(struct tbv_cfgfs_link *link)
{
	if (!link->local_src_set || !link->peer_set)
		return 0;

	return tbv_cfg_link_set_route(&link->cfg, &link->staged_route);
}

static ssize_t backend_show(struct config_item *item, char *buf)
{
	struct tbv_cfgfs_link *link = tbv_cfgfs_to_link(item);
	ssize_t ret;

	mutex_lock(&link->lock);
	ret = sysfs_emit(buf, "%s\n", tbv_cfgfs_backend_name(link->cfg.backend));
	mutex_unlock(&link->lock);
	return ret;
}

static ssize_t backend_store(struct config_item *item, const char *buf,
			     size_t count)
{
	struct tbv_cfgfs_link *link = tbv_cfgfs_to_link(item);
	u8 backend;
	int ret;

	if (sysfs_streq(buf, "native"))
		backend = TBV_CFG_BACKEND_NATIVE;
	else if (sysfs_streq(buf, "apple"))
		backend = TBV_CFG_BACKEND_APPLE;
	else
		return -EINVAL;

	mutex_lock(&link->lock);
	ret = tbv_cfg_link_set_backend(&link->cfg, backend);
	mutex_unlock(&link->lock);

	return ret ? ret : count;
}

static ssize_t local_src_ipv4_show(struct config_item *item, char *buf)
{
	struct tbv_cfgfs_link *link = tbv_cfgfs_to_link(item);
	ssize_t ret;

	mutex_lock(&link->lock);
	ret = tbv_cfgfs_format_ipv4(buf, &link->staged_route.local_src_addr);
	mutex_unlock(&link->lock);
	return ret;
}

static ssize_t local_src_ipv4_store(struct config_item *item, const char *buf,
				    size_t count)
{
	struct tbv_cfgfs_link *link = tbv_cfgfs_to_link(item);
	struct tbv_id_addr addr;
	int ret;

	ret = tbv_cfgfs_parse_ipv4(buf, &addr);
	if (ret)
		return ret;

	mutex_lock(&link->lock);
	link->staged_route.local_src_addr = addr;
	link->local_src_set = true;
	ret = tbv_cfgfs_apply_route_locked(link);
	mutex_unlock(&link->lock);

	return ret ? ret : count;
}

static ssize_t peer_ipv4_show(struct config_item *item, char *buf)
{
	struct tbv_cfgfs_link *link = tbv_cfgfs_to_link(item);
	ssize_t ret;

	mutex_lock(&link->lock);
	ret = tbv_cfgfs_format_ipv4(buf, &link->staged_route.peer_addr);
	mutex_unlock(&link->lock);
	return ret;
}

static ssize_t peer_ipv4_store(struct config_item *item, const char *buf,
			       size_t count)
{
	struct tbv_cfgfs_link *link = tbv_cfgfs_to_link(item);
	struct tbv_id_addr addr;
	int ret;

	ret = tbv_cfgfs_parse_ipv4(buf, &addr);
	if (ret)
		return ret;

	mutex_lock(&link->lock);
	link->staged_route.peer_addr = addr;
	link->peer_set = true;
	ret = tbv_cfgfs_apply_route_locked(link);
	mutex_unlock(&link->lock);

	return ret ? ret : count;
}

static ssize_t nccl_addr_range_show(struct config_item *item, char *buf)
{
	struct tbv_cfgfs_link *link = tbv_cfgfs_to_link(item);
	ssize_t ret;

	mutex_lock(&link->lock);
	if (!link->cfg.nccl_policy_set ||
	    !link->cfg.nccl_policy.has_addr_range) {
		ret = sysfs_emit(buf, "<unset>\n");
	} else {
		const struct tbv_id_addr *addr = &link->cfg.nccl_policy.addr_range;

		ret = sysfs_emit(buf, "%u.%u.%u.%u/%u\n",
				 addr->bytes[12], addr->bytes[13],
				 addr->bytes[14], addr->bytes[15],
				 link->cfg.nccl_policy.addr_range_bits);
	}
	mutex_unlock(&link->lock);
	return ret;
}

static ssize_t nccl_addr_range_store(struct config_item *item, const char *buf,
				     size_t count)
{
	struct tbv_cfgfs_link *link = tbv_cfgfs_to_link(item);
	struct tbv_id_nccl_policy policy;
	char ip[32];
	unsigned int bits;
	int ret;

	if (sscanf(buf, "%31[^/]/%u", ip, &bits) != 2)
		return -EINVAL;
	if (bits > 32)
		return -EINVAL;

	tbv_id_nccl_policy_default(&policy);
	ret = tbv_cfgfs_parse_ipv4(ip, &policy.addr_range);
	if (ret)
		return ret;
	policy.has_addr_range = true;
	policy.addr_range_bits = bits;

	mutex_lock(&link->lock);
	ret = tbv_cfg_link_set_nccl_policy(&link->cfg, &policy);
	mutex_unlock(&link->lock);

	return ret ? ret : count;
}

static int tbv_cfgfs_parse_gid_type(const char *type)
{
	if (!strcmp(type, "roce-v2"))
		return TBV_ID_GID_ROCE_V2;
	if (!strcmp(type, "roce-v1"))
		return TBV_ID_GID_ROCE_V1;
	if (!strcmp(type, "ib"))
		return TBV_ID_GID_IB;
	return -EINVAL;
}

static ssize_t app_gids_show(struct config_item *item, char *buf)
{
	struct tbv_cfgfs_link *link = tbv_cfgfs_to_link(item);
	ssize_t off = 0;
	u32 i;

	mutex_lock(&link->lock);
	for (i = 0; i < link->cfg.app_gid_count; i++) {
		const struct tbv_id_gid *gid = &link->cfg.app_gids[i];

		off += sysfs_emit_at(buf, off,
				     "%u %u %u %s %u.%u.%u.%u\n",
				     gid->device_id, gid->port, gid->gid_index,
				     tbv_cfgfs_gid_type_name(gid->gid_type),
				     gid->addr.bytes[12], gid->addr.bytes[13],
				     gid->addr.bytes[14], gid->addr.bytes[15]);
	}
	mutex_unlock(&link->lock);

	return off;
}

static ssize_t app_gids_store(struct config_item *item, const char *buf,
			      size_t count)
{
	struct tbv_cfgfs_link *link = tbv_cfgfs_to_link(item);
	struct tbv_id_gid gids[TBV_CFG_MAX_APP_GIDS];
	struct tbv_id_gid gid;
	char type[16];
	char ip[32];
	unsigned int dev, port, index;
	int gid_type;
	int ret;

	if (sscanf(buf, "%u %u %u %15s %31s",
		   &dev, &port, &index, type, ip) != 5)
		return -EINVAL;
	if (port > 255 || index > 255)
		return -EINVAL;

	gid_type = tbv_cfgfs_parse_gid_type(type);
	if (gid_type < 0)
		return gid_type;

	memset(&gid, 0, sizeof(gid));
	gid.device_id = dev;
	gid.port = port;
	gid.gid_index = index;
	gid.gid_type = gid_type;
	ret = tbv_cfgfs_parse_ipv4(ip, &gid.addr);
	if (ret)
		return ret;

	mutex_lock(&link->lock);
	if (link->cfg.app_gid_count >= TBV_CFG_MAX_APP_GIDS) {
		ret = -ENOSPC;
		goto unlock;
	}

	memcpy(gids, link->cfg.app_gids,
	       sizeof(*gids) * link->cfg.app_gid_count);
	gids[link->cfg.app_gid_count] = gid;
	ret = tbv_cfg_link_set_app_gids(&link->cfg, gids,
					link->cfg.app_gid_count + 1);
unlock:
	mutex_unlock(&link->lock);
	return ret ? ret : count;
}

static ssize_t state_show(struct config_item *item, char *buf)
{
	struct tbv_cfgfs_link *link = tbv_cfgfs_to_link(item);
	ssize_t ret;

	mutex_lock(&link->lock);
	ret = sysfs_emit(buf, "%s\n", tbv_cfgfs_state_name(link->cfg.state));
	mutex_unlock(&link->lock);
	return ret;
}

static ssize_t selection_show(struct config_item *item, char *buf)
{
	struct tbv_cfgfs_link *link = tbv_cfgfs_to_link(item);
	const struct tbv_id_selection *sel;
	ssize_t ret;

	mutex_lock(&link->lock);
	sel = &link->cfg.app_selection;
	if (!sel->valid) {
		ret = sysfs_emit(buf, "<unset>\n");
	} else {
		ret = sysfs_emit(buf, "%u %u %u %s %u.%u.%u.%u\n",
				 sel->device_id, sel->port, sel->gid_index,
				 tbv_cfgfs_gid_type_name(sel->gid_type),
				 sel->addr.bytes[12], sel->addr.bytes[13],
				 sel->addr.bytes[14], sel->addr.bytes[15]);
	}
	mutex_unlock(&link->lock);
	return ret;
}

static ssize_t seal_store(struct config_item *item, const char *buf,
			  size_t count)
{
	struct tbv_cfgfs_link *link = tbv_cfgfs_to_link(item);
	const struct tbv_transport_ops *ops;
	int ret;

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;

	mutex_lock(&link->lock);
	ret = tbv_cfg_link_seal(&link->cfg);
	if (ret)
		goto unlock;

	ops = tbv_transport_get(link->cfg.backend == TBV_CFG_BACKEND_NATIVE ?
				TBV_BACKEND_NATIVE : TBV_BACKEND_APPLE);
	if (!ops || !ops->validate_config) {
		ret = -EINVAL;
		goto unlock;
	}
	ret = ops->validate_config(&link->cfg);
unlock:
	mutex_unlock(&link->lock);
	return ret ? ret : count;
}

static ssize_t activate_store(struct config_item *item, const char *buf,
			      size_t count)
{
	struct tbv_cfgfs_link *link = tbv_cfgfs_to_link(item);
	const struct tbv_transport_ops *ops;
	enum tbv_backend_type backend;
	int ret;

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;

	mutex_lock(&link->lock);
	backend = link->cfg.backend == TBV_CFG_BACKEND_NATIVE ?
		  TBV_BACKEND_NATIVE : TBV_BACKEND_APPLE;
	ops = tbv_transport_get(backend);
	if (!ops || !ops->activate) {
		ret = -EINVAL;
		goto unlock;
	}

	ret = tbv_cfg_link_activate(&link->cfg);
	if (ret)
		goto unlock;

	ret = ops->activate(link->state, &link->cfg);
unlock:
	mutex_unlock(&link->lock);
	return ret ? ret : count;
}

static struct configfs_attribute tbv_cfgfs_attr_backend = {
	.ca_name = "backend",
	.ca_mode = 0644,
	.show = backend_show,
	.store = backend_store,
};

static struct configfs_attribute tbv_cfgfs_attr_local_src_ipv4 = {
	.ca_name = "local_src_ipv4",
	.ca_mode = 0644,
	.show = local_src_ipv4_show,
	.store = local_src_ipv4_store,
};

static struct configfs_attribute tbv_cfgfs_attr_peer_ipv4 = {
	.ca_name = "peer_ipv4",
	.ca_mode = 0644,
	.show = peer_ipv4_show,
	.store = peer_ipv4_store,
};

static struct configfs_attribute tbv_cfgfs_attr_nccl_addr_range = {
	.ca_name = "nccl_addr_range",
	.ca_mode = 0644,
	.show = nccl_addr_range_show,
	.store = nccl_addr_range_store,
};

static struct configfs_attribute tbv_cfgfs_attr_app_gids = {
	.ca_name = "app_gids",
	.ca_mode = 0644,
	.show = app_gids_show,
	.store = app_gids_store,
};

static struct configfs_attribute tbv_cfgfs_attr_state = {
	.ca_name = "state",
	.ca_mode = 0444,
	.show = state_show,
};

static struct configfs_attribute tbv_cfgfs_attr_selection = {
	.ca_name = "selection",
	.ca_mode = 0444,
	.show = selection_show,
};

static struct configfs_attribute tbv_cfgfs_attr_seal = {
	.ca_name = "seal",
	.ca_mode = 0200,
	.store = seal_store,
};

static struct configfs_attribute tbv_cfgfs_attr_activate = {
	.ca_name = "activate",
	.ca_mode = 0200,
	.store = activate_store,
};

static struct configfs_attribute *tbv_cfgfs_link_attrs[] = {
	&tbv_cfgfs_attr_backend,
	&tbv_cfgfs_attr_local_src_ipv4,
	&tbv_cfgfs_attr_peer_ipv4,
	&tbv_cfgfs_attr_nccl_addr_range,
	&tbv_cfgfs_attr_app_gids,
	&tbv_cfgfs_attr_state,
	&tbv_cfgfs_attr_selection,
	&tbv_cfgfs_attr_seal,
	&tbv_cfgfs_attr_activate,
	NULL,
};

static void tbv_cfgfs_link_release(struct config_item *item)
{
	struct tbv_cfgfs_link *link = tbv_cfgfs_to_link(item);
	const struct tbv_transport_ops *ops;

	if (link->cfg.state == TBV_CFG_ACTIVE) {
		ops = tbv_transport_get(link->cfg.backend == TBV_CFG_BACKEND_NATIVE ?
					TBV_BACKEND_NATIVE : TBV_BACKEND_APPLE);
		if (ops && ops->deactivate)
			ops->deactivate(link->state, &link->cfg);
	}
	mutex_destroy(&link->lock);
	kfree(link);
}

static struct configfs_item_operations tbv_cfgfs_link_item_ops = {
	.release = tbv_cfgfs_link_release,
};

static const struct config_item_type tbv_cfgfs_link_type = {
	.ct_item_ops = &tbv_cfgfs_link_item_ops,
	.ct_attrs = tbv_cfgfs_link_attrs,
	.ct_owner = THIS_MODULE,
};

static struct config_item *tbv_cfgfs_make_link(struct config_group *group,
					       const char *name)
{
	struct tbv_cfgfs_link *link;
	int link_id;

	link = kzalloc(sizeof(*link), GFP_KERNEL);
	if (!link)
		return ERR_PTR(-ENOMEM);

	mutex_init(&link->lock);
	link->state = tbv_cfgfs_state;
	link_id = atomic_inc_return(&tbv_cfgfs_next_link_id);
	tbv_cfg_link_init(&link->cfg, link_id);
	config_item_init_type_name(&link->item, name, &tbv_cfgfs_link_type);
	return &link->item;
}

static struct configfs_group_operations tbv_cfgfs_group_ops = {
	.make_item = tbv_cfgfs_make_link,
};

static const struct config_item_type tbv_cfgfs_root_type = {
	.ct_group_ops = &tbv_cfgfs_group_ops,
	.ct_owner = THIS_MODULE,
};

int tbv_configfs_start(struct tbv_state *state)
{
	int ret;

	config_group_init_type_name(&tbv_cfgfs_subsys.su_group, TBV_DRV_NAME,
				    &tbv_cfgfs_root_type);
	tbv_cfgfs_state = state;
	mutex_init(&tbv_cfgfs_subsys.su_mutex);

	ret = configfs_register_subsystem(&tbv_cfgfs_subsys);
	if (ret)
		return ret;

	pr_info("registered /sys/kernel/config/%s\n", TBV_DRV_NAME);
	return 0;
}

void tbv_configfs_stop(struct tbv_state *state)
{
	configfs_unregister_subsystem(&tbv_cfgfs_subsys);
	tbv_cfgfs_state = NULL;
	pr_info("unregistered\n");
}
