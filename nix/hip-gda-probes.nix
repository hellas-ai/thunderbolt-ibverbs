{ lib
, stdenv
, rocmPackages
, rdma-core-usb4
, source ? ../userspace
, gfxTargets ? [ "gfx1151" ]
}:

let
  hipPrograms = [
    "hip_coherence_probe"
    "hip_dv_kernel_cqe_probe"
    "hip_rdma_write_visibility_probe"
    "hip_reg_mr_probe"
    "hsa_fine_grain_pool_probe"
    "rc_write_gpu_poll"
  ];

  gfxFlags = lib.concatMapStringsSep " " (gfx: "--offload-arch=${gfx}") gfxTargets;
in
stdenv.mkDerivation {
  pname = "tbv-hip-gda-probes";
  version = "0.1.0";

  src = lib.cleanSourceWith {
    src = source;
    filter = path: type:
      let
        name = baseNameOf (toString path);
      in
        type == "directory"
        || lib.hasSuffix ".cpp" name
        || lib.hasSuffix ".c" name
        || lib.hasSuffix ".h" name;
  };

  nativeBuildInputs = [
    rocmPackages.hipcc
    rocmPackages.rocmClangStdenv.cc
  ];

  buildInputs = [
    rdma-core-usb4
    rocmPackages.clr
    rocmPackages.rocm-runtime
  ];

  dontConfigure = true;

  env = {
    HIP_PLATFORM = "amd";
    HIP_PATH = rocmPackages.clr;
    HIP_CLANG_PATH = "${rocmPackages.rocmClangStdenv.cc}/bin";
  };

  buildPhase = ''
    runHook preBuild

    common_flags=(
      -O2
      -std=c++17
      -Wall
      -Wextra
      ${gfxFlags}
      -I${rdma-core-usb4.dev}/include
      -Iusb4_rdma
      -I${rocmPackages.clr}/include
      -I${rocmPackages.rocm-runtime}/include
      -L${rdma-core-usb4}/lib
      -L${rocmPackages.clr}/lib
      -L${rocmPackages.rocm-runtime}/lib
      -libverbs
      -lamdhip64
      -lhsa-runtime64
      -Wl,-rpath,${rdma-core-usb4}/lib
      -Wl,-rpath,${rocmPackages.clr}/lib
      -Wl,-rpath,${rocmPackages.rocm-runtime}/lib
    )

    for name in ${lib.concatStringsSep " " hipPrograms}; do
      hipcc "bench/$name.cpp" "''${common_flags[@]}" -o "$name"
    done

    $CC -O2 -Wall -Wextra -fPIC -shared \
      -D__HIP_PLATFORM_AMD__ \
      -I${rocmPackages.clr}/include \
      bench/hip_host_coherent_shim.c \
      -ldl \
      -o libhip_host_coherent_shim.so

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    mkdir -p "$out/bin" "$out/lib"
    install -m 0755 ${lib.concatStringsSep " " hipPrograms} "$out/bin/"
    install -m 0644 libhip_host_coherent_shim.so "$out/lib/"
    runHook postInstall
  '';

  meta = with lib; {
    description = "ROCm HIP probes for Thunderbolt/USB4 GDA queue and payload correctness";
    license = licenses.mit;
    maintainers = with maintainers; [ georgewhewell ];
    platforms = platforms.linux;
  };
}
