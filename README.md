# thunderbolt-ibverbs

Experimental Linux kernel module exposing an RDMA verbs device over
Thunderbolt/USB4 host-to-host links.

This is a research driver. It is useful for controlled Linux-to-Linux testing,
but it is not a production storage, cluster, or security boundary. Use it on
machines you can reboot, over links to machines you trust.

For the background, hardware notes, and benchmark narrative, see the Hellas
blog post:

https://blog.hellas.ai/blog/thunderbolt-ibverbs/

## Status

- Native Linux-to-Linux verbs transport is the main path.
- Apple-compatible transport exists, but is still experimental.
- The module builds against stock kernels, but needs Linux 6.14 or newer
  (or this flake's `linux-thunderbolt` kernel) for the maintainer-tree
  Thunderbolt/USB4 subsystem changes it relies on.
- `nhi_interrupt_throttle_ns` is active only on kernels that export
  `tb_ring_throttling()`.
- The Nix flake builds a Thunderbolt testing kernel from the maintainer
  `next` branch with the local kernel patches applied.
- Debian, Fedora, Arch, and Nix builds are exercised in CI.

## License

The kernel module is licensed under GPL-2.0-only, matching the SPDX tags in the
kernel sources and `MODULE_LICENSE("GPL")`.

Small userspace-facing test and protocol helper files that say
`GPL-2.0 OR BSD-3-Clause` may be used under either license.

## Install From GitHub Releases

Pre-built DKMS source packages are attached to GitHub Releases:

  https://github.com/hellas-ai/thunderbolt-ibverbs/releases

Each release ships two packages per distro: the DKMS source package for the
kernel module, and a userspace libibverbs provider so `ibv_devices` and
downstream RDMA tools (NCCL, perftest, vllm) enumerate the device.

```sh
# Debian or Ubuntu (needs Linux 6.14+)
sudo apt install \
    ./thunderbolt-ibverbs-dkms_<ver>_all.deb \
    ./usb4-rdma-provider_<ver>_amd64.deb

# Fedora
sudo dnf install \
    ./thunderbolt-ibverbs-dkms-<ver>-1.noarch.rpm \
    ./usb4-rdma-provider-<ver>-1.x86_64.rpm

# Arch
sudo pacman -U \
    ./thunderbolt-ibverbs-dkms-<ver>-1-any.pkg.tar.zst \
    ./usb4-rdma-provider-<ver>-1-x86_64.pkg.tar.zst
```

DKMS builds the kernel module against your running kernel on install and
rebuilds it after every kernel upgrade. Older kernels need the
`linux-thunderbolt` build from this flake — see "Nix Thunderbolt Kernel" below.

## Requirements

Install matching kernel headers and the basic module build tools.

Debian or Ubuntu:

```sh
sudo apt install build-essential dkms git kmod "linux-headers-$(uname -r)" rdma-core perftest
```

Fedora:

```sh
sudo dnf install dkms gcc git kernel-devel kernel-headers kmod make rdma-core perftest
```

Arch Linux:

```sh
sudo pacman -S --needed base-devel dkms git kmod linux-headers rdma-core perftest
```

## Install With DKMS

```sh
git clone https://github.com/hellas-ai/thunderbolt-ibverbs.git
cd thunderbolt-ibverbs

sudo make dkms-add
sudo make dkms-build
sudo make dkms-install
```

After a kernel upgrade, DKMS should rebuild the module for the new kernel.

To remove it:

```sh
sudo make dkms-remove
```

## Build Without DKMS

For a one-off build against the running kernel:

```sh
make KVER="$(uname -r)"
sudo make KVER="$(uname -r)" modules_install
sudo depmod -a
```

## Nix

Build the module package:

```sh
nix build github:hellas-ai/thunderbolt-ibverbs#thunderbolt-ibverbs
```

On NixOS, add the flake input and import the module:

```nix
{
  inputs.thunderbolt-ibverbs.url = "github:hellas-ai/thunderbolt-ibverbs";

  outputs = { nixpkgs, thunderbolt-ibverbs, ... }: {
    nixosConfigurations.host = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        thunderbolt-ibverbs.nixosModules.default
        {
          hardware.thunderbolt-ibverbs.enable = true;
        }
      ];
    };
  };
}
```

## Load And Use

Connect the Thunderbolt/USB4 hosts first. On both Linux peers, load the module
with the native Linux transport enabled:

```sh
sudo modprobe thunderbolt_ibverbs \
  profile=linux_perf \
  bind_services=1 \
  allocate_rings=1 \
  start_rings=1 \
  negotiate_native=1 \
  enable_tunnels=1 \
  register_verbs=1
```

If userspace needs a RoCE netdev for GID metadata, pass one explicitly:

```sh
sudo modprobe thunderbolt_ibverbs \
  profile=linux_perf \
  bind_services=1 allocate_rings=1 start_rings=1 \
  negotiate_native=1 enable_tunnels=1 register_verbs=1 \
  roce_netdev=thunderbolt0
```

Check that the device registered:

```sh
dmesg | grep thunderbolt_ibverbs
ibv_devices
rdma link
```

With `perftest` installed, select the reported RDMA device explicitly:

```sh
# peer A
ib_write_bw -d usb4_rdma0

# peer B
ib_write_bw -d usb4_rdma0 <peer-a-address>
```

Unload the module before changing static load parameters:

```sh
sudo modprobe -r thunderbolt_ibverbs
```

To make a known-good configuration persistent, put the options in
`/etc/modprobe.d/thunderbolt-ibverbs.conf`.

## Useful Parameters

```text
profile=linux_perf|mac_compat|mixed
tbnet=auto|allow|prefer_rdma|block
lanes=auto|N|MIN-MAX
register_verbs=0|1
native_wr_striping=0|1
native_fragment_striping=0|1
zcopy_min_bytes=<bytes>
qp_timeout_ms=<ms>
nhi_interrupt_throttle_ns=<ns>
```

Run `make -C kernel help` for the full parameter list.

## Nix Thunderbolt Kernel

The module loads on stock kernels. For the maintainer-tree USB4 work, the flake
also exposes `linux-thunderbolt`: nixpkgs' `linuxPackages_testing.kernel` with
only the source, version, and kernel patch list overridden. It uses the nixpkgs
testing kernel configuration, not a machine-local config.

```sh
nix build .#linux-thunderbolt
nix build .#thunderbolt-ibverbs-linux-thunderbolt
```

On NixOS, use that kernel package set and enable the module:

```nix
{ pkgs, inputs, ... }:
let
  system = pkgs.stdenv.hostPlatform.system;
  tbv = inputs.thunderbolt-ibverbs.packages.${system};
in {
  boot.kernelPackages = pkgs.linuxPackagesFor tbv.linux-thunderbolt;
  hardware.thunderbolt-ibverbs.enable = true;
}
```

Hydra evaluates the same path through
`hydraJobs.x86_64-linux.linux-thunderbolt` and
`hydraJobs.x86_64-linux.thunderbolt-ibverbs-linux-thunderbolt`.
