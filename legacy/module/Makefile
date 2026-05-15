# Out-of-tree build for the release-track usb4_rdma kernel module.
#
# By default, builds against the running kernel's headers. Override by
# setting KDIR=/path/to/linux at make time.
#
# Set RESEARCH=1 to compile the BAR0 explorer and loadtest services used
# during hardware characterization. The default build keeps those out of the
# public module while retaining the production native Linux data path.

KVER ?= $(shell uname -r)
KDIR ?= $(shell \
	if [ -e /lib/modules/$(KVER)/build ]; then \
		printf '%s\n' /lib/modules/$(KVER)/build; \
	else \
		d=$$(find /nix/store -maxdepth 1 -type d \
			-name '*-linux-$(KVER)-dev' -print -quit 2>/dev/null); \
		if [ -n "$$d" ]; then \
			printf '%s\n' "$$d/lib/modules/$(KVER)/build"; \
		else \
			printf '%s\n' /lib/modules/$(KVER)/build; \
		fi; \
	fi)
PWD  := $(shell pwd)

obj-m := usb4_rdma.o
usb4_rdma-y := main.o config.o ibdev.o data.o nhi_raw.o
ccflags-y := -Wall -Wextra -Wno-unused-parameter

ifeq ($(RESEARCH),1)
usb4_rdma-y += bar.o loadtest.o
ccflags-y += -DUSB4_RDMA_RESEARCH=1
endif

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
	@echo "  modules            Build usb4_rdma.ko"
	@echo "  modules_install    Install under /lib/modules/\$$(uname -r)/extra/"
	@echo "  clean              Remove build artefacts"
	@echo ""
	@echo "Variables:"
	@echo "  KDIR=<dir>         Kernel build directory (default: running kernel)"
	@echo ""
	@echo "Quick test (after build, on both peers):"
	@echo "  sudo insmod usb4_rdma.ko"
	@echo "  dmesg | tail"
	@echo "  cat /sys/kernel/debug/usb4_rdma/*/state"
	@echo "  sudo rmmod usb4_rdma"
