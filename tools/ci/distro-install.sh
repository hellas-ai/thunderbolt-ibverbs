#!/usr/bin/env bash
# Install a thunderbolt-ibverbs-dkms or usb4-rdma-provider package built by the
# matching distro-package*.sh script. Verifies:
#
#   thunderbolt-ibverbs-dkms   — DKMS can build the module against the distro's
#                                packaged kernel-headers.
#   usb4-rdma-provider         — provider .so is installed at the expected path,
#                                its dynamic deps resolve, and libibverbs does
#                                not crash when its driver hint is present.
#
# Does NOT load the kernel module — that needs a real distro kernel and lives
# in tools/ci/vm-smoke.sh.

set -euo pipefail

usage() {
	cat <<'EOF'
Usage:
  tools/ci/distro-install.sh <artefact-path-or-glob>

Detects the package type from the artefact filename
(thunderbolt-ibverbs-dkms or usb4-rdma-provider) and runs the appropriate
verification flow.
EOF
}

target="${1:-}"
case "${target:-}" in
	-h|--help) usage; exit 0 ;;
	"") usage >&2; exit 1 ;;
esac

shopt -s nullglob
# shellcheck disable=SC2206
artefacts=( $target )
shopt -u nullglob
if [[ ${#artefacts[@]} -eq 0 ]]; then
	if [[ -f "$target" ]]; then
		artefacts=( "$target" )
	else
		printf 'error: no artefact matched: %s\n' "$target" >&2
		exit 1
	fi
fi
if [[ ${#artefacts[@]} -gt 1 ]]; then
	printf 'error: multiple artefacts matched: %s\n' "${artefacts[*]}" >&2
	exit 1
fi
artefact="$(realpath "${artefacts[0]}")"
[[ -f "$artefact" ]] || { printf 'error: not a file: %s\n' "$artefact" >&2; exit 1; }

case "$(basename "$artefact")" in
	thunderbolt-ibverbs-dkms*) pkg_kind="dkms" ;;
	usb4-rdma-provider*)       pkg_kind="rdma-provider" ;;
	*)
		printf 'error: unknown package name in %s\n' "$artefact" >&2
		exit 1
		;;
esac

install_dkms_deps() {
	if command -v apt-get >/dev/null 2>&1; then
		export DEBIAN_FRONTEND=noninteractive
		apt-get update -qq
		apt-get install -y -qq --no-install-recommends \
			build-essential ca-certificates dkms file kmod \
			linux-headers-amd64 make
	elif command -v dnf >/dev/null 2>&1; then
		dnf install -y -q --setopt=install_weak_deps=False \
			ca-certificates dkms diffutils file gcc kernel-devel \
			kernel-headers kmod make openssl
	elif command -v pacman >/dev/null 2>&1; then
		pacman -Syu --noconfirm --needed \
			base-devel ca-certificates dkms file kmod linux-headers make
	else
		printf 'error: unsupported distro\n' >&2
		cat /etc/os-release >&2 || true
		exit 1
	fi
}

install_provider_deps() {
	if command -v apt-get >/dev/null 2>&1; then
		export DEBIAN_FRONTEND=noninteractive
		apt-get update -qq
		apt-get install -y -qq --no-install-recommends \
			ca-certificates file ibverbs-providers ibverbs-utils libibverbs1
	elif command -v dnf >/dev/null 2>&1; then
		dnf install -y -q --setopt=install_weak_deps=False \
			ca-certificates file libibverbs libibverbs-utils
	elif command -v pacman >/dev/null 2>&1; then
		pacman -Syu --noconfirm --needed \
			ca-certificates file rdma-core
	else
		printf 'error: unsupported distro\n' >&2
		cat /etc/os-release >&2 || true
		exit 1
	fi
}

install_package() {
	case "$artefact" in
	*.deb)
		DEBIAN_FRONTEND=noninteractive apt-get install -y -qq "$artefact"
		;;
	*.rpm)
		dnf install -y "$artefact"
		;;
	*.pkg.tar.zst|*.pkg.tar.xz)
		pacman -U --noconfirm "$artefact"
		;;
	*)
		printf 'error: unsupported artefact extension: %s\n' "$artefact" >&2
		exit 1
		;;
	esac
}

