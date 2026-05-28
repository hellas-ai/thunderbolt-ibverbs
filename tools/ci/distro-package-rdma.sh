#!/usr/bin/env bash
# Build the usb4_rdma libibverbs userspace provider by applying our patches to
# rdma-core upstream, building with CMake/Ninja inside the target distro
# container, then packaging the resulting .so + driver hint as a native
# .deb/.rpm/.pkg.tar.zst. Building inside each distro's container gives a
# provider .so whose libibverbs PABI version matches that distro's libibverbs.

set -euo pipefail

usage() {
	cat <<'EOF'
Usage:
  tools/ci/distro-package-rdma.sh debian|fedora|arch

Outputs the produced .deb / .rpm / .pkg.tar.zst into OUT_DIR.

Environment:
  TBV_VERSION       Override version (default reads PACKAGE_VERSION from dkms.conf).
  OUT_DIR           Output directory (default $PWD/dist).
  WORK_DIR          Scratch directory (default mktemp).
  RDMA_CORE_TAG     rdma-core git tag to build against (default v62.0).
  TBV_SKIP_DEPS     Skip distro deps install (default 0).
  TBV_SKIP_BUILD    Skip the rdma-core build step (used by the arch builder
                    re-exec; default 0).
EOF
}

distro="${1:-}"
case "${distro:-}" in
	-h|--help) usage; exit 0 ;;
	debian|fedora|arch) ;;
	"") usage >&2; exit 1 ;;
	*) printf 'error: unsupported distro: %s\n' "$distro" >&2; exit 1 ;;
esac

repo_root="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
version="${TBV_VERSION:-$(awk -F'"' '/^PACKAGE_VERSION=/ { print $2; exit }' "$repo_root/dkms.conf")}"
[[ -n "$version" ]] || { printf 'error: could not determine version\n' >&2; exit 1; }
out_dir="${OUT_DIR:-$repo_root/dist}"
work_dir="${WORK_DIR:-$(mktemp -d)}"
rdma_core_tag="${RDMA_CORE_TAG:-v62.0}"
skip_deps="${TBV_SKIP_DEPS:-0}"
skip_build="${TBV_SKIP_BUILD:-0}"
pkgname="usb4-rdma-provider"

mkdir -p "$out_dir" "$work_dir"

install_deps() {
	[[ "$skip_deps" == "1" ]] && return 0
	case "$distro" in
		debian)
			export DEBIAN_FRONTEND=noninteractive
			apt-get update -qq
			apt-get install -y -qq --no-install-recommends \
				build-essential ca-certificates cmake dpkg-dev \
				git libcap-dev libnl-3-dev libnl-route-3-dev libsystemd-dev \
				libudev-dev libssl-dev ninja-build patch patchelf pkg-config \
				python3-docutils python3-pyelftools
			;;
		fedora)
			dnf install -y -q --setopt=install_weak_deps=False \
				cmake gcc gcc-c++ git libcap-devel libnl3-devel libudev-devel \
				make ninja-build openssl-devel patch patchelf pkgconf rpm-build \
				python3-docutils python3-pyelftools systemd-devel tar
			;;
		arch)
			pacman -Syu --noconfirm --needed \
				base-devel ca-certificates cmake git libnl ninja patch \
				patchelf python-docutils python-pyelftools sudo systemd
			;;
	esac
}

