// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/thunderbolt.h>

#include "tbv.h"

#ifdef TB_PROTOCOL_HANDLER_HAS_XDOMAIN
static struct tb_protocol_handler tbv_native_xdomain_handler;
static bool tbv_native_xdomain_handler_registered;

static int tbv_native_control_xdomain_handle(struct tb_xdomain *source_xd,
					     const void *buf, size_t size,
					     void *data)
{
	if (!source_xd || !data)
		return 0;

	return tbv_native_control_handle_packet(data, source_xd, buf, size);
}

int tbv_native_control_xdomain_start(struct tbv_state *state)
{
	int ret;

	memset(&tbv_native_xdomain_handler, 0,
	       sizeof(tbv_native_xdomain_handler));
	tbv_native_xdomain_handler.uuid = &tbv_native_service_uuid;
	tbv_native_xdomain_handler.callback_xd =
		tbv_native_control_xdomain_handle;
#ifdef TB_PROTOCOL_HANDLER_HAS_OWNER
	tbv_native_xdomain_handler.owner = THIS_MODULE;
#endif
	tbv_native_xdomain_handler.data = state;

	ret = tb_register_protocol_handler(&tbv_native_xdomain_handler);
	if (ret)
		return ret;

	tbv_native_xdomain_handler_registered = true;
	pr_info("native control using source-aware XDomain handler\n");
	return 0;
}

void tbv_native_control_xdomain_stop(void)
{
	if (!tbv_native_xdomain_handler_registered)
		return;

	tb_unregister_protocol_handler(&tbv_native_xdomain_handler);
	tbv_native_xdomain_handler_registered = false;
}
#else
int tbv_native_control_xdomain_start(struct tbv_state *state)
{
	return -EOPNOTSUPP;
}

void tbv_native_control_xdomain_stop(void)
{
}
#endif
