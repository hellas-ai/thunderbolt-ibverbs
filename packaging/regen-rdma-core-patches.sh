#!/usr/bin/env bash
# Regenerate the rdma-core patches that ship our libibverbs provider.
#
# Pulls a clean rdma-core source tarball (the version nixpkgs is pinned
# to), checks in our provider's source files, and writes two numbered
# patches into ./packaging/rdma-core-patches/ that any rdma-core build
# can apply. Run this after touching userspace/usb4_rdma/ files.

set -euo pipefail

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$REPO_ROOT/packaging/rdma-core-patches"

# Pull the rdma-core source via Nix; matches whatever pkgs.rdma-core uses.
RDMA_SRC=$(nix eval --raw "$REPO_ROOT"'#legacyPackages.x86_64-linux.linux_thunderbolt'.outPath 2>/dev/null \
            || nix-build "<nixpkgs>" -A rdma-core.src --no-out-link)
if [ ! -d "$RDMA_SRC" ]; then
    # The .src can be either a directory (github fetcher) or a tarball.
    RDMA_SRC=$(nix-build '<nixpkgs>' -A rdma-core.src --no-out-link)
fi
echo "Using rdma-core source: $RDMA_SRC"

WORK=$(mktemp -d -t rdma-patches-XXXXXX)
trap 'rm -rf "$WORK"' EXIT

cp -r "$RDMA_SRC"/. "$WORK/"
chmod -R u+w "$WORK"
cd "$WORK"

git init -q
git config user.email "ci@thunderbolt-ibverbs"
git config user.name "thunderbolt-ibverbs"
git config commit.gpgsign false
git add -A
git commit -qm "rdma-core baseline"

# Drop our provider source in.
mkdir -p providers/usb4_rdma
cp -r "$REPO_ROOT/userspace/usb4_rdma/." providers/usb4_rdma/
git add providers/usb4_rdma
git commit -qm "providers/usb4_rdma: add USB4 soft-RDMA provider

Out-of-tree provider for the usb4_rdma kernel module which exposes
Thunderbolt/USB4 host-to-host xdomain links as InfiniBand verbs
devices.

Source: https://github.com/hellas-ai/thunderbolt-ibverbs"

# Wire the provider into the build.
sed -i '/add_subdirectory(providers\/siw)/a add_subdirectory(providers/usb4_rdma)' CMakeLists.txt
git add CMakeLists.txt
git commit -qm "CMakeLists.txt: build the usb4_rdma provider"

# Declare the provider in the public header so the static-link
# all_providers.c indirection sees it. rdma-core hand-maintains this
# list — every in-tree provider has an extern in libibverbs/verbs.h.
sed -i '/extern const struct verbs_device_ops verbs_provider_siw;/a extern const struct verbs_device_ops verbs_provider_usb4_rdma;' libibverbs/verbs.h
git add libibverbs/verbs.h
git commit -qm "libibverbs/verbs.h: declare verbs_provider_usb4_rdma

Required for the static-archive build path (libibverbs/all_providers.c)
to compile when ENABLE_STATIC=1 is on, which is the default on Debian
and most distros."

mkdir -p "$OUT"
rm -f "$OUT"/*.patch
git format-patch -o "$OUT" HEAD~3

echo ""
echo "Regenerated patches in $OUT:"
ls -la "$OUT"/*.patch
