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

  # Strict declarative config: cfg.config is either `false` (do not manage
  # module params at all) or an attrset of exact kernel-module param names
  # to values. No camelCase mapping; whatever you write is what gets emitted.
  managed = cfg.config != false;
  managedConfig = if managed then cfg.config else {};

  renderValue = value:
    if value == null
    then null
    else if builtins.isBool value
    then
      if value
      then "1"
      else "0"
    else toString value;

  renderedModuleOptions =
    lib.concatStringsSep " "
    (lib.concatLists (lib.mapAttrsToList
      (name: value:
        let rendered = renderValue value;
        in lib.optional (rendered != null) "${name}=${rendered}")
      managedConfig));

  configuredTbnetIdentity = managedConfig.tbnet_identity or null;
  configuredTbnet = managedConfig.tbnet or null;
  configuredTbnetIdentityTbnet = managedConfig.tbnet_identity_tbnet or "thunderbolt0";

  loadStockThunderboltNet =
    builtins.elem configuredTbnetIdentity ["stock" "stock_proxy"];

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
    if cfg.check.expectedProfile != null
    then cfg.check.expectedProfile
    else managedConfig.profile or null;

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

      deadline=$((SECONDS + timeout))
      timed_out() {
        [ "$SECONDS" -ge "$deadline" ]
      }

      while ! grep -q '^thunderbolt_ibverbs ' /proc/modules; do
        if timed_out; then
          fail "module thunderbolt_ibverbs is not loaded within ''${timeout}s"
        fi
        sleep 1
      done

      while [ ! -r "$summary" ]; do
        if timed_out; then
          fail "debugfs summary did not appear at $summary within ''${timeout}s"
        fi
        sleep 1
      done

      wait_summary_value() {
        key="$1"
        expected="$2"
        label="$3"
        actual=""

        while true; do
          actual="$(get_summary "$key" || true)"
          if [ "$actual" = "$expected" ]; then
            return 0
          fi
          if timed_out; then
            fail "$label: expected $expected, got ''${actual:-<missing>}"
          fi
          sleep 1
        done
      }

      if [ -n "$expected_profile" ]; then
        wait_summary_value profile "$expected_profile" "profile mismatch"
      fi

      if [ -n "$expected_native_control" ]; then
        wait_summary_value native_control "$expected_native_control" "native_control mismatch"
      fi

      if [ "$require_verbs" = "1" ]; then
        wait_summary_value verbs_registered 1 "verbs device is not registered"
      fi

      if [ -n "$min_ready_rails" ]; then
        ready_rails=0
        while true; do
          if [ -r "$peers" ]; then
            ready_rails="$( { grep -o 'data_ready=1' "$peers" || true; } | wc -l | tr -d ' ')"
            if [ "$ready_rails" -ge "$min_ready_rails" ]; then
              break
            fi
          fi
          if timed_out; then
            if [ ! -r "$peers" ]; then
              fail "debugfs peers file is missing at $peers within ''${timeout}s"
            fi
            fail "ready rail count too low: expected >= $min_ready_rails, got $ready_rails"
          fi
          sleep 1
        done
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
    meta = {
      maintainers = with lib.maintainers; [ georgewhewell ];
    };
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
      tbnet_policy="${if configuredTbnet == null then "prefer_rdma" else configuredTbnet}"
      tbnet_identity="${if configuredTbnetIdentity == null then "off" else configuredTbnetIdentity}"
      tbnet_identity_tbnet="${configuredTbnetIdentityTbnet}"
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

      softdeps="$(modinfo -F softdep "$module" || true)"
      load_softdeps=1
      for dep in $softdeps; do
        case "$dep" in
          pre:)
            load_softdeps=1
            continue
            ;;
          post:)
            load_softdeps=0
            continue
            ;;
        esac
        [ "$load_softdeps" = "1" ] || continue
        [ -n "$dep" ] || continue
        modprobe "$dep" || true
      done

      deps=$(modinfo -F depends "$module" | tr ',' ' ' || true)
      for dep in $deps; do
        [ -n "$dep" ] || continue
        modprobe "$dep"
      done

      read -r -a opt_args <<< "$options"
      insmod "$module" "''${opt_args[@]}"

      if [ "$wait_secs" != "0" ]; then
        sleep "$wait_secs"
      fi

      if [ "$check_after_reload" = "1" ]; then
        ${checkHelper}/bin/thunderbolt-ibverbs-check
      fi
    '';
    meta = {
      maintainers = with lib.maintainers; [ georgewhewell ];
    };
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

    blacklist.enable = lib.mkOption {
      type = types.bool;
      default = false;
      description = ''
        Blacklist potentially-interfering kernel modules (currently:
        thunderbolt_net). Useful when running with tbnet_identity=minimal_packet,
        which requires thunderbolt_ibverbs to own the ThunderboltIP service
        from boot. Off by default — set true only when you know you want it.
      '';
    };

    waitSeconds = lib.mkOption {
      type = types.ints.unsigned;
      default = 8;
      description = "Default seconds the reload helper waits after loading thunderbolt_ibverbs.";
    };

    config = lib.mkOption {
      type = types.either (types.enum [false]) (types.attrsOf (types.oneOf [
        types.bool
        types.int
        types.str
        (types.nullOr types.str)
      ]));
      default = false;
      example = lib.literalExpression ''
        {
          profile = "linux_perf";
          tbnet = "prefer_rdma";
          tbnet_identity = "off";
          lanes = "2";
          register_verbs = true;
        }
      '';
      description = ''
        Strict declarative thunderbolt_ibverbs module configuration.

        - `false` (default): enable the module and load it on boot, but do not
          emit modprobe options, do not run the activation reload helper, and
          do not touch existing module state. The driver runs with its
          compiled-in defaults. Use this when you want the module present but
          intend to manage it manually (e.g. during interactive testing).

        - attrset: strict mode. Exactly the listed keys are written to
          `/etc/modprobe.d/` as `options thunderbolt_ibverbs <key=value> ...`,
          and the activation reload helper applies them on switch. Keys are
          the raw kernel-module parameter names (snake_case), values may be
          bool (rendered "1"/"0"), int, or string. `null` values are dropped.
      '';
    };

    extraModuleOptions = lib.mkOption {
      type = types.listOf types.str;
      default = [];
      example = ["debug=1"];
      description = ''
        Additional raw thunderbolt_ibverbs module options appended after the
        keys from `config`. Only used when `config` is an attrset.
      '';
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
        type = types.nullOr types.str;
        default = null;
        description = "Expected profile in debugfs summary. Defaults to the configured `profile` key when checks run.";
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
      (managed && configuredTbnetIdentity == "minimal_packet" && !cfg.blacklist.enable)
      "hardware.thunderbolt-ibverbs: config.tbnet_identity=minimal_packet normally requires thunderbolt_net to be absent at boot. Set hardware.thunderbolt-ibverbs.blacklist.enable = true to blacklist it.";

    hardware."thunderbolt-ibverbs" = {
      resolvedPackage = thunderboltIbverbsModule;
      reloadHelper = reloadHelper;
      checkHelper = checkHelper;
      renderedModuleOptions =
        if managed
        then lib.concatStringsSep " " (lib.filter (s: s != "") [renderedModuleOptions (lib.concatStringsSep " " cfg.extraModuleOptions)])
        else "";
    };

    boot.kernelPackages =
      lib.mkIf cfg.kernel.useProjectKernel
      (lib.mkOverride cfg.kernel.overridePriority projectKernelPackages);

    boot.blacklistedKernelModules =
      lib.optional cfg.blacklist.enable "thunderbolt_net";

    boot.kernelModules =
      lib.optionals cfg.loadOnBoot (["ib_uverbs"] ++ lib.optional loadStockThunderboltNet "thunderbolt_net" ++ ["thunderbolt_ibverbs"]);

    boot.extraModulePackages = [thunderboltIbverbsModule];

    boot.extraModprobeConfig = lib.mkIf managed ''
      options thunderbolt_ibverbs ${renderedModuleOptions} ${lib.concatStringsSep " " cfg.extraModuleOptions}
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
