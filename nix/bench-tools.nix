{ lib
, stdenv
, pkg-config
, apple-sdk_26 ? null
, rdma-core-usb4 ? null
, python3
, source ? ../userspace
, appleCompat ? ./apple-compat
}:

let
  isDarwin = stdenv.hostPlatform.isDarwin;

  # Programs portable to Apple's RDMA host SDK (linked via dynamic_lookup):
  # plain libibverbs verbs surface, no Linux-specific kernel uapi or
  # LD_PRELOAD-style entrypoints.
  darwinPrograms = [
    "mac_tb_rdma_probe"
    "rc_send_churn"
    "rc_write_poll"
    "rc_write_verify"
    "u4_pingpong"
    "uc_oneway"
    "uc_write_verify"
  ];

  # Full Linux set. ibv_trace is an LD_PRELOAD tracer built as .so.
  linuxPrograms = darwinPrograms ++ [
    "rc_qpn_churn"
    "rdma_gid_probe"
    "tbv_dv_caps_probe"
    "tbv_dv_send_probe"
  ];

  scripts = [
    "tbv_app_gate.sh"
    "tbv_app_gate_summarize.sh"
    "tbv_perftest_runner.py"
    "tbv_pytorch_smoke.py"
    "tbv_rdma_sweep.py"
    "tbv_uc_stress.py"
  ];

  cPrograms = if isDarwin then darwinPrograms else linuxPrograms;
in
assert lib.assertMsg (!isDarwin || apple-sdk_26 != null)
  "bench-tools Darwin build requires apple-sdk_26";
stdenv.mkDerivation {
  pname =
    if isDarwin
    then "thunderbolt-ibverbs-bench-tools-apple-rdma"
    else "thunderbolt-ibverbs-bench-tools";
  version = "0.1.0";

  src = lib.cleanSourceWith {
    src = source;
    filter = path: type:
      let
        rel = baseNameOf (toString path);
      in
        type == "directory"
        || lib.hasSuffix ".c" rel
        || lib.hasSuffix ".h" rel
        || lib.hasSuffix ".py" rel
        || lib.hasSuffix ".sh" rel;
  };

  nativeBuildInputs = lib.optionals (!isDarwin) [ pkg-config ];
  buildInputs = lib.optionals (!isDarwin) [ rdma-core-usb4 ]
    ++ lib.optionals isDarwin [ apple-sdk_26 ]
    ++ [ python3 ];

  dontConfigure = true;

  buildPhase =
    if isDarwin then ''
      runHook preBuild
	      for name in ${lib.concatStringsSep " " cPrograms}; do
	        $CC -O2 -Wall -Wextra -std=gnu11 \
	          -I${appleCompat} \
	          "bench/$name.c" \
	          -lrdma \
	          -o "$name"
      done
      runHook postBuild
    '' else ''
      runHook preBuild
      for name in ${lib.concatStringsSep " " cPrograms}; do
        extra=""
        case "$name" in
          uc_oneway) extra="-ldl" ;;
        esac
	        $CC -O2 -Wall -Wextra -std=gnu11 \
	          -I${rdma-core-usb4.dev}/include \
	          -Iusb4_rdma \
	          "bench/$name.c" \
	          -L${rdma-core-usb4}/lib -libverbs \
	          $extra \
          -Wl,-rpath,${rdma-core-usb4}/lib \
          -o "$name"
      done
	      $CC -O2 -Wall -Wextra -fPIC -shared \
	        -I${rdma-core-usb4.dev}/include \
	        bench/ibv_trace.c \
	        -ldl -lpthread \
        -L${rdma-core-usb4}/lib -libverbs \
        -Wl,-rpath,${rdma-core-usb4}/lib \
        -o libibv_trace.so
      runHook postBuild
    '';

  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin
    install -m 0755 ${lib.concatStringsSep " " cPrograms} $out/bin/
    install -m 0755 ${lib.concatMapStringsSep " " (script: "bench/${script}") scripts} $out/bin/
    ${lib.optionalString (!isDarwin) ''
      mkdir -p $out/lib
      install -m 0644 libibv_trace.so $out/lib/
    ''}
    runHook postInstall
  '';

  postFixup = ''
    patchShebangs $out/bin
  '';

  meta = with lib; {
    description = "Thunderbolt/USB4 RDMA bench programs (libibverbs C tools + perftest helper scripts)";
    license = licenses.mit;
    maintainers = with maintainers; [ georgewhewell ];
    platforms = if isDarwin then platforms.darwin else platforms.linux;
  };
}
