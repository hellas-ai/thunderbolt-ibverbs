# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree build for the release-track thunderbolt-ibverbs module.

KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PWD := $(shell pwd)

obj-m := thunderbolt_ibverbs.o
thunderbolt_ibverbs-y := main.o profile.o backend.o configfs.o core.o debugfs.o ibdev.o link.o native.o apple.o trace.o native_control.o native_control_legacy.o native_control_xdomain.o path.o peer.o rail.o service.o tbnet_identity.o tbnet_minimal.o ../proto/config.o ../proto/identity.o ../proto/reliability.o

ccflags-y := -Wall -Wextra -Wno-unused-parameter -I$(src) -I$(src)/..

# COMPAT PROBE. The Asahi USB4 tree carries struct tb_ring.interval_nsec, which
# stock kernels do not. Probe for it rather than requiring a patched kernel, so
# one source builds against either.
#
# Two properties this probe must have, both of them paid for by a CI failure:
#
#  1. Look under BOTH $(KDIR)/include and $(KDIR)/source/include. A kernel
#     packaged as a SPLIT build/source tree keeps only generated headers under
#     build/ and the real ones under source/ -- which is what nixpkgs ships and
#     what the Hydra builders run. Those two are SIBLINGS
#     (/lib/modules/$(KVER)/{build,source}), though some packagings nest
#     source/ inside build/, so both spellings are tried.
#
#  2. FAIL CLOSED. The previous form was
#         ifneq ($(shell grep -c interval_nsec $(TBV_TB_HDR) 2>/dev/null),0)
#     and grep on a missing file prints NOTHING, so $(shell ...) expanded to the
#     empty string, "" != "0" was true, and the flag was defined precisely when
#     the probe could not check anything. Against linux 6.18.37 that produced
#     "error: 'struct tb_ring' has no member named 'interval_nsec'" -- a probe
#     whose failure mode was to assert the thing it exists to doubt.
#     grep -l prints a filename on a match and nothing otherwise, so a missing
#     or unreadable header can only ever yield "absent".
#
# The pattern matches a member DECLARATION (identifier followed by ; , or :)
# rather than the bare string, so prose in a comment cannot enable the code.
TBV_TB_HDR := $(firstword $(wildcard $(KDIR)/include/linux/thunderbolt.h \
                                     $(KDIR)/source/include/linux/thunderbolt.h \
                                     $(KDIR)/../source/include/linux/thunderbolt.h))
tbv-kernel-has = $(if $(TBV_TB_HDR),$(shell grep -lE '$(1)[[:space:]]*[;,:]' $(TBV_TB_HDR) 2>/dev/null))

ifneq ($(call tbv-kernel-has,interval_nsec),)
ccflags-y += -DTBV_HAVE_RING_INTERVAL_NSEC
endif

.PHONY: all clean modules modules_install help compat-report

# Report what the COMPAT PROBE above concluded, without building anything.
# tools/ci/kernel-compat-probe-test.sh drives this against synthetic kernel
# trees; it is also the fastest way for a human to answer "why did this build
# think my kernel had that member?".
compat-report:
	@echo "TBV_TB_HDR=$(TBV_TB_HDR)"
	@echo "interval_nsec=$(if $(findstring TBV_HAVE_RING_INTERVAL_NSEC,$(ccflags-y)),yes,no)"


all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(RM) ../proto/*.o ../proto/.*.o.cmd

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
	@echo "  tbnet_identity_minimal_e2e=0|1"
	@echo "  tbnet_identity_minimal_apple_only=0|1"
	@echo "  bind_services=0|1"
	@echo "  allocate_rings=0|1"
	@echo "  start_rings=0|1"
	@echo "  negotiate_native=0|1"
	@echo "  enable_tunnels=0|1"
	@echo "  register_verbs=0|1"
	@echo "  native_prtcstns=<u32>"
	@echo "  apple_prtcstns=<u32>"
	@echo "  apple_tx_max_inflight_wr=<N>    deprecated compatibility knob; Apple SENDs serialize per QP"
	@echo "  apple_tx_max_inflight_frames=<N>"
	@echo "  apple_rx_pending_bytes=<bytes>"
	@echo "  apple_rx_pending_slots=<N>"
	@echo "  apple_rx_pending_total_bytes=<bytes>"
	@echo "  apple_rx_trace=<N>"
