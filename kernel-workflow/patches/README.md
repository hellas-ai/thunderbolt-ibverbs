# Kernel Patches

This directory carries two ready-to-use patch stacks:

- `portable.nix`: stock Linux 7.2 stack. It applies the project-local patches
  that are not already included upstream.
- `local.nix`: integration-tree stack for the flake's `linux-thunderbolt`
  package, which builds from `westeri/thunderbolt.git`.

The component lists are:

- `upstream-thunderbolt-next.nix`: Thunderbolt maintainer-tree commits that are
  included in Linux 7.2, but need backporting to Linux 7.1.
- `portable-for-kernel.nix`: selects the stock-kernel stack by version. Linux
  7.1 gets the upstream backports and local patches; Linux 7.2 omits those
  backports and the already-upstream XDomain delayed-work lifetime fix.
- `local-portable.nix`: project-local patches that apply after the upstream
  layer on a stock kernel.
- `local-integration-debug.nix`: debug patches that are only carried for the
  `westeri/thunderbolt.git` integration-tree kernel.
- `default.nix`: imports `portable.nix`.

Use `kernelPatchesFor` with the target kernel's version to patch a normal
kernel, for example:

```nix
boot.kernelPatches = thunderbolt-ibverbs.lib.kernelPatchesFor
  config.boot.kernelPackages.kernel.version;
```

Direct imports can use `import ./portable-for-kernel.nix "7.1.2"`. The
`portable.nix` list and `kernelPatches` export default to Linux 7.2.
`portable.nix` excludes
`local-integration-debug.nix` because that patch does not apply cleanly to the
stock kernel after the upstream layer.

The flake's `linux-thunderbolt` package uses `westeri/thunderbolt.git` as its
kernel source, so it applies `local.nix`. Applying
`upstream-thunderbolt-next.nix` there would apply the same maintainer commits
twice.

Flake exports:

- `lib.kernelPatches`: portable Linux 7.2 stock-kernel stack.
- `lib.kernelPatchesFor`: version selector for a stock target kernel.
- `lib.integrationKernelPatches`: local patches for the integration-tree kernel.
- `lib.upstreamKernelPatches`: maintainer-tree delta only.
- `lib.portableLocalKernelPatches`: local stock-kernel-compatible patches.

The same names are exposed under `legacyPackages.${system}` for NixOS configs
that cannot conveniently read flake `lib` attributes.
