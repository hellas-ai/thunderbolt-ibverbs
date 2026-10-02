# Kernel Patches

Linux 7.2 is the baseline for the kernel packages and patch stacks. It already
contains the 21 Thunderbolt maintainer-tree commits previously carried here,
the XDomain delayed-work lifetime fix, and the bounded response copy. Those
23 patches and the old maintainer-tree regeneration script have been removed.
Linux 7.1 backports are no longer maintained in this repository.

Two stacks remain:

- `portable.nix`: eight local patches for stock Linux 7.2, including the
  optional lane-bonding switch. The switch preserves automatic bonding by
  default.
- `local.nix`: eight local patches used by `linux-thunderbolt`. This stack
  includes NHI ring diagnostics instead of the lane-bonding switch.

The shared patches provide DMA scheduling controls, interrupt rearming,
protocol diagnostics, source-aware XDomain callbacks, handler lifetime
protection, property identity matching, and callback draining. Linux 7.2 does
not provide these local changes.

`local-portable.nix` derives the portable stack from `local.nix`, excluding
`local-integration-debug.nix` and adding the lane-bonding switch. `default.nix`
imports `portable.nix`. The integration name is retained for compatibility;
`linux-thunderbolt` now uses nixpkgs' Linux 7.2 source and configuration.

For a NixOS kernel supplied by the consuming configuration:

```nix
boot.kernelPatches = thunderbolt-ibverbs.lib.kernelPatchesFor
  config.boot.kernelPackages.kernel.version;
```

`kernelPatchesFor` rejects kernels older than 7.2 rather than returning an
incomplete backport stack. Later versions still require patch-application and
build validation when updating nixpkgs.

Flake exports:

- `lib.kernelPatches` and `lib.portableKernelPatches`: portable stack.
- `lib.kernelPatchesFor`: portable stack with a minimum-version check.
- `lib.integrationKernelPatches`: project-kernel stack with ring diagnostics.
- `lib.portableLocalKernelPatches`: compatibility alias for the portable stack.
- `lib.upstreamKernelPatches`: empty compatibility export; no backports remain.

The same names are exposed under `legacyPackages.${system}` for NixOS configs
that cannot conveniently read flake `lib` attributes.
