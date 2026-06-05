# Kernel Patches

This directory carries two ready-to-use patch stacks:

- `portable.nix`: stock-kernel stack. It applies the Thunderbolt maintainer-tree
  commits from `upstream-thunderbolt-next.nix`, then the project-local patches
  from `local-portable.nix`.
- `local.nix`: integration-tree stack for the flake's `linux-thunderbolt`
  package, which builds from `westeri/thunderbolt.git`.

The component lists are:

- `upstream-thunderbolt-next.nix`: Thunderbolt maintainer-tree commits that are
  not guaranteed to be present in a stock kernel.
- `local-portable.nix`: project-local patches that apply after the upstream
  layer on a stock kernel.
- `local-integration-debug.nix`: debug patches that are only carried for the
  `westeri/thunderbolt.git` integration-tree kernel.
- `default.nix`: imports `portable.nix`.

Use `portable.nix` or the `kernelPatches` flake export to patch a normal kernel,
for example `pkgs.linuxPackages_latest`. `portable.nix` excludes
`local-integration-debug.nix` because that patch does not apply cleanly to the
stock kernel after the upstream layer.

The flake's `linux-thunderbolt` package uses `westeri/thunderbolt.git` as its
kernel source, so it applies `local.nix`. Applying
`upstream-thunderbolt-next.nix` there would apply the same maintainer commits
twice.

Flake exports:

- `lib.kernelPatches`: portable stock-kernel stack.
- `lib.integrationKernelPatches`: local patches for the integration-tree kernel.
- `lib.upstreamKernelPatches`: maintainer-tree delta only.
- `lib.portableLocalKernelPatches`: local stock-kernel-compatible patches.

The same names are exposed under `legacyPackages.${system}` for NixOS configs
that cannot conveniently read flake `lib` attributes.
