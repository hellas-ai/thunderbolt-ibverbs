#!/usr/bin/env bash
# Build a DKMS source package for the given distro using the distro's native
# packaging tooling. Run inside a container that matches the target distro
# (debian:sid, fedora:rawhide, archlinux:latest). The script installs the
# build dependencies it needs.

set -euo pipefail

usage() {
	cat <<'EOF'
Usage:
  tools/ci/distro-package.sh debian|fedora|arch

Outputs the produced .deb / .rpm / .pkg.tar.zst into OUT_DIR.

Environment:
  TBV_VERSION     Override package version. Defaults to PACKAGE_VERSION read
                  from dkms.conf.
  OUT_DIR         Output directory. Defaults to $(pwd)/dist.
  WORK_DIR        Scratch directory. Defaults to a fresh mktemp dir.
  TBV_LINT        Run lintian / rpmlint / namcap on the artefact. 1 to enable.
                  Defaults to 0.
  TBV_SKIP_DEPS   Skip the distro deps install step. 1 to skip. Useful for
                  local dev when deps are already present.
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
[[ -n "$version" ]] || { printf 'error: could not determine version from dkms.conf\n' >&2; exit 1; }
out_dir="${OUT_DIR:-$repo_root/dist}"
work_dir="${WORK_DIR:-$(mktemp -d)}"
lint="${TBV_LINT:-0}"
skip_deps="${TBV_SKIP_DEPS:-0}"
modname="thunderbolt-ibverbs"
pkgname="${modname}-dkms"

mkdir -p "$out_dir" "$work_dir"

install_deps() {
	[[ "$skip_deps" == "1" ]] && return 0
	case "$distro" in
		debian)
			export DEBIAN_FRONTEND=noninteractive
			apt-get update -qq
			apt-get install -y -qq --no-install-recommends \
				ca-certificates dpkg-dev fakeroot lintian
			;;
		fedora)
			dnf install -y -q --setopt=install_weak_deps=False \
				ca-certificates coreutils rpm-build rpmlint tar
			;;
		arch)
			pacman -Sy --noconfirm --needed \
				base-devel ca-certificates dkms namcap sudo
			;;
	esac
}

stage_source() {
	local stage="$1"
	install -d -m 0755 "$stage"
	tar -C "$repo_root" -cf - \
		Makefile \
		dkms.conf \
		LICENSE \
		README.md \
		kernel \
		proto \
		| tar -C "$stage" -xf -
}

make_source_tarball() {
	local top="$work_dir/${modname}-${version}"
	rm -rf "$top"
	stage_source "$top"
	tar -C "$work_dir" -czf "$work_dir/${modname}-${version}.tar.gz" \
		"${modname}-${version}"
	printf '%s\n' "$work_dir/${modname}-${version}.tar.gz"
}

substitute() {
	sed "s/@VERSION@/${version}/g" "$1" > "$2"
}

build_deb() {
	local stage="$work_dir/deb"
	rm -rf "$stage"
	install -d -m 0755 "$stage/DEBIAN"
	stage_source "$stage/usr/src/${modname}-${version}"

	substitute "$repo_root/packaging/debian/control" "$stage/DEBIAN/control"
	substitute "$repo_root/packaging/debian/postinst" "$stage/DEBIAN/postinst"
	substitute "$repo_root/packaging/debian/prerm" "$stage/DEBIAN/prerm"
	chmod 0755 "$stage/DEBIAN/postinst" "$stage/DEBIAN/prerm"

	local deb="$out_dir/${pkgname}_${version}_all.deb"
	dpkg-deb --root-owner-group --build "$stage" "$deb" >/dev/null
	printf '==> Built %s\n' "$deb"

	if [[ "$lint" == "1" ]] && command -v lintian >/dev/null 2>&1; then
		printf '==> lintian\n'
		lintian --no-tag-display-limit \
			--suppress-tags no-changelog,no-manual-page,no-copyright-file,extended-description-is-probably-too-short,initial-upload-closes-no-bugs,debian-changelog-file-missing \
			"$deb" || true
	fi
}

build_rpm() {
	local rpm_top="$work_dir/rpmbuild"
	rm -rf "$rpm_top"
	install -d -m 0755 "$rpm_top"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}

	local tarball
	tarball="$(make_source_tarball)"
	cp "$tarball" "$rpm_top/SOURCES/"

	substitute "$repo_root/packaging/rpm/${pkgname}.spec" "$rpm_top/SPECS/${pkgname}.spec"

	rpmbuild --define "_topdir $rpm_top" -bb "$rpm_top/SPECS/${pkgname}.spec"

	local rpm
	rpm="$(find "$rpm_top/RPMS" -name '*.rpm' -print -quit)"
	[[ -n "$rpm" ]] || { printf 'error: rpmbuild produced no .rpm\n' >&2; exit 1; }

	cp "$rpm" "$out_dir/"
	printf '==> Built %s\n' "$out_dir/$(basename "$rpm")"

	if [[ "$lint" == "1" ]] && command -v rpmlint >/dev/null 2>&1; then
		printf '==> rpmlint\n'
		rpmlint "$out_dir/$(basename "$rpm")" || true
	fi
}

build_arch_as_builder() {
	local stage="$work_dir/arch"
	rm -rf "$stage"
	install -d -m 0755 "$stage"

	local tarball
	tarball="$(make_source_tarball)"
	cp "$tarball" "$stage/"

	substitute "$repo_root/packaging/arch/PKGBUILD" "$stage/PKGBUILD"
	cp "$repo_root/packaging/arch/${pkgname}.install" "$stage/"

	( cd "$stage" && makepkg --noconfirm --skipchecksums )

	local pkg
	pkg="$(find "$stage" -name '*.pkg.tar.zst' -print -quit)"
	[[ -n "$pkg" ]] || { printf 'error: makepkg produced no package\n' >&2; exit 1; }

	cp "$pkg" "$out_dir/"
	printf '==> Built %s\n' "$out_dir/$(basename "$pkg")"

	if [[ "$lint" == "1" ]] && command -v namcap >/dev/null 2>&1; then
		printf '==> namcap\n'
		namcap "$out_dir/$(basename "$pkg")" || true
	fi
}

build_arch() {
	# makepkg refuses to run as root. If we are root, create a builder user
	# and re-exec this script as that user.
	if [[ "$(id -u)" -eq 0 ]]; then
		id -u builder >/dev/null 2>&1 || useradd -m -s /bin/bash builder
		chown -R builder:builder "$work_dir" "$out_dir"
		exec sudo -u builder \
			env TBV_VERSION="$version" OUT_DIR="$out_dir" \
				WORK_DIR="$work_dir" TBV_LINT="$lint" \
				TBV_SKIP_DEPS=1 \
			bash "$0" "$distro"
	fi
	build_arch_as_builder
}

install_deps

case "$distro" in
	debian) build_deb ;;
	fedora) build_rpm ;;
	arch)   build_arch ;;
esac
