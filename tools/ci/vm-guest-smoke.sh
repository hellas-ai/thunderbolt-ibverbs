#!/usr/bin/env bash
# Runs inside a distro VM. Installs the guest's own kernel headers, builds the
# module against the running kernel, loads it without real Thunderbolt devices,
# and verifies kernel registration plus userspace RDMA visibility.

set -euo pipefail

distro="${1:-}"
src_dir="${TBV_GUEST_SRC:-/work/src}"

usage() {
	cat <<'EOF'
Usage:
  tools/ci/vm-guest-smoke.sh debian|fedora|arch
EOF
}

die() {
	printf 'error: %s\n' "$*" >&2
	exit 1
}

if [[ "$distro" == "-h" || "$distro" == "--help" ]]; then
	usage
	exit 0
fi
[[ -n "$distro" ]] || die "missing distro"
[[ -d "$src_dir" ]] || die "source directory not found: $src_dir"

install_deps() {
	local kver="$1"

	case "$distro" in
	debian)
		export DEBIAN_FRONTEND=noninteractive
		apt-get update -qq
		apt-get install -y -qq --no-install-recommends \
			build-essential ca-certificates gcc git ibverbs-utils \
			iproute2 kmod libibverbs-dev linux-headers-"$kver" \
			make rdma-core
		;;
	fedora)
		dnf install -y -q --setopt=install_weak_deps=False \
			ca-certificates gcc git iproute kernel-headers kmod \
			libibverbs-utils make openssl rdma-core rdma-core-devel \
			"kernel-devel-uname-r == $kver"
		;;
	arch)
		pacman -Syu --noconfirm --needed \
			base-devel ca-certificates gcc git iproute2 kmod \
			linux-headers make rdma-core
		;;
	*)
		die "unsupported distro: $distro"
		;;
	esac
}

maybe_reboot_for_arch_headers() {
	local kver="$1"

	if [[ "$distro" != "arch" || -d "/lib/modules/$kver/build" ]]; then
		return 0
	fi

	if [[ "${TBV_GUEST_REBOOTED:-0}" == "1" ]]; then
		die "headers for running Arch kernel are still missing after reboot: $kver"
	fi

	printf 'Arch kernel headers do not match the running kernel; rebooting once\n'
	(sleep 1; systemctl reboot) >/dev/null 2>&1 &
	exit 75
}

build_module() {
	local kver="$1"

	[[ -d "/lib/modules/$kver/build" ]] ||
		die "kernel headers missing: /lib/modules/$kver/build"

	make -C "$src_dir" KVER="$kver" KDIR="/lib/modules/$kver/build" modules
	install -D -m 0644 "$src_dir/kernel/thunderbolt_ibverbs.ko" \
		"/lib/modules/$kver/extra/thunderbolt_ibverbs.ko"
	depmod -a "$kver"
}

load_and_check() {
	local kver="$1"

	modprobe thunderbolt_ibverbs profile=linux_perf bind_services=0 \
		register_verbs=1

	if ! grep -q '^thunderbolt_ibverbs ' /proc/modules; then
		dmesg | tail -n 80 >&2 || true
		die "thunderbolt_ibverbs is not loaded"
	fi

	modinfo thunderbolt_ibverbs | sed -n '1,20p'
	ls -l /sys/class/infiniband
	test -d /sys/class/infiniband/usb4_rdma0

	udevadm settle || true
	ls -l /dev/infiniband /sys/class/infiniband_verbs
	test -e /dev/infiniband/uverbs0
	rdma link show | tee /tmp/tbv-rdma-link.txt
	grep -q usb4_rdma /tmp/tbv-rdma-link.txt

	ibv_devices | tee /tmp/tbv-ibv-devices.txt
	if grep -q usb4_rdma /tmp/tbv-ibv-devices.txt; then
		gcc -std=c11 -Wall -Wextra -Werror \
			"$src_dir/tools/ci/verbs-smoke.c" -libverbs \
			-o /tmp/tbv-verbs-smoke
		/tmp/tbv-verbs-smoke
	else
		printf 'libibverbs did not enumerate usb4_rdma without a userspace provider; checked rdma(8) and uverbs char device instead\n'
	fi

	rmmod thunderbolt_ibverbs
	if grep -q '^thunderbolt_ibverbs ' /proc/modules; then
		die "thunderbolt_ibverbs did not unload"
	fi

	make -C "$src_dir" KVER="$kver" KDIR="/lib/modules/$kver/build" clean
}

if [[ "$(id -u)" -ne 0 ]]; then
	exec sudo env TBV_GUEST_REBOOTED="${TBV_GUEST_REBOOTED:-0}" \
		TBV_GUEST_SRC="$src_dir" bash "$0" "$distro"
fi

printf '==> Guest distro\n'
grep -E '^(PRETTY_NAME|NAME|VERSION)=' /etc/os-release || true
printf '==> Kernel: %s\n' "$(uname -r)"

kver="$(uname -r)"
printf '==> Installing guest dependencies\n'
install_deps "$kver"
maybe_reboot_for_arch_headers "$kver"

printf '==> Building module for %s\n' "$kver"
build_module "$kver"

printf '==> Loading module and checking userspace RDMA visibility\n'
load_and_check "$kver"

printf '==> VM smoke OK\n'
