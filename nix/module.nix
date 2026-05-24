{
  lib,
  stdenv,
  kernel,
}:

let
  root = ../.;
  rootPrefix = "${toString root}/";
in
stdenv.mkDerivation {
  pname = "thunderbolt-ibverbs";
  version = "0.1.0";

  src = lib.cleanSourceWith {
    src = root;
    filter = path: type:
      let
        rel = lib.removePrefix rootPrefix (toString path);
      in
        rel == "Makefile"
        || rel == "kernel"
        || lib.hasPrefix "kernel/" rel
        || rel == "proto"
        || lib.hasPrefix "proto/" rel;
  };

  nativeBuildInputs = kernel.moduleBuildDependencies;

  buildPhase = ''
    runHook preBuild
    make \
      KVER=${kernel.modDirVersion} \
      KDIR=${kernel.dev}/lib/modules/${kernel.modDirVersion}/build \
      modules
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -D -m 0644 kernel/thunderbolt_ibverbs.ko \
      "$out/lib/modules/${kernel.modDirVersion}/extra/thunderbolt_ibverbs.ko"
    runHook postInstall
  '';

  dontPatchELF = true;
  dontStrip = true;
  dontFixup = true;

  meta = {
    description = "Thunderbolt/USB4 host-to-host RDMA verbs kernel module";
    license = lib.licenses.gpl2Only;
    platforms = lib.platforms.linux;
  };
}
