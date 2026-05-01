# Standalone build for the tbnetmq skeleton.
#
# This is intentionally separate from the main usb4_rdma module so the
# two register independently with the kernel's tb_service bus and can
# be loaded / unloaded independently while we develop tbnetmq.

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

obj-m := tbnetmq.o

ccflags-y := -Wall -Wextra -Wno-unused-parameter

.PHONY: all clean
all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
