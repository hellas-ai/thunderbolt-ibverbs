# Kernel Patches

This directory carries these patch sets:

- `upstream-thunderbolt-next.nix`: Thunderbolt maintainer-tree commits that are
  not guaranteed to be present in a stock kernel.
- `local-portable.nix`: project-local patches that apply after the upstream
  layer on a stock kernel.
- `local-integration-debug.nix`: debug patches that are only carried for the
  `westeri/thunderbolt.git` integration-tree kernel.
- `local.nix`: all local patches for the integration-tree kernel.
- `portable.nix`: upstream maintainer-tree commits plus local patches that
  apply on a stock kernel.

`default.nix` intentionally preserves the historical local-only patch stack for
existing consumers. Use `portable.nix` or the `portableKernelPatches` flake
export to patch a normal kernel, for example `pkgs.linuxPackages_latest`.
`portable.nix` excludes `local-integration-debug.nix` because that patch does
not apply cleanly to the stock kernel after the upstream layer.

The flake's `linux-thunderbolt` package uses `westeri/thunderbolt.git` as its
kernel source, so it applies only `local.nix`. Applying
`upstream-thunderbolt-next.nix` there would double-apply the same maintainer
commits.

Flake exports:

- `lib.kernelPatches`: historical local-only integration-tree stack.
- `lib.portableKernelPatches`: portable stock-kernel stack.
- `lib.upstreamKernelPatches`: maintainer-tree delta only.
- `lib.portableLocalKernelPatches`: local stock-kernel-compatible patches.
- `lib.integrationDebugKernelPatches`: integration-tree-only debug patches.
- `lib.kernelPatchesForIntegrationTree`: descriptive alias for
  `lib.kernelPatches`.

The same names are exposed under `legacyPackages.${system}` for NixOS configs
that cannot conveniently read flake `lib` attributes.
