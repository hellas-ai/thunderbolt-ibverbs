# Out-of-tree build for the experimental ThunderboltIP RX-no-E2E variant.
#
# This module keeps the stock "network" service and advertises E2E support,
# but can allocate the receive ring without RING_FLAG_E2E. It is a diagnostic
# module for AMD NHI credit-return characterization and must not be loaded at
# the same time as stock thunderbolt_net.

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PWD  := $(shell pwd)

obj-m := thunderbolt_net_rxnoe2e.o
thunderbolt_net_rxnoe2e-objs := main.o trace.o

CFLAGS_trace.o := -I$(src)
ccflags-y := -Wall -Wextra -Wno-unused-parameter

.PHONY: all clean modules help

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

help:
	@echo "Targets:"
	@echo "  modules            Build thunderbolt_net_rxnoe2e.ko"
	@echo "  clean              Remove build artefacts"
	@echo ""
	@echo "Load after unloading stock thunderbolt_net:"
	@echo "  sudo insmod thunderbolt_net_rxnoe2e.ko rx_e2e=0"
