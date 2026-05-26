{ lib
, bash
, stdenvNoCC
}:

let
  name = "apple-rdma-sdk-26.5";
in
derivation {
  inherit name;
  system = stdenvNoCC.hostPlatform.system;
  builder = "${bash}/bin/bash";
  args = [
    "-c"
    ''
    cat >&2 <<'EOF'
apple-rdma-sdk is an opaque fixed-output dependency.

The Darwin bench/perftest packages need Xcode 26.5's Apple RDMA
libibverbs headers, but those headers are not yet available from the
packaged nixpkgs macOS SDK. Seed this exact fixed-output path into the
store/cache instead of letting Hydra build it.

Expected output contents:
  $out/include/infiniband/verbs.h

Expected recursive hash:
  sha256-ULtTIGilQ3RFG3snbP2WLO39EOruJ9/LxNvspy26gr0=

Temporary seed recipe on a Mac with Xcode 26.5:
  tmp=$(mktemp -d)
  mkdir -p "$tmp/include"
  cp -R /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX26.5.sdk/usr/include/infiniband "$tmp/include/"
  nix hash path "$tmp"
  nix store add-path --mode nar --hash-algo sha256 --name apple-rdma-sdk-26.5 "$tmp"

TODO: replace this opaque fixed-output header bundle once macOS/Xcode 26.5
SDK packaging exposes usr/include/infiniband.
EOF
    exit 1
    ''
  ];
  outputHashMode = "recursive";
  outputHashAlgo = "sha256";
  outputHash = "sha256-ULtTIGilQ3RFG3snbP2WLO39EOruJ9/LxNvspy26gr0=";

  preferLocalBuild = false;
  allowSubstitutes = true;
} // {
  meta = with lib; {
    description = "Opaque fixed-output Apple RDMA libibverbs header bundle";
    platforms = platforms.darwin;
  };
}