find_dkms_kver() {
	local kver=""
	if [[ -d /usr/src/kernels ]]; then
		kver="$(find /usr/src/kernels -mindepth 1 -maxdepth 1 -type d \
			-printf '%f\n' | sort -V | tail -n 1)"
		if [[ -n "$kver" && ! -e "/lib/modules/$kver/build" ]]; then
			mkdir -p "/lib/modules/$kver"
			ln -s "/usr/src/kernels/$kver" "/lib/modules/$kver/build"
		fi
	fi
	if [[ -z "$kver" && -d /lib/modules ]]; then
		kver="$(find /lib/modules -mindepth 1 -maxdepth 1 -type d \
			-printf '%f\n' | sort -V | tail -n 1)"
	fi
	[[ -n "$kver" && -d "/lib/modules/$kver/build" ]] ||
		{ printf 'error: no kernel headers found under /lib/modules/*/build\n' >&2; exit 1; }
	printf '%s\n' "$kver"
}

verify_dkms() {
	install_dkms_deps
	install_package

	local modname=thunderbolt-ibverbs
	local src_dir
	src_dir="$(find /usr/src -maxdepth 1 -type d -name "${modname}-*" -print -quit)"
	[[ -n "$src_dir" ]] ||
		{ printf 'error: %s source not under /usr/src after install\n' "$modname" >&2; exit 1; }
	local version
	version="$(awk -F'"' '/^PACKAGE_VERSION=/ { print $2; exit }' "$src_dir/dkms.conf")"

	local kver
	kver="$(find_dkms_kver)"

	printf '==> Source dir: %s\n' "$src_dir"
	printf '==> Version:    %s\n' "$version"
	printf '==> Kernel:     %s\n' "$kver"

	dkms status -m "$modname" -v "$version" || true

	if ! dkms build -m "$modname" -v "$version" -k "$kver" --force; then
		cat "/var/lib/dkms/$modname/$version/build/make.log" >&2 || true
		exit 1
	fi

	local built
	built="$(find "/var/lib/dkms/$modname/$version" -name 'thunderbolt_ibverbs.ko' -print -quit)"
	[[ -n "$built" ]] ||
		{ printf 'error: dkms build did not produce thunderbolt_ibverbs.ko\n' >&2; exit 1; }

	file "$built"
	modinfo "$built" | sed -n '1,20p'

	printf '==> DKMS install verification OK\n'
}

verify_provider() {
	install_provider_deps
	install_package

	local driver="/etc/libibverbs.d/usb4_rdma.driver"
	[[ -f "$driver" ]] ||
		{ printf 'error: driver hint missing at %s\n' "$driver" >&2; exit 1; }

	printf '==> Driver hint: %s\n' "$driver"
	cat "$driver"

	local so=""
	for d in /usr/lib/x86_64-linux-gnu/libibverbs /usr/lib64/libibverbs /usr/lib/libibverbs; do
		[[ -d "$d" ]] || continue
		so="$(find "$d" -maxdepth 1 -name 'libusb4_rdma-rdmav*.so' -print -quit)"
		[[ -n "$so" ]] && break
	done
	[[ -n "$so" ]] ||
		{ printf 'error: provider .so not found in any libibverbs dir\n' >&2; exit 1; }

	printf '==> Provider .so: %s\n' "$so"
	file "$so"

	printf '==> ldd lib resolution\n'
	if ldd "$so" 2>&1 | grep -E 'not found'; then
		printf 'error: unresolved dynamic library\n' >&2
		exit 1
	fi
	ldd "$so" | sed -n '1,12p'

	# ldd -r performs relocations and reports missing function references.
	# Catches PABI mismatches (e.g. wrong verbs_register_driver_<N> version)
	# and missing imports — without needing an actual /sys/class/infiniband
	# device for libibverbs to match against.
	printf '==> ldd -r symbol resolution\n'
	if ldd -r "$so" 2>&1 | grep -E 'undefined symbol'; then
		printf 'error: unresolved symbols in provider .so\n' >&2
		exit 1
	fi

	printf '==> ibv_devices smoke\n'
	ibv_devices

	printf '==> provider install verification OK\n'
}

case "$pkg_kind" in
	dkms)          verify_dkms ;;
	rdma-provider) verify_provider ;;
esac
