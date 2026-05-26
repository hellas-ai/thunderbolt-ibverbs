{
  self ? null,
  inputs ? {},
}: {
  config,
  lib,
  pkgs,
  ...
}: let
  cfg = config.hardware."thunderbolt-ibverbs";
  types = lib.types;
  system = pkgs.stdenv.hostPlatform.system;

  projectPackages =
    if self != null && self ? packages && builtins.hasAttr system self.packages
    then self.packages.${system}
    else {};

  source =
    if cfg.source != null
    then cfg.source
    else if self != null && self ? outPath
    then self.outPath
    else ./.;

  projectKernelPackages = pkgs.linuxPackagesFor projectPackages.linux-thunderbolt;

  kernelPackages =
    if cfg.kernel.useProjectKernel
    then projectKernelPackages
    else config.boot.kernelPackages;

  buildKernelModule = kernelPackages:
    kernelPackages.callPackage ./nix/module.nix {
      inherit source;
    };

  optionSpecs = {
    profile = {
      param = "profile";
      type = types.enum ["auto" "mac_compat" "linux_perf" "mixed"];
      default = "linux_perf";
      description = "Driver profile. mac_compat enables the Apple-compatible UC path; linux_perf enables the native Linux path; mixed enables both.";
    };
    compat = {
      param = "compat";
      type = types.enum ["auto" "force" "off"];
      default = "off";
      description = "Compatibility mode for peer/backend selection.";
    };
    tbnet = {
      param = "tbnet";
      type = types.enum ["auto" "allow" "prefer_rdma" "block"];
      default = "prefer_rdma";
      description = "Thunderbolt-net coexistence policy.";
    };
    tbnetIdentity = {
      param = "tbnet_identity";
      type = types.enum ["auto" "stock" "stock_proxy" "minimal_packet" "off"];
      default = "off";
      description = "ThunderboltIP identity strategy used for Apple compatibility.";
    };
    tbnetIdentityTbnet = {
      param = "tbnet_identity_tbnet";
      type = types.str;
      default = "thunderbolt0";
      description = "Thunderbolt-net interface name used by stock or proxy identity modes.";
    };
    tbnetIdentityGid = {
      param = "tbnet_identity_gid";
      type = types.str;
      default = "auto";
      description = "Netdev name used for the Apple-compatible GID identity.";
    };
    tbnetIdentityMinimalE2e = {
      param = "tbnet_identity_minimal_e2e";
      type = types.bool;
      default = false;
      description = "Enable E2E flow control on minimal ThunderboltIP packet rings. Keep disabled for Strix Halo Mac compatibility.";
    };
    tbnetIdentityMinimalAppleOnly = {
      param = "tbnet_identity_minimal_apple_only";
      type = types.bool;
      default = true;
      description = "Bind minimal ThunderboltIP identity only to Apple peers so Linux peers remain available for the native backend.";
    };
    roceNetdev = {
      param = "roce_netdev";
      type = types.nullOr types.str;
      default = "br0.lan";
      description = "Netdev used for RoCE GID metadata. Set null to omit roce_netdev.";
    };
    lanes = {
      param = "lanes";
      type = types.str;
      default = "2";
      description = "Lane request passed to the driver: auto, N, or MIN-MAX.";
    };
    bindServices = {
      param = "bind_services";
      type = types.bool;
      default = true;
      description = "Bind supported Thunderbolt services.";
    };
    nativePrtcstns = {
      param = "native_prtcstns";
      type = types.ints.unsigned;
      default = 0;
      description = "Native backend PRTCSTNS override. 0 lets the driver choose.";
    };
    applePrtcstns = {
      param = "apple_prtcstns";
      type = types.ints.unsigned;
      default = 0;
      description = "Apple-compatible backend PRTCSTNS override. 0 lets the driver choose.";
    };
    allocateRings = {
      param = "allocate_rings";
      type = types.bool;
      default = true;
      description = "Allocate Thunderbolt DMA rings.";
    };
    startRings = {
      param = "start_rings";
      type = types.bool;
      default = true;
      description = "Start allocated Thunderbolt DMA rings.";
    };
    negotiateNative = {
      param = "negotiate_native";
      type = types.bool;
      default = true;
      description = "Run native Linux peer/rail negotiation.";
    };
    enableTunnels = {
      param = "enable_tunnels";
      type = types.bool;
      default = true;
      description = "Enable Thunderbolt DMA tunnels after negotiation.";
    };
    nativeData = {
      param = "native_data";
      type = types.bool;
      default = true;
      description = "Enable native Linux data path.";
    };
    appleData = {
      param = "apple_data";
      type = types.bool;
      default = false;
      description = "Enable Apple-compatible data path.";
    };
    nativeFragmentStriping = {
      param = "native_fragment_striping";
      type = types.bool;
      default = false;
      description = "Enable experimental native fragment striping.";
    };
    registerVerbs = {
      param = "register_verbs";
      type = types.bool;
      default = true;
      description = "Register the ibverbs device.";
    };
    zcopyMinBytes = {
      param = "zcopy_min_bytes";
      type = types.ints.unsigned;
      default = 4096;
      description = "Minimum native RDMA WRITE bytes before zero-copy page streaming is used; 0 disables zero-copy.";
    };
    qpTimeoutMs = {
      param = "qp_timeout_ms";
      type = types.ints.unsigned;
      default = 5000;
      description = "Default QP operation timeout in milliseconds.";
    };
    appleTxMaxInflightWr = {
      param = "apple_tx_max_inflight_wr";
      type = types.ints.unsigned;
      default = 1;
      description = "Maximum Apple-compatible UC SEND work requests in flight per QP; 0 disables the software window.";
    };
    appleTxMaxInflightFrames = {
      param = "apple_tx_max_inflight_frames";
      type = types.ints.unsigned;
      default = 2;
      description = "Maximum Apple-compatible 4 KiB FA57 frames queued from one SEND before waiting for TX completions; 0 disables the frame window.";
    };
    appleRxPendingBytes = {
      param = "apple_rx_pending_bytes";
      type = types.ints.unsigned;
      default = 16777216;
      description = "Maximum bytes buffered per early Apple UC receive when no receive WQE is posted.";
    };
    appleRxPendingSlots = {
      param = "apple_rx_pending_slots";
      type = types.ints.unsigned;
      default = 4096;
      description = "Maximum number of early Apple UC receives buffered per QP.";
    };
    appleRxPendingTotalBytes = {
      param = "apple_rx_pending_total_bytes";
      type = types.ints.unsigned;
      default = 67108864;
      description = "Maximum aggregate bytes buffered for early Apple UC receives per QP.";
    };
    appleRxTrace = {
      param = "apple_rx_trace";
      type = types.ints.unsigned;
      default = 0;
      description = "Print the first N Apple RX callbacks with SOF/EOF and assembly state.";
    };
    nhiInterruptThrottleNs = {
      param = "nhi_interrupt_throttle_ns";
      type = types.ints.unsigned;
      default = 0;
      description = "NHI interrupt throttle in nanoseconds; 0 leaves driver default behavior.";
    };
  };

  mkDriverOption = _name: spec:
    lib.mkOption {
      inherit (spec) type default description;
    };

  renderValue = value:
    if value == null
    then null
    else if builtins.isBool value
    then
      if value
      then "1"
      else "0"
    else toString value;

  renderDriverOption = name: value: let
    rendered = renderValue value;
    param = optionSpecs.${name}.param or name;
  in
    lib.optional (rendered != null) "${param}=${rendered}";

  renderedModuleOptions =
    lib.concatStringsSep " "
    ((lib.concatLists (lib.mapAttrsToList renderDriverOption cfg.moduleOptions))
      ++ cfg.extraModuleOptions);

  thunderboltIbverbsModule =
    if cfg.package != null
    then cfg.package
    else cfg.packageFor kernelPackages;

  modulePath = "${thunderboltIbverbsModule}/lib/modules/${kernelPackages.kernel.modDirVersion}/extra/thunderbolt_ibverbs.ko";

  nullableString = value:
    if value == null
    then ""
    else toString value;

  boolString = value:
    if value
    then "1"
    else "0";

  effectiveExpectedProfile =
    if cfg.check.expectedProfile == null
    then cfg.moduleOptions.profile
    else cfg.check.expectedProfile;

  effectiveBlacklistThunderboltNet =
    if cfg.blacklistThunderboltNet != null
    then cfg.blacklistThunderboltNet
    else cfg.moduleOptions.tbnetIdentity == "minimal_packet";

  loadStockThunderboltNet = builtins.elem cfg.moduleOptions.tbnetIdentity ["stock" "stock_proxy"];

  checkHelper = pkgs.writeShellApplication {
    name = "thunderbolt-ibverbs-check";
    runtimeInputs = with pkgs; [
      coreutils
      gawk
      gnugrep
      gnused
      kmod
    ];
    text = ''
      summary="${cfg.check.debugfsRoot}/summary"
      peers="${cfg.check.debugfsRoot}/peers"
      timeout="${toString cfg.check.timeoutSeconds}"
      min_ready_rails=${lib.escapeShellArg (nullableString cfg.check.minReadyRails)}
      expected_speed=${lib.escapeShellArg (nullableString cfg.check.expectedRailSpeed)}
      expected_profile=${lib.escapeShellArg (nullableString effectiveExpectedProfile)}
      expected_native_control=${lib.escapeShellArg (nullableString cfg.check.expectedNativeControl)}
      require_verbs="${boolString cfg.check.requireVerbs}"
      fail_on_legacy_ambiguity="${boolString cfg.check.failOnLegacyAmbiguity}"

      fail() {
        echo "thunderbolt-ibverbs-check: $*" >&2
        exit 1
      }

      get_summary() {
        awk -F': *' -v key="$1" '$1 == key { print $2; found=1; exit } END { if (!found) exit 1 }' "$summary"
      }

      if ! grep -q '^thunderbolt_ibverbs ' /proc/modules; then
        fail "module thunderbolt_ibverbs is not loaded"
      fi

      deadline=$((SECONDS + timeout))
      while [ ! -r "$summary" ]; do
        if [ "$SECONDS" -ge "$deadline" ]; then
          fail "debugfs summary did not appear at $summary within ''${timeout}s"
        fi
        sleep 1
      done

      if [ -n "$expected_profile" ]; then
        actual_profile="$(get_summary profile || true)"
        [ "$actual_profile" = "$expected_profile" ] ||
          fail "profile mismatch: expected $expected_profile, got ''${actual_profile:-<missing>}"
      fi

      if [ -n "$expected_native_control" ]; then
        actual_native_control="$(get_summary native_control || true)"
        [ "$actual_native_control" = "$expected_native_control" ] ||
          fail "native_control mismatch: expected $expected_native_control, got ''${actual_native_control:-<missing>}"
      fi

      if [ "$require_verbs" = "1" ]; then
        verbs_registered="$(get_summary verbs_registered || true)"
        [ "$verbs_registered" = "1" ] ||
          fail "verbs device is not registered"
      fi

      if [ -n "$min_ready_rails" ]; then
        [ -r "$peers" ] || fail "debugfs peers file is missing at $peers"
        ready_rails="$(grep -o 'data_ready=1' "$peers" | wc -l | tr -d ' ')"
        [ "$ready_rails" -ge "$min_ready_rails" ] ||
          fail "ready rail count too low: expected >= $min_ready_rails, got $ready_rails"
      fi

      if [ -n "$expected_speed" ]; then
        [ -r "$peers" ] || fail "debugfs peers file is missing at $peers"
        bad_speed_lines="$(grep 'data_ready=1' "$peers" | grep -v "link_speed=$expected_speed" || true)"
        [ -z "$bad_speed_lines" ] ||
          fail "at least one ready rail is not $expected_speed: $bad_speed_lines"
      fi

      if [ "$fail_on_legacy_ambiguity" = "1" ]; then
        legacy_limited="$(get_summary native_legacy_ambiguous_limited || echo 0)"
        [ "$legacy_limited" = "0" ] ||
          fail "legacy source-blind ambiguity counter is nonzero: $legacy_limited"
      fi

      echo "thunderbolt-ibverbs-check: ok"
    '';
  };

  reloadHelper = pkgs.writeShellApplication {
    name = "thunderbolt-ibverbs-reload-system";
    runtimeInputs = with pkgs; [
      coreutils
      gnugrep
      iproute2
      kmod
    ];
    text = ''
      if [ "$(id -u)" -ne 0 ]; then
        echo "thunderbolt-ibverbs-reload-system: run this helper with sudo" >&2
        exit 1
      fi

      module="${modulePath}"
      options="''${TBV_OPTIONS:-${renderedModuleOptions}}"
      wait_secs="''${TBV_WAIT_SECS:-${toString cfg.waitSeconds}}"
      check_after_reload="''${TBV_CHECK_AFTER_RELOAD:-${boolString cfg.check.afterReload}}"
      tbnet_policy="prefer_rdma"
      tbnet_identity="off"
      tbnet_identity_tbnet="thunderbolt0"
      booted_module="/run/booted-system/kernel-modules/lib/modules/$(uname -r)/extra/thunderbolt_ibverbs.ko"

      if [ "''${TBV_ALLOW_NON_BOOTED_MODULE:-0}" != "1" ] &&
         [ -e "$booted_module" ] && ! cmp -s "$module" "$booted_module"; then
        echo "thunderbolt-ibverbs-reload-system: refusing to load thunderbolt_ibverbs from a non-booted system closure" >&2
        echo "  requested: $module" >&2
        echo "  booted:    $booted_module" >&2
        echo "reboot into the new generation first, or set TBV_ALLOW_NON_BOOTED_MODULE=1 for an ABI-compatible dev test" >&2
        exit 1
      fi

      for opt in $options; do
        case "$opt" in
          tbnet=*) tbnet_policy="''${opt#tbnet=}" ;;
          tbnet_identity=*) tbnet_identity="''${opt#tbnet_identity=}" ;;
          tbnet_identity_tbnet=*) tbnet_identity_tbnet="''${opt#tbnet_identity_tbnet=}" ;;
        esac
      done

      if [ "$tbnet_identity" = "minimal_packet" ] && grep -q '^thunderbolt_net ' /proc/modules; then
        echo "thunderbolt-ibverbs-reload-system: tbnet_identity=minimal_packet must own the ThunderboltIP service from boot; rebuild/reboot with the declarative thunderbolt_net blacklist active" >&2
        exit 1
      fi

      unbind_tbnet_services() {
        local dev service driver key

        [ -d /sys/bus/thunderbolt/drivers/thunderbolt-net ] || return 0
        for dev in /sys/bus/thunderbolt/devices/*; do
          [ -e "$dev/key" ] || continue
          key="$(cat "$dev/key" 2>/dev/null || true)"
          [ "$key" = "network" ] || continue
          service="$(basename "$dev")"
          driver="none"
          if [ -L "$dev/driver" ]; then
            driver="$(basename "$(readlink "$dev/driver")")"
          fi
          [ "$driver" = "thunderbolt-net" ] || continue
          printf '%s\n' "$service" > /sys/bus/thunderbolt/drivers/thunderbolt-net/unbind
        done
      }

      if grep -q '^thunderbolt_ibverbs ' /proc/modules; then
        rmmod thunderbolt_ibverbs
      fi

      if [ "$tbnet_policy" != "allow" ]; then
        unbind_tbnet_services
      fi

      case "$tbnet_identity" in
        stock|stock_proxy)
          modprobe thunderbolt_net
          for _ in $(seq 1 20); do
            if ip link show "$tbnet_identity_tbnet" >/dev/null 2>&1; then
              ip link set "$tbnet_identity_tbnet" up || true
              break
            fi
            sleep 0.5
          done
          ;;
      esac

      if [ "$tbnet_identity" = "minimal_packet" ]; then
        unbind_tbnet_services
        if grep -q '^thunderbolt_net ' /proc/modules; then
          echo "thunderbolt-ibverbs-reload-system: tbnet_identity=minimal_packet requires thunderbolt_net to be absent at load time; rebuild/reboot with the declarative thunderbolt_net blacklist active" >&2
          exit 1
        fi
      fi

      deps=$(modinfo -F depends "$module" | tr ',' ' ' || true)
      for dep in $deps; do
        [ -n "$dep" ] || continue
        modprobe "$dep"
      done
      modprobe ib_uverbs || true

      read -r -a opt_args <<< "$options"
      insmod "$module" "''${opt_args[@]}"

      if [ "$wait_secs" != "0" ]; then
        sleep "$wait_secs"
      fi

      if [ "$check_after_reload" = "1" ]; then
        ${checkHelper}/bin/thunderbolt-ibverbs-check
      fi
    '';
  };
in {
  options.hardware."thunderbolt-ibverbs" = {
    enable = lib.mkEnableOption "the Thunderbolt/USB4 ibverbs kernel module";

    source = lib.mkOption {
      type = types.nullOr types.path;
      default = null;
      description = "Source tree used to build thunderbolt_ibverbs.ko. Defaults to this flake source.";
    };

    kernel.useProjectKernel = lib.mkOption {
      type = types.bool;
      default = false;
      description = "Use this flake's patched linux-thunderbolt kernel package set.";
    };

    kernel.overridePriority = lib.mkOption {
      type = types.int;
      default = 900;
      description = "mkOverride priority used when selecting the project kernel package set.";
    };

    loadOnBoot = lib.mkOption {
      type = types.bool;
      default = true;
      description = "Load ib_uverbs and thunderbolt_ibverbs at boot through boot.kernelModules.";
    };

    userspaceTools = {
      enable = lib.mkOption {
        type = types.bool;
        default = true;
        description = "Install rdma-core command-line tools such as rdma, ibv_devices, and ibv_devinfo.";
      };

      package = lib.mkOption {
        type = types.package;
        default =
          if projectPackages ? rdma-core-usb4
          then projectPackages.rdma-core-usb4
          else pkgs.rdma-core;
        defaultText = lib.literalExpression "thunderbolt-ibverbs.packages.${system}.rdma-core-usb4 or pkgs.rdma-core";
        description = "rdma-core package used for userspace verbs tools. The flake default includes the usb4_rdma libibverbs provider.";
      };
    };

    blacklistThunderboltNet = lib.mkOption {
      type = types.nullOr types.bool;
      default = null;
      description = "Whether to blacklist thunderbolt_net. The default blacklists it only when tbnetIdentity is minimal_packet.";
    };

    waitSeconds = lib.mkOption {
      type = types.ints.unsigned;
      default = 8;
      description = "Default seconds the reload helper waits after loading thunderbolt_ibverbs.";
    };

    extraModuleOptions = lib.mkOption {
      type = types.listOf types.str;
      default = [];
      example = ["debug=1"];
      description = "Additional raw thunderbolt_ibverbs module options appended after typed options.";
    };

    moduleOptions = lib.mkOption {
      type = types.submodule {
        options = lib.mapAttrs mkDriverOption optionSpecs;
      };
      default = {};
      description = "Typed thunderbolt_ibverbs kernel module parameters.";
    };

    renderedModuleOptions = lib.mkOption {
      type = types.str;
      readOnly = true;
      description = "Rendered thunderbolt_ibverbs module option string.";
    };

    packageFor = lib.mkOption {
      type = types.raw;
      default = buildKernelModule;
      defaultText = lib.literalExpression "kernelPackages: kernelPackages.callPackage ./nix/module.nix { source = ...; }";
      description = "Function from a kernel package scope to a thunderbolt_ibverbs kernel module package.";
    };

    package = lib.mkOption {
      type = types.nullOr types.package;
      default = null;
      description = "Prebuilt thunderbolt_ibverbs package override. If null, packageFor is applied to the selected kernel package scope.";
    };

    resolvedPackage = lib.mkOption {
      type = types.package;
      readOnly = true;
      description = "Resolved kernel module package used by this host.";
    };

    reloadHelper = lib.mkOption {
      type = types.package;
      readOnly = true;
      description = "System reload helper package.";
    };

    checkHelper = lib.mkOption {
      type = types.package;
      readOnly = true;
      description = "Runtime state check helper package.";
    };

    check = {
      enable = lib.mkOption {
        type = types.bool;
        default = false;
        description = "Run thunderbolt-ibverbs-check as a oneshot systemd service after boot module loading.";
      };
      afterReload = lib.mkOption {
        type = types.bool;
        default = false;
        description = "Run thunderbolt-ibverbs-check at the end of thunderbolt-ibverbs-reload-system.";
      };
      timeoutSeconds = lib.mkOption {
        type = types.ints.unsigned;
        default = 15;
        description = "Seconds to wait for debugfs state before the check helper fails.";
      };
      debugfsRoot = lib.mkOption {
        type = types.str;
        default = "/sys/kernel/debug/thunderbolt_ibverbs";
        description = "Debugfs directory exported by thunderbolt_ibverbs.";
      };
      expectedProfile = lib.mkOption {
        type = types.nullOr optionSpecs.profile.type;
        default = null;
        description = "Expected profile in debugfs summary. Defaults to the configured module profile when checks run.";
      };
      expectedNativeControl = lib.mkOption {
        type = types.nullOr (types.enum ["source_aware" "legacy"]);
        default = null;
        description = "Expected native control mode in debugfs summary.";
      };
      requireVerbs = lib.mkOption {
        type = types.bool;
        default = true;
        description = "Require verbs_registered=1.";
      };
      minReadyRails = lib.mkOption {
        type = types.nullOr types.ints.unsigned;
        default = null;
        description = "Minimum number of data_ready rails expected in debugfs peers.";
      };
      expectedRailSpeed = lib.mkOption {
        type = types.nullOr types.str;
        default = null;
        example = "20Gb/s";
        description = "Require every ready rail to report this link_speed.";
      };
      failOnLegacyAmbiguity = lib.mkOption {
        type = types.bool;
        default = true;
        description = "Fail if native_legacy_ambiguous_limited is nonzero.";
      };
    };
  };

  config = lib.mkIf cfg.enable {
    assertions = [
      {
        assertion = !cfg.kernel.useProjectKernel || projectPackages ? linux-thunderbolt;
        message = "hardware.thunderbolt-ibverbs.kernel.useProjectKernel requires importing module.nix from the thunderbolt-ibverbs flake.";
      }
    ];

    warnings =
      lib.optional
      (cfg.moduleOptions.tbnetIdentity == "minimal_packet" && effectiveBlacklistThunderboltNet == false)
      "hardware.thunderbolt-ibverbs: tbnetIdentity=minimal_packet normally requires thunderbolt_net to be absent before module load.";

    hardware."thunderbolt-ibverbs" = {
      resolvedPackage = thunderboltIbverbsModule;
      reloadHelper = reloadHelper;
      checkHelper = checkHelper;
      renderedModuleOptions = renderedModuleOptions;
    };

    boot.kernelPackages =
      lib.mkIf cfg.kernel.useProjectKernel
      (lib.mkOverride cfg.kernel.overridePriority projectKernelPackages);

    boot.blacklistedKernelModules =
      lib.optional effectiveBlacklistThunderboltNet "thunderbolt_net";

    boot.kernelModules =
      lib.optionals cfg.loadOnBoot (["ib_uverbs"] ++ lib.optional loadStockThunderboltNet "thunderbolt_net" ++ ["thunderbolt_ibverbs"]);

    boot.extraModulePackages = [thunderboltIbverbsModule];

    boot.extraModprobeConfig = ''
      options thunderbolt_ibverbs ${renderedModuleOptions}
    '';

    environment.systemPackages = [
      reloadHelper
      checkHelper
    ] ++ lib.optional cfg.userspaceTools.enable cfg.userspaceTools.package;

    systemd.services.thunderbolt-ibverbs-check = lib.mkIf cfg.check.enable {
      description = "Validate thunderbolt_ibverbs runtime state";
      wantedBy = ["multi-user.target"];
      wants = ["sys-kernel-debug.mount"];
      after = ["systemd-modules-load.service" "sys-kernel-debug.mount"];
      serviceConfig = {
        Type = "oneshot";
        ExecStart = "${checkHelper}/bin/thunderbolt-ibverbs-check";
      };
    };
  };
}
