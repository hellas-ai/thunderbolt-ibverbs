// SPDX-License-Identifier: GPL-2.0
/*
 * Public configuration policy for the release-track usb4_rdma module.
 *
 * The implementation today is the Linux/Linux native backend. The
 * Apple-compatible backend remains in module/apple_rdma while the two-lane
 * macOS questions are still being characterized. Keep the public knobs here
 * stable so the eventual unified backend can slot in without changing user
 * scripts.
 */

#define pr_fmt(fmt) "usb4_rdma/config: " fmt

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/string.h>

#include "config.h"

enum usb4_rdma_compat_setting {
	USB4_RDMA_COMPAT_AUTO = 0,
	USB4_RDMA_COMPAT_FORCE,
	USB4_RDMA_COMPAT_OFF,
};

enum usb4_rdma_profile_setting {
	USB4_RDMA_PROFILE_AUTO = 0,
	USB4_RDMA_PROFILE_SETTING_MAC_COMPAT,
	USB4_RDMA_PROFILE_SETTING_LINUX_PERF,
};

enum usb4_rdma_tbnet_policy {
	USB4_RDMA_TBNET_AUTO = 0,
	USB4_RDMA_TBNET_ALLOW,
	USB4_RDMA_TBNET_PREFER_RDMA,
	USB4_RDMA_TBNET_BLOCK,
};

static char *compat = "auto";
module_param(compat, charp, 0444);
MODULE_PARM_DESC(compat,
		 "peer compatibility mode: auto, force (Apple), off (native Linux only)");

static char *profile = "auto";
module_param(profile, charp, 0444);
MODULE_PARM_DESC(profile,
		 "deployment profile: auto, mac_compat, linux_perf");

static char *lanes = "auto";
module_param(lanes, charp, 0444);
MODULE_PARM_DESC(lanes,
		 "maximum RDMA lanes to negotiate: auto, N, or min-max");

static char *tbnet = "auto";
module_param(tbnet, charp, 0444);
MODULE_PARM_DESC(tbnet,
		 "Thunderbolt-net coexistence policy: auto, allow, prefer_rdma, block");

static enum usb4_rdma_wire_mode resolved_wire_mode = USB4_RDMA_WIRE_NATIVE;
static enum usb4_rdma_profile resolved_profile = USB4_RDMA_PROFILE_LINUX_PERF;
static enum usb4_rdma_tbnet_policy resolved_tbnet = USB4_RDMA_TBNET_AUTO;
static bool lanes_auto = true;
static unsigned int lanes_min = 1;
static unsigned int lanes_max;

static bool streq(const char *a, const char *b)
{
	return a && !strcmp(a, b);
}

static const char *compat_name(enum usb4_rdma_compat_setting value)
{
	switch (value) {
	case USB4_RDMA_COMPAT_AUTO:
		return "auto";
	case USB4_RDMA_COMPAT_FORCE:
		return "force";
	case USB4_RDMA_COMPAT_OFF:
		return "off";
	}
	return "unknown";
}

static int parse_compat(enum usb4_rdma_compat_setting *out)
{
	if (streq(compat, "auto")) {
		*out = USB4_RDMA_COMPAT_AUTO;
		return 0;
	}
	if (streq(compat, "force")) {
		*out = USB4_RDMA_COMPAT_FORCE;
		return 0;
	}
	if (streq(compat, "off")) {
		*out = USB4_RDMA_COMPAT_OFF;
		return 0;
	}

	pr_err("invalid compat=%s; expected auto, force, or off\n",
	       compat ?: "(null)");
	return -EINVAL;
}

static int parse_profile(enum usb4_rdma_profile_setting *out)
{
	if (streq(profile, "auto")) {
		*out = USB4_RDMA_PROFILE_AUTO;
		return 0;
	}
	if (streq(profile, "mac_compat")) {
		*out = USB4_RDMA_PROFILE_SETTING_MAC_COMPAT;
		return 0;
	}
	if (streq(profile, "linux_perf")) {
		*out = USB4_RDMA_PROFILE_SETTING_LINUX_PERF;
		return 0;
	}

	pr_err("invalid profile=%s; expected auto, mac_compat, or linux_perf\n",
	       profile ?: "(null)");
	return -EINVAL;
}

static int parse_tbnet(void)
{
	if (streq(tbnet, "auto")) {
		resolved_tbnet = USB4_RDMA_TBNET_AUTO;
		return 0;
	}
	if (streq(tbnet, "allow")) {
		resolved_tbnet = USB4_RDMA_TBNET_ALLOW;
		return 0;
	}
	if (streq(tbnet, "prefer_rdma")) {
		resolved_tbnet = USB4_RDMA_TBNET_PREFER_RDMA;
		return 0;
	}
	if (streq(tbnet, "block")) {
		resolved_tbnet = USB4_RDMA_TBNET_BLOCK;
		return 0;
	}

	pr_err("invalid tbnet=%s; expected auto, allow, prefer_rdma, or block\n",
	       tbnet ?: "(null)");
	return -EINVAL;
}

static int parse_lane_uint(const char *text, size_t len, unsigned int *out)
{
	char tmp[16];

	if (!len || len >= sizeof(tmp))
		return -EINVAL;
	memcpy(tmp, text, len);
	tmp[len] = '\0';
	return kstrtouint(tmp, 0, out);
}

