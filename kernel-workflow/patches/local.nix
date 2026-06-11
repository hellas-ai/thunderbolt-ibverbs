[
  {
    name = "usb4-dma-priority-weight-params";
    patch = ./0002-thunderbolt-tunnel-add-dma-priority-weight-params.patch;
  }
]
++ (import ./local-integration-debug.nix)
++ [
  {
    name = "usb4-nhi-clear-pending-before-unmask";
    patch = ./0004-thunderbolt-nhi-clear-pending-before-unmask.patch;
  }
  {
    name = "usb4-xdomain-log-unmatched-protocol-uuids";
    patch = ./0005-thunderbolt-xdomain-log-unmatched-protocol-uuids.patch;
  }
  {
    name = "usb4-xdomain-source-aware-protocol-handler";
    patch = ./0007-thunderbolt-xdomain-pass-source-to-protocol-handlers.patch;
  }
  {
    name = "usb4-xdomain-pin-protocol-handler-owner";
    patch = ./0008-thunderbolt-xdomain-pin-protocol-handler-owner.patch;
  }
  {
    name = "usb4-xdomain-property-identity-match";
    patch = ./0009-thunderbolt-xdomain-match-properties-by-identity.patch;
  }
  {
    name = "usb4-xdomain-drain-protocol-callbacks";
    patch = ./0010-thunderbolt-xdomain-drain-protocol-callbacks-on-unr.patch;
  }
]
