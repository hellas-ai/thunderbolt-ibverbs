#!/usr/bin/env bash
# Install a DKMS source package built by tools/ci/distro-package.sh and verify
# DKMS can build the module against the distro's packaged kernel-headers. Does
# NOT load the module — that needs a real distro kernel and lives in
# tools/ci/vm-smoke.sh.

set -euo pipefail

usage() {
	cat <<'EOF'
Usage:
  tools/ci/distro-install.sh <artefact-path-or-glob>

Detects the distro from the artefact extension (.deb, .rpm, .pkg.tar.zst).
Installs the package, then runs `dkms build` against the latest packaged
kernel-headers. Verifies thunderbolt_ibverbs.ko is produced.
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

install_deps() {
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

install_deps
install_package

modname=thunderbolt-ibverbs
src_dir="$(find /usr/src -maxdepth 1 -type d -name "${modname}-*" -print -quit)"
[[ -n "$src_dir" ]] ||
	{ printf 'error: %s source not under /usr/src after install\n' "$modname" >&2; exit 1; }
version="$(awk -F'"' '/^PACKAGE_VERSION=/ { print $2; exit }' "$src_dir/dkms.conf")"

kver="$(find_dkms_kver)"

printf '==> Source dir: %s\n' "$src_dir"
printf '==> Version:    %s\n' "$version"
printf '==> Kernel:     %s\n' "$kver"

dkms status -m "$modname" -v "$version" || true

if ! dkms build -m "$modname" -v "$version" -k "$kver" --force; then
	cat "/var/lib/dkms/$modname/$version/build/make.log" >&2 || true
	exit 1
fi

built="$(find "/var/lib/dkms/$modname/$version" -name 'thunderbolt_ibverbs.ko' -print -quit)"
[[ -n "$built" ]] ||
	{ printf 'error: dkms build did not produce thunderbolt_ibverbs.ko\n' >&2; exit 1; }

file "$built"
modinfo "$built" | sed -n '1,20p'

printf '==> install verification OK\n'
