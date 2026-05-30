{ lib
, stdenv
, pkg-config
, rdma-core-usb4
, source ? ../.
}:

# Probes that exercise the USB4 RDMA Direct Verbs (DV / GDA) ABI.
#
# Each probe is a small standalone tool that talks to the kernel module via
# the raw RDMA_VERBS_IOCTL ABI plus the shared ABI header at
# userspace/usb4_rdma/usb4_rdma_dv.h, without depending on rdma-core's
# private execute_ioctl() helper. This keeps the probes buildable as
# ordinary userspace binaries.

let
  probes = [
    "tbv_dv_caps_probe"
  ];
in
stdenv.mkDerivation {
  pname = "thunderbolt-ibverbs-dv-probes";
  version = "0.1.0";

  src = lib.cleanSourceWith {
    src = source;
    filter = path: type:
      let
        rel = lib.removePrefix (toString source + "/") (toString path);
      in
        type == "directory"
        || rel == "userspace/usb4_rdma/usb4_rdma_dv.h"
        || (lib.hasPrefix "userspace/bench/tbv_dv_" rel
            && lib.hasSuffix ".c" rel);
  };

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ rdma-core-usb4 ];

  dontConfigure = true;

  buildPhase = ''
    runHook preBuild
    for name in ${lib.concatStringsSep " " probes}; do
      $CC -O2 -Wall -Wextra -std=gnu11 \
        -I${rdma-core-usb4.dev}/include \
        -Iuserspace/usb4_rdma \
        "userspace/bench/$name.c" \
        -L${rdma-core-usb4}/lib -libverbs \
        -Wl,-rpath,${rdma-core-usb4}/lib \
        -o "$(echo "$name" | tr '_' '-')"
    done
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin
    for name in ${lib.concatStringsSep " " probes}; do
      install -m 0755 "$(echo "$name" | tr '_' '-')" "$out/bin/"
    done
    runHook postInstall
  '';

  meta = with lib; {
    description = "Thunderbolt/USB4 RDMA Direct Verbs (GDA) probes";
    license = with licenses; [ gpl2Only bsd3 ];
    platforms = platforms.linux;
    mainProgram = "tbv-dv-caps-probe";
  };
}
