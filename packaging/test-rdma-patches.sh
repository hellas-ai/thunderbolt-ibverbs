#!/usr/bin/env bash
# Smoke-test the rdma-core patches on a clean distro container.
#
# Pulls upstream rdma-core v62.0 source, applies our two patches, runs
# the distro-native rdma-core CMake build, verifies that
# `libusb4_rdma-rdmav*.so` and the `usb4_rdma.driver` config are produced.
# Does not install or load anything — just confirms patches apply and
# build succeeds in the distro's toolchain.
#
# Usage:
#   ./packaging/test-rdma-patches.sh                # debian:sid (default)
#   ./packaging/test-rdma-patches.sh ubuntu:24.04
#   ./packaging/test-rdma-patches.sh archlinux:latest
#   ./packaging/test-rdma-patches.sh fedora:rawhide

set -euo pipefail

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMAGE=${1:-debian:sid}
DOCKER=${DOCKER:-docker}

# Pin to a tag the patches were generated against. Bump in lockstep with
# `nixpkgs#rdma-core` and `regen-rdma-core-patches.sh`.
RDMA_CORE_TAG=${RDMA_CORE_TAG:-v62.0}

STAGE=$(mktemp -d -t tb-ibv-rdma-patches-XXXXXX)
trap 'rm -rf "$STAGE"' EXIT
cp -r "$REPO_ROOT/packaging/rdma-core-patches" "$STAGE/patches"

echo "=== Testing rdma-core $RDMA_CORE_TAG + our patches on $IMAGE ==="
echo ""

$DOCKER run --rm \
    -v "$STAGE:/work:ro" \
    -e RDMA_CORE_TAG="$RDMA_CORE_TAG" \
    "$IMAGE" bash -euxc '
        if command -v apt-get >/dev/null; then
            apt-get update -qq
            DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
                git ca-certificates cmake build-essential pkg-config \
                libnl-3-dev libnl-route-3-dev libudev-dev libsystemd-dev \
                python3-pyelftools python3-docutils libssl-dev libcap-dev \
                ninja-build patch
        elif command -v pacman >/dev/null; then
            pacman -Sy --noconfirm --quiet \
                base-devel git cmake \
                libnl systemd python-pyelftools python-docutils \
                ninja >/dev/null
        elif command -v dnf >/dev/null; then
            dnf install -y -q \
                git cmake gcc gcc-c++ make pkgconf \
                libnl3-devel libudev-devel systemd-devel \
                python3-pyelftools python3-docutils openssl-devel libcap-devel \
                ninja-build patch
        else
            echo "Unknown distro" >&2
            exit 1
        fi

        cd /tmp
        git clone --depth=1 --branch "$RDMA_CORE_TAG" https://github.com/linux-rdma/rdma-core
        cd rdma-core
        patch -p1 < /work/patches/0001-*.patch
        patch -p1 < /work/patches/0002-*.patch

        mkdir build && cd build
        cmake -GNinja -DNO_PYVERBS=1 ..
        ninja

        echo ""
        echo "=== Verify our provider was built ==="
        ls -l lib/libusb4_rdma-rdmav*.so
        ls -l etc/libibverbs.d/usb4_rdma.driver
        cat etc/libibverbs.d/usb4_rdma.driver

        echo "==> rdma-core+patches build OK on $(grep PRETTY_NAME /etc/os-release | cut -d= -f2)"
    '

echo ""
echo "rdma-core patches verified on $IMAGE."
