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
- The module builds against stock kernels.
- The patch in `patches/linux/` is optional. It only enables the
  `nhi_interrupt_throttle_ns` tuning parameter.
- Debian, Fedora, Arch, and Nix builds are exercised in CI.

## License

The kernel module is licensed under GPL-2.0-only, matching the SPDX tags in the
kernel sources and `MODULE_LICENSE("GPL")`.

Small userspace-facing test and protocol helper files that say
`GPL-2.0 OR BSD-3-Clause` may be used under either license.

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

## Optional Kernel Patch

The module loads without any kernel patch. Without the patch, it uses the
stock Thunderbolt NHI interrupt behavior and ignores non-zero
`nhi_interrupt_throttle_ns` values.

To enable that tuning knob, apply:

```text
patches/linux/0001-thunderbolt-nhi-add-per-ring-interrupt-throttling-helper.patch
```

For a local kernel tree:

```sh
cd linux
git am /path/to/thunderbolt-ibverbs/patches/linux/0001-thunderbolt-nhi-add-per-ring-interrupt-throttling-helper.patch
make olddefconfig
make -j"$(nproc)"
```

For NixOS, the flake exposes the patch as
`inputs.thunderbolt-ibverbs.lib.kernelPatches`:

```nix
{ pkgs, inputs, ... }:
let
  linuxPackagesTbv = pkgs.linuxPackages_latest.extend (_self: super: {
    kernel = super.kernel.override {
      kernelPatches =
        (super.kernel.kernelPatches or [])
        ++ inputs.thunderbolt-ibverbs.lib.kernelPatches;
    };
  });
in {
  boot.kernelPackages = linuxPackagesTbv;
  hardware.thunderbolt-ibverbs.enable = true;
}
```

Reboot into the patched kernel, then rebuild or reload the module.
