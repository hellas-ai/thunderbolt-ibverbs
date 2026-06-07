let
  debugNames = map (patch: patch.name) (import ./local-integration-debug.nix);
in
builtins.filter (patch: !(builtins.elem patch.name debugNames)) (import ./local.nix)
++ [
  {
    name = "usb4-xdomain-property-fixes";
    patch = ./0122-thunderbolt-xdomain-property-fixes.patch;
  }
  {
    name = "usb4-xdomain-delayed-work-uaf";
    patch = ./0123-thunderbolt-Prevent-XDomain-delayed-work-use-after.patch;
  }
  {
    name = "usb4-xdomain-lane-bonding-module-param";
    patch = ./0009-thunderbolt-xdomain-lane-bonding-module-param.patch;
  }
  {
    name = "usb4-xdomain-route-trace";
    patch = ./0010-thunderbolt-trace-XDomain-route-matching.patch;
  }
  {
    name = "usb4-xdomain-bridge-pad-mode";
    patch = ./0011-thunderbolt-xdomain-bridge-pad-mode.patch;
  }
  {
    name = "usb4-xdomain-bridge-sideband-mode";
    patch = ./0013-thunderbolt-xdomain-add-bridge-sideband-mode.patch;
  }
  {
    name = "usb4-xdomain-final-short-properties-chunk";
    patch = ./0014-thunderbolt-accept-final-short-xdomain-properties-chunks.patch;
  }
]
