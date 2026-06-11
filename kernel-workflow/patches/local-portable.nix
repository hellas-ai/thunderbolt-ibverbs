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
]
