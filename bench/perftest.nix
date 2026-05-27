{ lib
, pkgs
, perftest
, rdma-core-usb4
, runnerSrc
, benchConfig
}:

let
  inherit (lib) concatMap optionalAttrs;
  defaultTopology = benchConfig.topologies.strixPair;
  defaultModuleProfile = defaultTopology.moduleProfile or benchConfig.moduleProfiles.linuxPerf;

  renderOption = flag: value:
    if value == null || value == false then
      [ ]
    else if value == true then
      [ "--${flag}" ]
    else
      [ "--${flag}" (toString value) ];

  renderOptions = options:
    concatMap (name: renderOption name options.${name}) (builtins.attrNames options);

  mkPerftest = { name, bin, options ? { }, directions ? [ "forward" ], tags ? [ ], expectFailure ? false }:
    {
      inherit name bin directions tags expectFailure;
      argv = renderOptions options;
      options = options;
    };

  sizes = {
    small = [ 1024 4096 65536 ];
    latency = [ 64 256 1024 4096 16384 65536 ];
    full = [ 1024 4096 16384 65536 262144 1048576 ];
  };

  qps = {
    normal = [ 1 2 4 ];
    fourRail = [ 1 2 4 8 ];
  };

  mkBwCases = { prefix, bins, sizes, qps, directions ? [ "forward" ], baseOptions ? { } }:
    concatMap
      (bin:
        concatMap
          (q:
            map
              (size:
                mkPerftest {
                  name = "${prefix}.${bin}.size${toString size}.qps${toString q}";
                  inherit bin directions;
                  options = baseOptions // {
                    connection = "RC";
                    iters = if size >= 1048576 then 300 else if size >= 262144 then 800 else 1000;
                    qp = q;
                    size = size;
                    "tx-depth" = if size >= 1048576 then 16 else if size >= 262144 then 32 else 128;
                  } // optionalAttrs (bin == "ib_send_bw") {
                    "rx-depth" = 512;
                  };
                })
              sizes)
          qps)
      bins;

  mkLatCases = { prefix, bins, sizes, directions ? [ "forward" ], baseOptions ? { } }:
    concatMap
      (bin:
        map
          (size:
            mkPerftest {
              name = "${prefix}.${bin}.size${toString size}";
              inherit bin directions;
              options = baseOptions // {
                connection = "RC";
                iters = 1000;
                qp = 1;
                size = size;
              } // optionalAttrs (bin == "ib_send_lat") {
                "rx-depth" = 512;
              };
            })
          sizes)
      bins;

  mkReadOutCases = { prefix, sizes, outs, directions ? [ "forward" ], baseOptions ? { } }:
    concatMap
      (size:
        map
          (outstanding:
            mkPerftest {
              name = "${prefix}.ib_read_lat.size${toString size}.outs${toString outstanding}";
              bin = "ib_read_lat";
              inherit directions;
              options = baseOptions // {
                connection = "RC";
                iters = 1000;
                outs = outstanding;
                qp = 1;
                size = size;
              };
            })
          outs)
      sizes;

  mkBidiCases = { prefix, bins, sizes, qps, baseOptions ? { } }:
    concatMap
      (bin:
        concatMap
          (q:
            map
              (size:
                mkPerftest {
                  name = "${prefix}.${bin}.bidi.size${toString size}.qps${toString q}";
                  inherit bin;
                  directions = [ "forward" ];
                  options = baseOptions // {
                    connection = "RC";
                    iters = if size >= 1048576 then 300 else 1000;
                    qp = q;
                    size = size;
                    "tx-depth" = if size >= 1048576 then 16 else 128;
                  } // optionalAttrs (bin == "ib_send_bw") {
                    "rx-depth" = 512;
                  } // {
                    bidirectional = true;
                  };
                })
              sizes)
          qps)
      bins;

  mkPlan = { name, description, defaults ? { }, cases }:
    {
      format = 1;
      inherit name description cases;
      config = {
        topology = defaultTopology;
        moduleProfile = defaultModuleProfile;
      };
      defaults = {
        server = defaultTopology.head;
        client = defaultTopology.worker;
        dev = defaultTopology.rdmaDev;
        gidIndex = defaultTopology.gidIndex;
        backend = defaultModuleProfile.nativeBackend or "native";
        expectRails = defaultTopology.expect.nativeRails or null;
        expectSpeed = defaultTopology.expect.nativeRailSpeed or "any";
        timeout = 90;
        basePort = 18700;
        serverStartDelay = 0.8;
        directions = "from-plan";
        resultDir = "thunderbolt-ibverbs/results";
        copyTools = true;
        stopOnFail = false;
      } // defaults;
      forcedOutput = {
        cpuFreq = true;
        outJson = true;
        reportGbitsForBandwidth = true;
        reportBothForBidirectional = true;
      };
      telemetry = {
        debugfsSummary = true;
        debugfsPeers = true;
        moduleParams = true;
        rdmaLink = true;
        ibvDevinfo = true;
        ipLinkStats = true;
        dmesgTail = false;
      };
    };

  smokeCases =
    mkBwCases {
      prefix = "smoke";
      bins = [ "ib_write_bw" "ib_read_bw" "ib_send_bw" ];
      sizes = [ 4096 ];
      qps = [ 1 ];
    } ++
    mkLatCases {
      prefix = "smoke";
      bins = [ "ib_write_lat" "ib_read_lat" "ib_send_lat" ];
      sizes = [ 64 4096 ];
    };

  defaultCases =
    mkBwCases {
      prefix = "default";
      bins = [ "ib_write_bw" "ib_read_bw" "ib_send_bw" ];
      sizes = sizes.small;
      qps = qps.normal;
    } ++
    mkBidiCases {
      prefix = "default";
      bins = [ "ib_write_bw" "ib_read_bw" "ib_send_bw" ];
      sizes = [ 65536 ];
      qps = [ 2 4 ];
    } ++
    mkLatCases {
      prefix = "default";
      bins = [ "ib_write_lat" "ib_read_lat" "ib_send_lat" ];
      sizes = [ 64 4096 65536 ];
    };

  fullCases =
    mkBwCases {
      prefix = "full";
      bins = [ "ib_write_bw" "ib_read_bw" "ib_send_bw" ];
      sizes = sizes.full;
      qps = qps.fourRail;
      directions = [ "forward" "reverse" ];
    } ++
    mkBidiCases {
      prefix = "full";
      bins = [ "ib_write_bw" "ib_read_bw" "ib_send_bw" ];
      sizes = [ 4096 65536 1048576 ];
      qps = [ 1 2 4 8 ];
    } ++
    mkLatCases {
      prefix = "full";
      bins = [ "ib_write_lat" "ib_read_lat" "ib_send_lat" ];
      sizes = sizes.latency;
      directions = [ "forward" "reverse" ];
    };

  plans = {
    smoke = mkPlan {
      name = "smoke";
      description = "Short RC WRITE/READ/SEND perftest smoke suite.";
      cases = smokeCases;
    };
    default = mkPlan {
      name = "default";
      description = "Balanced Thunderbolt ibverbs perftest suite covering WRITE, READ, SEND, latency, and bidirectional bandwidth.";
      cases = defaultCases;
    };
    full = mkPlan {
      name = "full";
      description = "Large perftest matrix; use when the topology is stable and you want comprehensive numbers.";
      cases = fullCases;
    };
    native4rail = mkPlan {
      name = "native4rail";
      description = "Native linux_perf dual-cable four-rail 20 Gb/s suite.";
      defaults = {
        expectRails = benchConfig.topologies.strixPairFourRail.expect.nativeRails;
        expectSpeed = benchConfig.topologies.strixPairFourRail.expect.nativeRailSpeed;
      };
      cases =
        mkBwCases {
          prefix = "native4rail";
          bins = [ "ib_write_bw" "ib_read_bw" "ib_send_bw" ];
          sizes = sizes.full;
          qps = qps.fourRail;
        } ++
        mkBidiCases {
          prefix = "native4rail";
          bins = [ "ib_write_bw" "ib_read_bw" "ib_send_bw" ];
          sizes = [ 65536 1048576 ];
          qps = [ 4 8 ];
        } ++
        mkLatCases {
          prefix = "native4rail";
          bins = [ "ib_write_lat" "ib_read_lat" "ib_send_lat" ];
          sizes = [ 64 4096 65536 ];
        };
    };
    rxeEthernet = mkPlan {
      name = "rxe-ethernet";
      description = "RXE over the normal Ethernet bridge.";
      defaults = {
        dev = "rxe_eth0";
        backend = null;
        expectRails = null;
        expectSpeed = "any";
      };
      cases = defaultCases;
    };
    rxeTbnet = mkPlan {
      name = "rxe-tbnet";
      description = "RXE over thunderbolt_net.";
      defaults = {
        dev = "rxe_tb0";
        backend = null;
        expectRails = null;
        expectSpeed = "any";
      };
      cases = defaultCases;
    };
    readOuts = mkPlan {
      name = "read-outs";
      description = "Focused RC READ latency diagnostic varying outstanding RDMA READ count.";
      defaults = {
        expectRails = 1;
        timeout = 120;
      };
      cases = mkReadOutCases {
        prefix = "readouts";
        sizes = [ 4096 65536 ];
        outs = [ 1 2 4 8 16 32 64 128 ];
      };
    };
  };

  mkRunner = name: plan:
    let
      planFile = pkgs.writeText "tbv-perftest-plan-${name}.json" (builtins.toJSON plan);
    in
    pkgs.writeShellApplication {
      name = "tbv-perftest-${name}";
      runtimeInputs = with pkgs; [
        coreutils
        nix
        openssh
        python3
      ];
      text = ''
        export TBV_RDMA_CORE=${rdma-core-usb4}
        export TBV_PERFTEST=${perftest}
        exec ${pkgs.python3}/bin/python3 -u ${runnerSrc} --plan ${planFile} "$@"
      '';
      meta = with pkgs.lib; {
        description = plan.description;
        platforms = platforms.linux;
      };
    };

  runners = lib.mapAttrs mkRunner plans;
in
{
  inherit plans runners;
  apps = lib.mapAttrs (_: drv: {
    type = "app";
    program = lib.getExe drv;
  }) runners;
}
