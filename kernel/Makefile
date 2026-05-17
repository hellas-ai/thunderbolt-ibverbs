# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree build for the release-track thunderbolt-ibverbs module.

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PWD := $(shell pwd)

obj-m := thunderbolt_ibverbs.o
thunderbolt_ibverbs-y := main.o profile.o backend.o core.o debugfs.o ibdev.o native_control.o native_control_legacy.o native_control_xdomain.o path.o peer.o rail.o service.o tbnet_identity.o

ccflags-y := -Wall -Wextra -Wno-unused-parameter

.PHONY: all clean modules modules_install help

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

help:
	@echo "Targets:"
	@echo "  modules            Build thunderbolt_ibverbs.ko"
	@echo "  modules_install    Install under /lib/modules/\$$(uname -r)/extra/"
	@echo "  clean              Remove build artifacts"
	@echo ""
	@echo "Parameters:"
	@echo "  compat=auto|force|off"
	@echo "  profile=auto|mac_compat|linux_perf|mixed"
	@echo "  tbnet=auto|allow|prefer_rdma|block"
	@echo "  tbnet_identity=auto|stock|stock_proxy|minimal_packet|off"
	@echo "  tbnet_identity_tbnet=<netdev>"
	@echo "  tbnet_identity_gid=auto|<netdev>"
	@echo "  bind_services=0|1"
	@echo "  allocate_rings=0|1"
	@echo "  start_rings=0|1"
	@echo "  negotiate_native=0|1"
	@echo "  enable_tunnels=0|1"
	@echo "  register_verbs=0|1"
	@echo "  native_prtcstns=<u32>"
	@echo "  apple_prtcstns=<u32>"
	@echo "  apple_tx_max_inflight_wr=<N>"
	@echo "  apple_tx_max_inflight_frames=<N>"
	@echo "  apple_tx_pace_us=<N>"
	@echo "  apple_tx_pace_interval_frames=<N>"
	@echo "  apple_tx_pace_after_frames=<N>"
	@echo "  apple_rx_pending_bytes=<bytes>"
	@echo "  apple_rx_pending_slots=<N>"
	@echo "  apple_rx_pending_total_bytes=<bytes>"
