/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _USB4_RDMA_CONFIG_H
#define _USB4_RDMA_CONFIG_H

#include <linux/types.h>

struct seq_file;

enum usb4_rdma_wire_mode {
	USB4_RDMA_WIRE_NATIVE = 0,
	USB4_RDMA_WIRE_APPLE,
};

enum usb4_rdma_profile {
	USB4_RDMA_PROFILE_LINUX_PERF = 0,
	USB4_RDMA_PROFILE_MAC_COMPAT,
};

int usb4_rdma_config_init(void);
void usb4_rdma_config_show(struct seq_file *m);

enum usb4_rdma_wire_mode usb4_rdma_config_wire_mode(void);
enum usb4_rdma_profile usb4_rdma_config_profile(void);
const char *usb4_rdma_config_wire_mode_name(void);
const char *usb4_rdma_config_profile_name(void);
const char *usb4_rdma_config_tbnet_policy_name(void);

unsigned int usb4_rdma_config_max_lanes(unsigned int hard_max);
bool usb4_rdma_config_research_build(void);

#endif /* _USB4_RDMA_CONFIG_H */
