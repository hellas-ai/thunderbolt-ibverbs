{
  description = "Thunderbolt/USB4 host-to-host RDMA verbs kernel module";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    linux-src = {
      # Thunderbolt maintainer tree carrying the USB4STREAM/XDomain base series.
      url = "git+https://git.kernel.org/pub/scm/linux/kernel/git/westeri/thunderbolt.git?ref=refs/heads/next&shallow=1";
      flake = false;
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      linux-src,
    }:
    let
      lib = nixpkgs.lib;
      upstreamThunderboltKernelPatches = import ./kernel-workflow/patches/upstream-thunderbolt-next.nix;
      portableLocalThunderboltKernelPatches = import ./kernel-workflow/patches/local-portable.nix;
      portableThunderboltKernelPatches = import ./kernel-workflow/patches/portable.nix;
      integrationDebugThunderboltKernelPatches = import ./kernel-workflow/patches/local-integration-debug.nix;
      integrationThunderboltKernelPatches = import ./kernel-workflow/patches/local.nix;
      kernelPatchSets = {
        kernelPatches = portableThunderboltKernelPatches;
        integrationKernelPatches = integrationThunderboltKernelPatches;
        upstreamKernelPatches = upstreamThunderboltKernelPatches;
        portableLocalKernelPatches = portableLocalThunderboltKernelPatches;
      };
      linuxSystems = [ "x86_64-linux" ];
      darwinSystems = [ "aarch64-darwin" ];
      systems = linuxSystems ++ darwinSystems;
      forAllSystems = f: lib.genAttrs systems (system: f (import nixpkgs { inherit system; }));
      forLinuxSystems = f: lib.genAttrs linuxSystems (system: f (import nixpkgs { inherit system; }));
      forDarwinSystems = f: lib.genAttrs darwinSystems (system: f (import nixpkgs { inherit system; }));
      linuxSrcMakefile = builtins.readFile "${linux-src}/Makefile";
      linuxSrcMakeVars =
        let
          lines = lib.splitString "\n" linuxSrcMakefile;
          readVar =
            name:
            let
              matches = lib.filter (match: match != null) (
                map (line: builtins.match "${name}[[:space:]]*=[[:space:]]*(.*)" line) lines
              );
            in
            if matches == [ ] then
              throw "could not read ${name} from Linux Makefile"
            else
              lib.head (lib.head matches);
        in
        {
          version = readVar "VERSION";
          patchlevel = readVar "PATCHLEVEL";
          sublevel = readVar "SUBLEVEL";
          extraversion = readVar "EXTRAVERSION";
        };
      linuxSrcVersion = "${linuxSrcMakeVars.version}.${linuxSrcMakeVars.patchlevel}.${linuxSrcMakeVars.sublevel}${linuxSrcMakeVars.extraversion}";
      mkThunderboltKernel =
        pkgs:
        let
          testingKernel = pkgs.linuxPackages_testing.kernel;
          kernelPatches = (testingKernel.passthru.kernelPatches or [ ]) ++ integrationThunderboltKernelPatches;
        in
        (testingKernel.override {
          argsOverride = {
            pname = "linux-thunderbolt";
            version = linuxSrcVersion;
            modDirVersion = linuxSrcVersion;
            src = linux-src;
            inherit kernelPatches;
          };
        }).overrideAttrs (old: {
          meta = (old.meta or { }) // {
            maintainers = with pkgs.lib.maintainers; [ georgewhewell ];
          };
        });
      mkThunderboltLinuxPackages = pkgs: pkgs.linuxPackagesFor (mkThunderboltKernel pkgs);
      rdmaCoreUsb4Patches = [
        ./packaging/rdma-core-patches/0001-providers-usb4_rdma-add-USB4-soft-RDMA-provider.patch
        ./packaging/rdma-core-patches/0002-CMakeLists.txt-build-the-usb4_rdma-provider.patch
        ./packaging/rdma-core-patches/0003-libibverbs-verbs.h-declare-verbs_provider_usb4_rdma.patch
      ];
      mkRdmaCoreUsb4 =
        pkgs:
        pkgs.rdma-core.overrideAttrs (old: {
          pname = "rdma-core-usb4";
          patches = (old.patches or [ ]) ++ rdmaCoreUsb4Patches;
          meta = (old.meta or { }) // {
            maintainers = with pkgs.lib.maintainers; [ georgewhewell ];
          };
        });
      mkScriptSyntaxCheck =
        pkgs:
        pkgs.stdenv.mkDerivation {
          pname = "thunderbolt-ibverbs-script-syntax";
          version = "0.1.0";
          src = ./.;

          nativeBuildInputs = [ pkgs.bash ];

          dontConfigure = true;

          buildPhase = ''
            runHook preBuild
            bash -n \
              packaging/regen-rdma-core-patches.sh \
              packaging/test-rdma-patches.sh \
              kernel-workflow/regen-upstream-thunderbolt-patches.sh \
              tools/ci/distro-build.sh \
              tools/ci/distro-install.sh \
              tools/ci/distro-package-rdma.sh \
              tools/ci/distro-package.sh \
              tools/ci/vm-guest-smoke.sh \
              tools/ci/vm-smoke.sh
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            mkdir -p "$out"
            runHook postInstall
          '';

          meta = {
            maintainers = with pkgs.lib.maintainers; [ georgewhewell ];
          };
        };
      mkPortableKernelPatchCheck =
        pkgs:
        let
          portablePatchBundle = pkgs.runCommand "thunderbolt-portable-kernel-patches" { } ''
            mkdir -p "$out"
            ${lib.concatMapStringsSep "\n" (patch: ''
              cp ${patch.patch} "$out/${baseNameOf (toString patch.patch)}"
            '') portableThunderboltKernelPatches}
            printf '%s\n' ${
              lib.escapeShellArgs (map (patch: baseNameOf (toString patch.patch)) portableThunderboltKernelPatches)
            } > "$out/series"
          '';
        in
        pkgs.stdenv.mkDerivation {
          pname = "thunderbolt-portable-kernel-patches-apply-check";
          version = "0.1.0";
          src = pkgs.linuxPackages_latest.kernel.src;

          nativeBuildInputs = [ pkgs.git ];

          patchPhase = ''
            runHook prePatch
            while IFS= read -r patch; do
              [ -n "$patch" ] || continue
              patch="${portablePatchBundle}/$patch"
              echo "checking $patch"
              git apply --check "$patch"
              git apply "$patch"
            done < ${portablePatchBundle}/series
            runHook postPatch
          '';

          dontConfigure = true;
          dontBuild = true;

          installPhase = ''
            runHook preInstall
            mkdir -p "$out"
            runHook postInstall
          '';

          meta = {
            maintainers = with pkgs.lib.maintainers; [ georgewhewell ];
          };
        };
      mkProtoSmoke =
        pkgs:
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
            $CC -std=c11 -Wall -Wextra -Werror -I. \
              tools/ci/reliability-smoke.c \
              proto/reliability.c \
              -o tbv-reliability-smoke
            ./tbv-reliability-smoke
            $CC -std=c11 -Wall -Wextra -Werror -I. \
              tools/ci/identity-smoke.c \
              proto/identity.c \
              -o tbv-identity-smoke
            ./tbv-identity-smoke
            $CC -std=c11 -Wall -Wextra -Werror -I. \
              tools/ci/config-smoke.c \
              proto/config.c \
              proto/identity.c \
              -o tbv-config-smoke
            ./tbv-config-smoke
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            mkdir -p "$out"
            runHook postInstall
          '';

          meta = {
            maintainers = with pkgs.lib.maintainers; [ georgewhewell ];
          };
        };
      mkNixosVmSmoke =
        pkgs:
        let
          module = pkgs.linuxPackages.callPackage ./nix/module.nix { };
          rdmaCoreUsb4 = mkRdmaCoreUsb4 pkgs;
        in
        pkgs.testers.runNixOSTest {
          name = "thunderbolt-ibverbs-vm-smoke";
          meta = {
            maintainers = with pkgs.lib.maintainers; [ georgewhewell ];
          };

          nodes.machine =
            { pkgs, ... }:
            {
              boot.extraModulePackages = [ module ];
              environment.systemPackages = [
                pkgs.kmod
                rdmaCoreUsb4
              ];
            };

          testScript = ''
            machine.start()
            machine.wait_for_unit("multi-user.target")
            machine.succeed("modprobe thunderbolt_ibverbs profile=linux_perf bind_services=0 register_verbs=1")
            machine.succeed("grep '^thunderbolt_ibverbs ' /proc/modules")
            machine.succeed("modinfo thunderbolt_ibverbs")
            machine.succeed("rdma link show")
            machine.succeed("ibv_devices")
            machine.succeed("rmmod thunderbolt_ibverbs")
          '';
        };
      mkVerbsSmokeBuild =
        pkgs:
        pkgs.stdenv.mkDerivation {
          pname = "thunderbolt-ibverbs-verbs-smoke-build";
          version = "0.1.0";
          src = ./.;

          nativeBuildInputs = [ pkgs.pkg-config ];
          buildInputs = [ (mkRdmaCoreUsb4 pkgs) ];

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

          meta = {
            maintainers = with pkgs.lib.maintainers; [ georgewhewell ];
          };
        };
    in
    {
      packages = forAllSystems (
        pkgs:
        let
          isLinux = pkgs.stdenv.hostPlatform.isLinux;
          isDarwin = pkgs.stdenv.hostPlatform.isDarwin;
          rdmaCoreUsb4 = mkRdmaCoreUsb4 pkgs;
          packageArgs =
            lib.optionalAttrs isLinux { rdma-core-usb4 = rdmaCoreUsb4; }
            // lib.optionalAttrs isDarwin { inherit (pkgs) apple-sdk_26; };
          perftest = pkgs.callPackage ./nix/perftest.nix packageArgs;
          benchTools = pkgs.callPackage ./nix/bench-tools.nix packageArgs;
          perftestBench =
            if isLinux then
              import ./lib/bench/perftest.nix {
                inherit lib pkgs perftest;
                # cross-system reference: the wrapper bakes the darwin
                # perftest store path so the runner can pick the right
                # binary per host without an explicit CLI override.
                perftestDarwin = self.packages.aarch64-darwin.perftest or null;
                rdma-core-usb4 = rdmaCoreUsb4;
                runnerSrc = ./userspace/bench/tbv_perftest_runner.py;
                benchConfig = import ./lib/bench { inherit lib; };
              }
            else
              null;
        in
        {
          perftest = perftest;
          bench-tools = benchTools;
        }
        // lib.optionalAttrs isLinux (
          let
            module = pkgs.linuxPackages.callPackage ./nix/module.nix { };
            thunderboltKernel = mkThunderboltKernel pkgs;
            thunderboltLinuxPackages = mkThunderboltLinuxPackages pkgs;
            moduleForThunderboltKernel = thunderboltLinuxPackages.callPackage ./nix/module.nix { };
          in
          {
            default = module;
            linux-thunderbolt = thunderboltKernel;
            linux-thunderbolt-dev = thunderboltKernel.dev;
            linux-thunderbolt-modules = thunderboltKernel.modules;
            rdma-core-usb4 = rdmaCoreUsb4;
            thunderbolt-ibverbs = module;
            thunderbolt-ibverbs-linux-thunderbolt = moduleForThunderboltKernel;
            tbv-perftest = perftestBench.runner;
          }
        )
      );

      apps = forLinuxSystems (
        pkgs:
        let
          pkgsAt = self.packages.${pkgs.stdenv.hostPlatform.system};
        in
        {
          tbv-perftest = {
            type = "app";
            program = lib.getExe pkgsAt.tbv-perftest;
            meta = {
              description = "Run the Thunderbolt/USB4 RDMA perftest benchmark matrix";
              maintainers = with pkgs.lib.maintainers; [ georgewhewell ];
            };
          };
        }
      );

      checks = forAllSystems (
        pkgs:
        let
          isLinux = pkgs.stdenv.hostPlatform.isLinux;
          pkgsAt = self.packages.${pkgs.stdenv.hostPlatform.system};
        in
        {
          perftest = pkgsAt.perftest;
          bench-tools = pkgsAt.bench-tools;
        }
        // lib.optionalAttrs isLinux {
          thunderbolt-ibverbs = pkgsAt.thunderbolt-ibverbs;
          portable-kernel-patches = mkPortableKernelPatchCheck pkgs;
          script-syntax = mkScriptSyntaxCheck pkgs;
          proto-smoke = mkProtoSmoke pkgs;
          rdma-core-usb4 = pkgsAt.rdma-core-usb4;
          verbs-smoke-build = mkVerbsSmokeBuild pkgs;
        }
      );

      hydraJobs = forAllSystems (
        pkgs:
        let
          isLinux = pkgs.stdenv.hostPlatform.isLinux;
          pkgsAt = self.packages.${pkgs.stdenv.hostPlatform.system};
        in
        {
          perftest = pkgsAt.perftest;
          bench-tools = pkgsAt.bench-tools;
          checks = self.checks.${pkgs.stdenv.hostPlatform.system};
        }
        // lib.optionalAttrs isLinux {
          thunderbolt-ibverbs = pkgsAt.thunderbolt-ibverbs;
          linux-thunderbolt = pkgsAt.linux-thunderbolt;
          linux-thunderbolt-dev = pkgsAt.linux-thunderbolt-dev;
          linux-thunderbolt-modules = pkgsAt.linux-thunderbolt-modules;
          rdma-core-usb4 = pkgsAt.rdma-core-usb4;
          thunderbolt-ibverbs-linux-thunderbolt = pkgsAt.thunderbolt-ibverbs-linux-thunderbolt;
          vm-smoke.nixos = mkNixosVmSmoke pkgs;
        }
      );

      devShells = forAllSystems (
        pkgs:
        let
          isLinux = pkgs.stdenv.hostPlatform.isLinux;
          pkgsAt = self.packages.${pkgs.stdenv.hostPlatform.system};
        in
        {
          default = pkgs.mkShell {
            name = "thunderbolt-ibverbs-dev";
            packages = [
              pkgs.python3
              pkgsAt.perftest
              pkgsAt.bench-tools
            ]
            ++ lib.optionals isLinux [
              pkgs.kmod
              pkgsAt.rdma-core-usb4
            ];
            meta = {
              maintainers = with pkgs.lib.maintainers; [ georgewhewell ];
            };
          };
        }
      );

      overlays.default =
        final: prev:
        let
          isLinux = prev.stdenv.hostPlatform.isLinux;
          isDarwin = prev.stdenv.hostPlatform.isDarwin;
          packageArgs = lib.optionalAttrs isLinux {
            rdma-core-usb4 = final.rdma-core-usb4;
          } // lib.optionalAttrs isDarwin {
            inherit (final) apple-sdk_26;
          };
        in
        {
          thunderbolt-ibverbs-bench-tools = final.callPackage ./nix/bench-tools.nix packageArgs;
          thunderbolt-ibverbs-perftest = final.callPackage ./nix/perftest.nix packageArgs;
        }
        // lib.optionalAttrs isLinux (
          let
            thunderboltKernel = mkThunderboltKernel final;
            thunderboltLinuxPackages = final.linuxPackagesFor thunderboltKernel;
          in
          {
            linux-thunderbolt = thunderboltKernel;
            linux-thunderbolt-dev = thunderboltKernel.dev;
            linux-thunderbolt-modules = thunderboltKernel.modules;
            linuxPackages_thunderbolt = thunderboltLinuxPackages;
            rdma-core-usb4 = mkRdmaCoreUsb4 prev;
            thunderbolt-ibverbs = final.linuxPackages.callPackage ./nix/module.nix { };
            thunderbolt-ibverbs-linux-thunderbolt = thunderboltLinuxPackages.callPackage ./nix/module.nix { };
          }
        );

      lib = kernelPatchSets;

      legacyPackages = forLinuxSystems (_pkgs: kernelPatchSets);

      nixosModules.default = import ./module.nix { inherit self; };
    };
}
