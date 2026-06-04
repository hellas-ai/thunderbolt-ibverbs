let
  debugNames = map (patch: patch.name) (import ./local-integration-debug.nix);
in
builtins.filter (patch: !(builtins.elem patch.name debugNames)) (import ./local.nix)