static int parse_lanes(void)
{
	const char *dash;
	int ret;

	if (streq(lanes, "auto")) {
		lanes_auto = true;
		lanes_min = 1;
		lanes_max = 0;
		return 0;
	}

	lanes_auto = false;
	dash = strchr(lanes, '-');
	if (dash) {
		ret = parse_lane_uint(lanes, dash - lanes, &lanes_min);
		if (ret)
			goto invalid;
		ret = kstrtouint(dash + 1, 0, &lanes_max);
		if (ret)
			goto invalid;
	} else {
		ret = kstrtouint(lanes, 0, &lanes_max);
		if (ret)
			goto invalid;
		lanes_min = lanes_max;
	}

	if (!lanes_min || !lanes_max || lanes_min > lanes_max)
		goto invalid;
	return 0;

invalid:
	pr_err("invalid lanes=%s; expected auto, N, or min-max\n",
	       lanes ?: "(null)");
	return -EINVAL;
}

int usb4_rdma_config_init(void)
{
	enum usb4_rdma_compat_setting compat_setting;
	enum usb4_rdma_profile_setting profile_setting;
	int ret;

	ret = parse_compat(&compat_setting);
	if (ret)
		return ret;
	ret = parse_profile(&profile_setting);
	if (ret)
		return ret;
	ret = parse_lanes();
	if (ret)
		return ret;
	ret = parse_tbnet();
	if (ret)
		return ret;

	switch (compat_setting) {
	case USB4_RDMA_COMPAT_AUTO:
	case USB4_RDMA_COMPAT_OFF:
		resolved_wire_mode = USB4_RDMA_WIRE_NATIVE;
		break;
	case USB4_RDMA_COMPAT_FORCE:
		resolved_wire_mode = USB4_RDMA_WIRE_APPLE;
		break;
	}

	switch (profile_setting) {
	case USB4_RDMA_PROFILE_AUTO:
		resolved_profile = resolved_wire_mode == USB4_RDMA_WIRE_APPLE ?
			USB4_RDMA_PROFILE_MAC_COMPAT :
			USB4_RDMA_PROFILE_LINUX_PERF;
		break;
	case USB4_RDMA_PROFILE_SETTING_MAC_COMPAT:
		resolved_profile = USB4_RDMA_PROFILE_MAC_COMPAT;
		break;
	case USB4_RDMA_PROFILE_SETTING_LINUX_PERF:
		resolved_profile = USB4_RDMA_PROFILE_LINUX_PERF;
		break;
	}

	if (resolved_wire_mode == USB4_RDMA_WIRE_APPLE ||
	    resolved_profile == USB4_RDMA_PROFILE_MAC_COMPAT) {
		pr_err("Apple-compatible backend is not linked into this release-track module yet; use module/apple_rdma for research interop\n");
		return -EOPNOTSUPP;
	}

	pr_info("compat=%s profile=%s lanes=%s tbnet=%s -> wire=%s profile=%s\n",
		compat_name(compat_setting), profile, lanes, tbnet,
		usb4_rdma_config_wire_mode_name(),
		usb4_rdma_config_profile_name());
	return 0;
}

enum usb4_rdma_wire_mode usb4_rdma_config_wire_mode(void)
{
	return resolved_wire_mode;
}

enum usb4_rdma_profile usb4_rdma_config_profile(void)
{
	return resolved_profile;
}

const char *usb4_rdma_config_wire_mode_name(void)
{
	switch (resolved_wire_mode) {
	case USB4_RDMA_WIRE_NATIVE:
		return "native-linux";
	case USB4_RDMA_WIRE_APPLE:
		return "apple";
	}
	return "unknown";
}

const char *usb4_rdma_config_profile_name(void)
{
	switch (resolved_profile) {
	case USB4_RDMA_PROFILE_LINUX_PERF:
		return "linux_perf";
	case USB4_RDMA_PROFILE_MAC_COMPAT:
		return "mac_compat";
	}
	return "unknown";
}

const char *usb4_rdma_config_tbnet_policy_name(void)
{
	switch (resolved_tbnet) {
	case USB4_RDMA_TBNET_AUTO:
		return "auto";
	case USB4_RDMA_TBNET_ALLOW:
		return "allow";
	case USB4_RDMA_TBNET_PREFER_RDMA:
		return "prefer_rdma";
	case USB4_RDMA_TBNET_BLOCK:
		return "block";
	}
	return "unknown";
}

unsigned int usb4_rdma_config_max_lanes(unsigned int hard_max)
{
	unsigned int target;

	if (!hard_max)
		return 0;

	if (lanes_auto)
		target = resolved_profile == USB4_RDMA_PROFILE_MAC_COMPAT ?
			1 : hard_max;
	else
		target = lanes_max;

	return clamp_t(unsigned int, target, 1, hard_max);
}

bool usb4_rdma_config_research_build(void)
{
#ifdef USB4_RDMA_RESEARCH
	return true;
#else
	return false;
#endif
}

void usb4_rdma_config_show(struct seq_file *m)
{
	seq_printf(m, "wire_mode:       %s\n",
		   usb4_rdma_config_wire_mode_name());
	seq_printf(m, "profile:         %s\n",
		   usb4_rdma_config_profile_name());
	seq_printf(m, "compat_param:    %s\n", compat ?: "(null)");
	seq_printf(m, "profile_param:   %s\n", profile ?: "(null)");
	seq_printf(m, "lanes_param:     %s\n", lanes ?: "(null)");
	seq_printf(m, "tbnet_policy:    %s\n",
		   usb4_rdma_config_tbnet_policy_name());
	seq_printf(m, "research_build:  %u\n",
		   usb4_rdma_config_research_build());
}
