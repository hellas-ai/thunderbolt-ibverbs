# Out-of-tree build for the usb4_rdma kernel module skeleton.
#
# By default, builds against the running kernel's headers. Override by
# setting KDIR=/path/to/linux at make time.

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

obj-m := usb4_rdma.o
usb4_rdma-y := main.o bar.o loadtest.o

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