build_provider() {
	[[ "$skip_build" == "1" ]] && return 0
	local src="$work_dir/rdma-core"
	local build="$src/build"

	if [[ ! -d "$src" ]]; then
		git clone --depth 1 --branch "$rdma_core_tag" \
			https://github.com/linux-rdma/rdma-core "$src"
	fi

	for p in "$repo_root/packaging/rdma-core-patches"/*.patch; do
		( cd "$src" && patch --silent -p1 < "$p" )
	done

	rm -rf "$build"
	mkdir "$build"
	( cd "$build" && cmake -GNinja -DNO_PYVERBS=1 .. >/dev/null && ninja )

	local so
	so="$(find "$build/lib" -maxdepth 1 -name 'libusb4_rdma-rdmav*.so' -print -quit)"
	[[ -n "$so" ]] || { printf 'error: provider .so not produced\n' >&2; exit 1; }

	# CMake embeds the build dir as RUNPATH so libibverbs can run from the
	# build tree without installing. For packaging we want a clean .so with no
	# build-host paths leaked — strip the RUNPATH.
	patchelf --remove-rpath "$so"

	printf '==> Built provider: %s\n' "$(basename "$so")"
}

stage_provider_files() {
	local stage="$1"
	local src="$work_dir/rdma-core"
	local build="$src/build"

	install -d -m 0755 "$stage"

	local so
	so="$(find "$build/lib" -maxdepth 1 -name 'libusb4_rdma-rdmav*.so' -print -quit)"
	[[ -n "$so" ]] || { printf 'error: provider .so not in build tree\n' >&2; exit 1; }

	cp "$so" "$stage/"
	# The .driver file emitted by rdma-core's build tree embeds the absolute
	# build-tree path so libibverbs can run from the build dir without install.
	# For packaging, write the plain installed-tree form: `driver <name>`.
	printf 'driver usb4_rdma\n' > "$stage/usb4_rdma.driver"
}

substitute() {
	sed "s/@VERSION@/${version}/g" "$1" > "$2"
}

build_deb() {
	local arch
	arch="$(dpkg-architecture -q DEB_HOST_MULTIARCH)"
	local deb_stage="$work_dir/deb"
	rm -rf "$deb_stage"
	install -d -m 0755 "$deb_stage/DEBIAN"
	install -d -m 0755 "$deb_stage/usr/lib/$arch/libibverbs"
	install -d -m 0755 "$deb_stage/etc/libibverbs.d"

	local files="$work_dir/files"
	stage_provider_files "$files"

	install -m 0644 "$files"/libusb4_rdma-rdmav*.so \
		"$deb_stage/usr/lib/$arch/libibverbs/"
	install -m 0644 "$files/usb4_rdma.driver" \
		"$deb_stage/etc/libibverbs.d/usb4_rdma.driver"

	substitute "$repo_root/packaging/debian/control-rdma" "$deb_stage/DEBIAN/control"

	local deb="$out_dir/${pkgname}_${version}_amd64.deb"
	dpkg-deb --root-owner-group --build "$deb_stage" "$deb" >/dev/null
	printf '==> Built %s\n' "$deb"
}

build_rpm() {
	local rpm_top="$work_dir/rpmbuild"
	rm -rf "$rpm_top"
	install -d -m 0755 "$rpm_top"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}

	stage_provider_files "$rpm_top/SOURCES"

	substitute "$repo_root/packaging/rpm/${pkgname}.spec" \
		"$rpm_top/SPECS/${pkgname}.spec"

	rpmbuild --define "_topdir $rpm_top" -bb \
		"$rpm_top/SPECS/${pkgname}.spec"

	local rpm
	rpm="$(find "$rpm_top/RPMS" -name '*.rpm' -print -quit)"
	[[ -n "$rpm" ]] || { printf 'error: rpmbuild produced no .rpm\n' >&2; exit 1; }

	cp "$rpm" "$out_dir/"
	printf '==> Built %s\n' "$out_dir/$(basename "$rpm")"
}

build_arch_as_builder() {
	local stage="$work_dir/arch"
	rm -rf "$stage"
	install -d -m 0755 "$stage"

	stage_provider_files "$stage"

	local soname
	soname="$(basename "$(find "$stage" -name 'libusb4_rdma-rdmav*.so' -print -quit)")"
	[[ -n "$soname" ]] || { printf 'error: staged .so missing\n' >&2; exit 1; }

	sed -e "s/@VERSION@/${version}/g" -e "s/@SONAME@/${soname}/g" \
		"$repo_root/packaging/arch/PKGBUILD-rdma" > "$stage/PKGBUILD"

	( cd "$stage" && makepkg --noconfirm --skipchecksums --nodeps )

	local pkg
	pkg="$(find "$stage" -maxdepth 1 -name "${pkgname}-[0-9]*.pkg.tar.zst" -print -quit)"
	[[ -n "$pkg" ]] || { printf 'error: makepkg produced no main package\n' >&2; exit 1; }

	cp "$pkg" "$out_dir/"
	printf '==> Built %s\n' "$out_dir/$(basename "$pkg")"
}

build_arch() {
	# makepkg refuses to run as root. Build rdma-core as root (above), then
	# re-exec as a builder user for the packaging step.
	if [[ "$(id -u)" -eq 0 ]]; then
		id -u builder >/dev/null 2>&1 || useradd -m -s /bin/bash builder
		chown -R builder:builder "$work_dir" "$out_dir"
		exec sudo -u builder \
			env TBV_VERSION="$version" OUT_DIR="$out_dir" \
				WORK_DIR="$work_dir" RDMA_CORE_TAG="$rdma_core_tag" \
				TBV_SKIP_DEPS=1 TBV_SKIP_BUILD=1 \
			bash "$0" "$distro"
	fi
	build_arch_as_builder
}

install_deps
build_provider

case "$distro" in
	debian) build_deb ;;
	fedora) build_rpm ;;
	arch)   build_arch ;;
esac
