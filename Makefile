# SPDX-License-Identifier: GPL-2.0
#
# Top-level wrapper for out-of-tree builds and DKMS.

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
KERNEL_DIR ?= kernel

.PHONY: all clean modules modules_install help dkms-add dkms-build dkms-install dkms-remove

all: modules

modules:
	$(MAKE) -C $(KERNEL_DIR) KVER=$(KVER) KDIR=$(KDIR) modules

modules_install:
	$(MAKE) -C $(KERNEL_DIR) KVER=$(KVER) KDIR=$(KDIR) modules_install

clean:
	$(MAKE) -C $(KERNEL_DIR) KVER=$(KVER) KDIR=$(KDIR) clean

help:
	$(MAKE) -C $(KERNEL_DIR) help
	@echo ""
	@echo "Top-level targets:"
	@echo "  dkms-add          Register this source tree with DKMS"
	@echo "  dkms-build        Build with DKMS for KVER=$$(uname -r)"
	@echo "  dkms-install      Install with DKMS for KVER=$$(uname -r)"
	@echo "  dkms-remove       Remove this DKMS module version"

DKMS_VERSION := $(shell sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' dkms.conf)

dkms-add:
	dkms add .

dkms-build:
	dkms build thunderbolt-ibverbs/$(DKMS_VERSION) -k $(KVER)

dkms-install:
	dkms install thunderbolt-ibverbs/$(DKMS_VERSION) -k $(KVER)

dkms-remove:
	dkms remove thunderbolt-ibverbs/$(DKMS_VERSION) --all
