{ lib
, stdenv
, pkg-config
, rdma-core-usb4
, python3
, source ? ../userspace/bench
}:

let
  # Standalone C programs. Each is built as $out/bin/<name> from <name>.c.
  cPrograms = [
    "mac_tb_rdma_probe"
    "rc_write_poll"
    "rc_write_verify"
    "u4_pingpong"
    "uc_oneway"
  ];
  # ibv_trace.c is an LD_PRELOAD tracer; built as $out/lib/libibv_trace.so.
  scripts = [
    "tbv_perftest_runner.py"
    "tbv_rdma_sweep.py"
  ];
in
stdenv.mkDerivation {
  pname = "thunderbolt-ibverbs-bench-tools";
  version = "0.1.0";

  src = lib.cleanSourceWith {
    src = source;
    filter = path: _type:
      let
        rel = baseNameOf (toString path);
      in
        lib.hasSuffix ".c" rel
        || lib.hasSuffix ".py" rel
        || lib.hasSuffix ".sh" rel;
  };

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ rdma-core-usb4 python3 ];

  dontConfigure = true;

  buildPhase = ''
    runHook preBuild
    for name in ${lib.concatStringsSep " " cPrograms}; do
      extra=""
      case "$name" in
        uc_oneway) extra="-ldl" ;;
      esac
      $CC -O2 -Wall -Wextra -std=gnu11 \
        -I${rdma-core-usb4.dev}/include \
        "$name.c" \
        -L${rdma-core-usb4}/lib -libverbs \
        $extra \
        -Wl,-rpath,${rdma-core-usb4}/lib \
        -o "$name"
    done
    $CC -O2 -Wall -Wextra -fPIC -shared \
      -I${rdma-core-usb4.dev}/include \
      ibv_trace.c \
      -ldl -lpthread \
      -L${rdma-core-usb4}/lib -libverbs \
      -Wl,-rpath,${rdma-core-usb4}/lib \
      -o libibv_trace.so
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin $out/lib
    install -m 0755 ${lib.concatStringsSep " " cPrograms} $out/bin/
    install -m 0755 ${lib.concatStringsSep " " scripts} $out/bin/
    install -m 0644 libibv_trace.so $out/lib/
    runHook postInstall
  '';

  postFixup = ''
    patchShebangs $out/bin
  '';

  meta = with lib; {
    description = "Thunderbolt/USB4 RDMA bench programs (libibverbs C tools + perftest/vllm helper scripts)";
    license = licenses.mit;
    platforms = platforms.linux;
  };
}
