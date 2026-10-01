kernelVersion:
let
  # Linux 7.2 contains the maintainer-tree series and the XDomain delayed-work
  # lifetime fix. Reapplying those backports fails before the local patches.
  hasUpstreamSeries = builtins.compareVersions kernelVersion "7.2" >= 0;
  upstream = if hasUpstreamSeries then [ ] else import ./upstream-thunderbolt-next.nix;
  local = builtins.filter (
    patch: !hasUpstreamSeries || patch.name != "usb4-xdomain-delayed-work-uaf"
  ) (import ./local-portable.nix);
in
upstream ++ local
