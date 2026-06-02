#!/usr/bin/env bash
# Build the module the way non-Nix users will: inside a distro container,
# against that distro's packaged kernel headers, through both plain make and
# DKMS. Also compiles the small userspace-facing protocol header smoke test.

set -euo pipefail

usage() {
	cat <<'EOF'
Usage:
  tools/ci/distro-build.sh [image]

Examples:
  tools/ci/distro-build.sh debian:sid
  tools/ci/distro-build.sh fedora:rawhide
  tools/ci/distro-build.sh archlinux:latest
  DOCKER=podman tools/ci/distro-build.sh debian:sid
EOF
}

repo_root="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
	usage
	exit 0
fi

if [[ "${1:-}" != "--inside" ]]; then
	image="${1:-debian:sid}"
	docker_bin="${DOCKER:-docker}"
	if ! command -v "$docker_bin" >/dev/null 2>&1; then
		printf 'error: %s not found; set DOCKER=podman or install Docker\n' "$docker_bin" >&2
		exit 1
	fi

	exec "$docker_bin" run --rm \
		-v "$repo_root:/repo:ro" \
		-w /repo \
		"$image" \
		bash /repo/tools/ci/distro-build.sh --inside
fi

install_deps() {
	if command -v apt-get >/dev/null 2>&1; then
		apt-get update -qq
		DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
			bash build-essential ca-certificates dkms file gcc git kmod \
			linux-headers-amd64 make tar xz-utils
	elif command -v dnf >/dev/null 2>&1; then
		dnf install -y -q --setopt=install_weak_deps=False \
			bash ca-certificates diffutils dkms file findutils gcc git \
			kernel-devel kernel-headers kmod make openssl tar xz >/dev/null
	elif command -v pacman >/dev/null 2>&1; then
		pacman -Syu --noconfirm --needed \
			base-devel ca-certificates dkms file gcc git kmod linux-headers \
			make openssl pahole tar xz
	else
		cat /etc/os-release >&2 || true
		printf 'error: unsupported image; expected apt-get, dnf, or pacman\n' >&2
		exit 1
	fi
}

find_kernel_version() {
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

	if [[ -z "$kver" || ! -d "/lib/modules/$kver/build" ]]; then
		printf 'error: could not find packaged kernel headers under /lib/modules/*/build\n' >&2
		exit 1
	fi

	printf '%s\n' "$kver"
}

clean_artifacts() {
	find "$1" -type f \( \
		-name '*.o' -o -name '*.ko' -o -name '*.cmd' \
		-o -name '*.mod' -o -name '*.mod.c' \
		-o -name 'Module.symvers' -o -name 'modules.order' \
		-o -name '.Module.symvers.cmd' \
	\) -delete
	find "$1" -type d \( -name '.tmp_versions' -o -name '.cache' \) \
		-prune -exec rm -rf {} +
}

copy_source() {
	mkdir -p /work/src
	tar -C /repo \
		--exclude=.git \
		--exclude=result \
		--exclude=dist \
		-cf - . | tar -C /work/src -xf -
	clean_artifacts /work/src
}

build_with_make() {
	local kver="$1"

	make -C /work/src KVER="$kver" KDIR="/lib/modules/$kver/build" modules
	modinfo /work/src/kernel/thunderbolt_ibverbs.ko | sed -n '1,20p'
	make -C /work/src KVER="$kver" KDIR="/lib/modules/$kver/build" clean
}

build_userspace_smoke() {
	gcc -std=c11 -Wall -Wextra -Werror -I/work/src \
		/work/src/tools/ci/proto-smoke.c \
		-o /tmp/tbv-proto-smoke
	/tmp/tbv-proto-smoke
	gcc -std=c11 -Wall -Wextra -Werror -I/work/src \
		/work/src/tools/ci/reliability-smoke.c \
		/work/src/proto/reliability.c \
		-o /tmp/tbv-reliability-smoke
	/tmp/tbv-reliability-smoke
}

build_with_dkms() {
	local kver="$1"
	local pkg="thunderbolt-ibverbs"
	local ver="0.1.0"
	local dkms_src="/usr/src/$pkg-$ver"
	local built

	rm -rf "$dkms_src"
	cp -a /work/src "$dkms_src"
	clean_artifacts "$dkms_src"

	dkms add -m "$pkg" -v "$ver"
	if ! dkms build -m "$pkg" -v "$ver" -k "$kver"; then
		cat "/var/lib/dkms/$pkg/$ver/build/make.log" >&2 || true
		exit 1
	fi

	built="$(find "/var/lib/dkms/$pkg/$ver" -name thunderbolt_ibverbs.ko \
		-print -quit)"
	if [[ -z "$built" ]]; then
		printf 'error: DKMS build succeeded but thunderbolt_ibverbs.ko was not found\n' >&2
		exit 1
	fi

	file "$built"
	modinfo "$built" | sed -n '1,20p'
}

install_deps
copy_source

printf '==> Distro\n'
cat /etc/os-release | grep -E '^(PRETTY_NAME|NAME|VERSION)=' || true

kver="$(find_kernel_version)"
printf '==> Kernel headers: %s\n' "$kver"

printf '==> make modules\n'
build_with_make "$kver"

printf '==> userspace protocol header smoke\n'
build_userspace_smoke

printf '==> DKMS build\n'
build_with_dkms "$kver"

printf '==> distro build OK\n'
