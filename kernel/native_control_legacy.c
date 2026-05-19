// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "thunderbolt_ibverbs: " fmt

#include <linux/module.h>
#include <linux/string.h>
#include <linux/thunderbolt.h>

#include "tbv.h"

static struct tb_protocol_handler tbv_native_legacy_handler;
static bool tbv_native_legacy_handler_registered;

static int tbv_native_control_legacy_handle(const void *buf, size_t size,
					    void *data)
{
	if (!data)
		return 0;

	return tbv_native_control_handle_packet(data, NULL, buf, size);
}

int tbv_native_control_legacy_start(struct tbv_state *state)
{
	int ret;

	memset(&tbv_native_legacy_handler, 0,
	       sizeof(tbv_native_legacy_handler));
	tbv_native_legacy_handler.uuid = &tbv_native_service_uuid;
	tbv_native_legacy_handler.callback = tbv_native_control_legacy_handle;
#ifdef TB_PROTOCOL_HANDLER_HAS_OWNER
	tbv_native_legacy_handler.owner = THIS_MODULE;
#endif
	tbv_native_legacy_handler.data = state;

	ret = tb_register_protocol_handler(&tbv_native_legacy_handler);
	if (ret)
		return ret;

	tbv_native_legacy_handler_registered = true;
	pr_info("native control using legacy source-blind XDomain handler\n");
	return 0;
}

void tbv_native_control_legacy_stop(void)
{
	if (!tbv_native_legacy_handler_registered)
		return;

	tb_unregister_protocol_handler(&tbv_native_legacy_handler);
	tbv_native_legacy_handler_registered = false;
}
