{
  description = "Thunderbolt/USB4 host-to-host RDMA verbs kernel module";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      lib = nixpkgs.lib;
      optionalKernelPatches = [
        {
          name = "thunderbolt-nhi-ring-throttling-helper";
          patch = ./patches/linux/0001-thunderbolt-nhi-add-per-ring-interrupt-throttling-helper.patch;
        }
      ];
      systems = [
        "x86_64-linux"
      ];
      forAllSystems = f:
        lib.genAttrs systems (system:
          f (import nixpkgs { inherit system; }));
      mkProtoSmoke = pkgs:
        pkgs.stdenv.mkDerivation {
          pname = "thunderbolt-ibverbs-proto-smoke";
          version = "0.1.0";
          src = ./.;

          dontConfigure = true;

          buildPhase = ''
            runHook preBuild
            $CC -std=c11 -Wall -Wextra -Werror -I. \
              tools/ci/proto-smoke.c \
              -o tbv-proto-smoke
            ./tbv-proto-smoke
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            mkdir -p "$out"
            runHook postInstall
          '';
        };
      mkVerbsSmokeBuild = pkgs:
        pkgs.stdenv.mkDerivation {
          pname = "thunderbolt-ibverbs-verbs-smoke-build";
          version = "0.1.0";
          src = ./.;

          nativeBuildInputs = [ pkgs.pkg-config ];
          buildInputs = [ pkgs.rdma-core ];

          dontConfigure = true;

          buildPhase = ''
            runHook preBuild
            $CC -std=c11 -Wall -Wextra -Werror \
              tools/ci/verbs-smoke.c \
              $(pkg-config --cflags --libs libibverbs) \
              -o tbv-verbs-smoke
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            mkdir -p "$out"
            runHook postInstall
          '';
        };
    in
    {
      packages = forAllSystems (pkgs:
        let
          module = pkgs.linuxPackages.callPackage ./nix/module.nix { };
        in
        {
          default = module;
          thunderbolt-ibverbs = module;
        });

      checks = forAllSystems (pkgs: {
        thunderbolt-ibverbs =
          self.packages.${pkgs.stdenv.hostPlatform.system}.thunderbolt-ibverbs;
        proto-smoke = mkProtoSmoke pkgs;
        verbs-smoke-build = mkVerbsSmokeBuild pkgs;
      });

      hydraJobs.x86_64-linux = {
        thunderbolt-ibverbs = self.packages.x86_64-linux.thunderbolt-ibverbs;
        checks = self.checks.x86_64-linux;
      };

      overlays.default = final: prev: {
        thunderbolt-ibverbs =
          final.linuxPackages.callPackage ./nix/module.nix { };
      };

      lib.kernelPatches = optionalKernelPatches;

      legacyPackages = forAllSystems (_pkgs: {
        kernelPatches = optionalKernelPatches;
      });

      nixosModules.default = { config, lib, ... }:
        let
          cfg = config.hardware.thunderbolt-ibverbs;
          module =
            config.boot.kernelPackages.callPackage ./nix/module.nix { };
        in
        {
          options.hardware.thunderbolt-ibverbs.enable =
            lib.mkEnableOption "the thunderbolt_ibverbs kernel module";

          config = lib.mkIf cfg.enable {
            boot.extraModulePackages = [ module ];
            boot.kernelModules = [ "thunderbolt_ibverbs" ];
          };
        };
    };
}
