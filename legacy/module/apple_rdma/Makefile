# Out-of-tree build for the experimental Apple RDMA-over-Thunderbolt
# replacement module.

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
PWD := $(shell pwd)

obj-m := apple_rdma.o

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
	@echo "  modules            Build apple_rdma.ko"
	@echo "  clean              Remove build artefacts"
	@echo ""
	@echo "Useful load:"
	@echo "  sudo insmod apple_rdma.ko service_uuid=<mac-ad-fa57-uuid>"
