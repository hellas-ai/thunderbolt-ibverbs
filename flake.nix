{
  description = "Thunderbolt/USB4 host-to-host RDMA verbs kernel module";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    {
      self,
      nixpkgs,
    }:
    let
      lib = nixpkgs.lib;
      portableLocalThunderboltKernelPatches = import ./kernel-workflow/patches/local-portable.nix;
      portableThunderboltKernelPatches = import ./kernel-workflow/patches/portable.nix;
      portableThunderboltKernelPatchesFor = import ./kernel-workflow/patches/portable-for-kernel.nix;
      integrationThunderboltKernelPatches = import ./kernel-workflow/patches/local.nix;
      kernelPatchSets = {
        kernelPatches = portableThunderboltKernelPatches;
        kernelPatchesFor = portableThunderboltKernelPatchesFor;
        portableKernelPatches = portableThunderboltKernelPatches;
        integrationKernelPatches = integrationThunderboltKernelPatches;
        # Compatibility export: Linux 7.2 already includes the upstream series.
        upstreamKernelPatches = [ ];
        portableLocalKernelPatches = portableLocalThunderboltKernelPatches;
      };
      linuxSystems = [ "x86_64-linux" ];
      darwinSystems = [ "aarch64-darwin" ];
      systems = linuxSystems ++ darwinSystems;
      forAllSystems = f: lib.genAttrs systems (system: f (import nixpkgs { inherit system; }));
      forLinuxSystems = f: lib.genAttrs linuxSystems (system: f (import nixpkgs { inherit system; }));
      forDarwinSystems = f: lib.genAttrs darwinSystems (system: f (import nixpkgs { inherit system; }));
      mkThunderboltKernel =
        pkgs:
        let
          stockKernel = pkgs.linuxPackages_7_2.kernel;
        in
        (stockKernel.override {
          pname = "linux-thunderbolt";
          kernelPatches = (stockKernel.kernelPatches or [ ]) ++ integrationThunderboltKernelPatches;
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
          version = "0.3.4";
          src = ./.;

          nativeBuildInputs = [
            pkgs.bash
            pkgs.python3
          ];

          dontConfigure = true;

          buildPhase = ''
            runHook preBuild
            bash -n \
              packaging/regen-rdma-core-patches.sh \
              packaging/test-rdma-patches.sh \
              tools/tbv-target-module.sh \
              tools/ci/distro-build.sh \
              tools/ci/distro-install.sh \
              tools/ci/distro-package-rdma.sh \
              tools/ci/distro-package.sh \
              tools/ci/vm-guest-smoke.sh \
              tools/ci/vm-smoke.sh \
              userspace/bench/tbv_vllm_smoke.sh
            python -m py_compile \
              userspace/bench/tbv_perftest_runner.py \
              userspace/bench/tbv_rdma_sweep.py \
              userspace/bench/tbv_uc_stress.py
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
          kernel = pkgs.linuxPackages_7_2.kernel;
          kernelPatches = portableThunderboltKernelPatchesFor kernel.version;
          portablePatchBundle = pkgs.runCommand "thunderbolt-portable-kernel-patches" { } ''
            mkdir -p "$out"
            ${lib.concatMapStringsSep "\n" (patch: ''
              cp ${patch.patch} "$out/${baseNameOf (toString patch.patch)}"
            '') kernelPatches}
            printf '%s\n' ${
              lib.escapeShellArgs (map (patch: baseNameOf (toString patch.patch)) kernelPatches)
            } > "$out/series"
          '';
        in
        pkgs.stdenv.mkDerivation {
          pname = "thunderbolt-portable-kernel-patches-apply-check";
          version = "0.3.4";
          src = kernel.src;

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
          version = "0.3.4";
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
        { kernelPackages, expectedNativeControl }:
        let
          module = kernelPackages.callPackage ./nix/module.nix { };
          rdmaCoreUsb4 = mkRdmaCoreUsb4 pkgs;
        in
        pkgs.testers.runNixOSTest {
          name = "thunderbolt-ibverbs-vm-smoke-${expectedNativeControl}";
          meta = {
            maintainers = with pkgs.lib.maintainers; [ georgewhewell ];
          };

          nodes.machine =
            { pkgs, ... }:
            {
              boot.kernelPackages = kernelPackages;
              boot.extraModulePackages = [ module ];
              environment.systemPackages = [
                pkgs.kmod
                rdmaCoreUsb4
              ];
            };

          testScript = ''
            machine.start()
            machine.wait_for_unit("multi-user.target")
            machine.succeed("test $(uname -r) = '${kernelPackages.kernel.modDirVersion}'")
            machine.succeed("modprobe thunderbolt_ibverbs profile=linux_perf bind_services=1 register_verbs=1")
            machine.succeed("grep '^native_control: ${expectedNativeControl}$' /sys/kernel/debug/thunderbolt_ibverbs/summary")
            machine.succeed("grep '^thunderbolt_ibverbs ' /proc/modules")
            machine.succeed("modinfo thunderbolt_ibverbs")
            machine.succeed("modinfo -F depends thunderbolt_ibverbs | tr ',' '\\n' | grep -qx ib_core")
            machine.succeed("modinfo -F depends thunderbolt_ibverbs | tr ',' '\\n' | grep -qx thunderbolt")
            machine.succeed("modinfo -F depends thunderbolt_ibverbs | tr ',' '\\n' | grep -qx configfs")
            machine.succeed("modinfo -F softdep thunderbolt_ibverbs | grep -q 'pre:.*ib_uverbs'")
            machine.succeed("mkdir -p /sys/kernel/config")
            machine.succeed("mountpoint -q /sys/kernel/config || mount -t configfs configfs /sys/kernel/config")
            machine.succeed("test -d /sys/kernel/config/thunderbolt_ibverbs")
            machine.succeed("trace_dir=/sys/kernel/tracing; test -d $trace_dir/events || trace_dir=/sys/kernel/debug/tracing; test -f $trace_dir/events/thunderbolt_ibverbs/tbv_cfgfs_link_op/format")
            machine.succeed("trace_dir=/sys/kernel/tracing; test -d $trace_dir/events || trace_dir=/sys/kernel/debug/tracing; test -f $trace_dir/events/thunderbolt_ibverbs/tbv_active_link/format")
            machine.fail("mkdir /sys/kernel/config/thunderbolt_ibverbs/vm-link")
            machine.succeed("mkdir /sys/kernel/config/thunderbolt_ibverbs/usb4_rdma0")
            machine.succeed("echo native > /sys/kernel/config/thunderbolt_ibverbs/usb4_rdma0/backend")
            machine.succeed("echo 10.0.4.2 > /sys/kernel/config/thunderbolt_ibverbs/usb4_rdma0/local_src_ipv4")
            machine.succeed("echo 10.0.5.2 > /sys/kernel/config/thunderbolt_ibverbs/usb4_rdma0/peer_ipv4")
            machine.succeed("echo '4 1 1 roce-v2 10.0.4.2' > /sys/kernel/config/thunderbolt_ibverbs/usb4_rdma0/app_gids")
            machine.succeed("echo 1 > /sys/kernel/config/thunderbolt_ibverbs/usb4_rdma0/seal")
            machine.succeed("grep '^sealed$' /sys/kernel/config/thunderbolt_ibverbs/usb4_rdma0/state")
            machine.succeed("grep '^4 1 1 roce-v2 10.0.4.2$' /sys/kernel/config/thunderbolt_ibverbs/usb4_rdma0/selection")
            machine.succeed("echo 1 > /sys/kernel/config/thunderbolt_ibverbs/usb4_rdma0/activate")
            machine.succeed("grep '^active$' /sys/kernel/config/thunderbolt_ibverbs/usb4_rdma0/state")
            machine.succeed("grep '^configured_links: 1$' /sys/kernel/debug/thunderbolt_ibverbs/summary")
            machine.succeed("grep '^link 1 name=usb4_rdma0 backend=native dev=4 port=1 gid=1 gid_type=2 addr=10.0.4.2$' /sys/kernel/debug/thunderbolt_ibverbs/configured_links")
            machine.fail("echo apple > /sys/kernel/config/thunderbolt_ibverbs/usb4_rdma0/backend")
            machine.succeed("rmdir /sys/kernel/config/thunderbolt_ibverbs/usb4_rdma0")
            machine.succeed("grep '^configured_links: 0$' /sys/kernel/debug/thunderbolt_ibverbs/summary")
            machine.succeed("rdma link show")
            machine.succeed("ibv_devices")
            machine.succeed("rmmod thunderbolt_ibverbs")
          '';
        };
      mkVerbsSmokeBuild =
        pkgs:
        pkgs.stdenv.mkDerivation {
          pname = "thunderbolt-ibverbs-verbs-smoke-build";
          version = "0.3.4";
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
            module = pkgs.linuxPackages_7_2.callPackage ./nix/module.nix { };
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
          targetModuleApp = pkgs.writeShellApplication {
            name = "tbv-target-module";
            runtimeInputs = [
              pkgs.coreutils
              pkgs.findutils
              pkgs.gawk
              pkgs.gnugrep
              pkgs.kmod
              pkgs.nix
              pkgs.openssh
            ];
            text = ''
              exec ${./tools/tbv-target-module.sh} "$@"
            '';
            meta = {
              description = "Build, verify, copy, and optionally reload thunderbolt_ibverbs for a NixOS target kernel";
              maintainers = with pkgs.lib.maintainers; [ georgewhewell ];
            };
          };
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
          tbv-target-module = {
            type = "app";
            program = lib.getExe targetModuleApp;
            meta = {
              description = "Build and reload thunderbolt_ibverbs only when the module matches the target's booted kernel";
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
          vm-smoke.nixos = mkNixosVmSmoke pkgs {
            kernelPackages = pkgs.linuxPackages_7_2;
            expectedNativeControl = "legacy";
          };
          vm-smoke.nixos-thunderbolt = mkNixosVmSmoke pkgs {
            kernelPackages = mkThunderboltLinuxPackages pkgs;
            expectedNativeControl = "source_aware";
          };
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
              pkgs.bpftrace
              pkgs.kmod
              pkgs.perf
              pkgs.trace-cmd
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
            thunderbolt-ibverbs = final.linuxPackages_7_2.callPackage ./nix/module.nix { };
            thunderbolt-ibverbs-linux-thunderbolt = thunderboltLinuxPackages.callPackage ./nix/module.nix { };
          }
        );

      lib = kernelPatchSets;

      legacyPackages = forLinuxSystems (_pkgs: kernelPatchSets);

      nixosModules.default = import ./module.nix { inherit self; };
    };
}
