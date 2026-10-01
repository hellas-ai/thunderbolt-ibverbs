// SPDX-License-Identifier: GPL-2.0
/* Compile-only Kbuild feature probe; never linked into the module. */
#include <linux/thunderbolt.h>

_Static_assert(__builtin_types_compatible_p(
	typeof(((struct tb_service_driver *)0)->probe),
	int (*)(struct tb_service *)), "tb_service probe still takes an ID");
