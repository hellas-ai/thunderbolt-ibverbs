{ lib
, pkgs
, perftest
, perftestDarwin ? null
, rdma-core-usb4
, runnerSrc
, benchConfig
}:

let
  inherit (lib) concatMap getExe hasPrefix optionalAttrs;

  topology = benchConfig.topologies.strixPair;
  moduleProfile = topology.moduleProfile or benchConfig.moduleProfiles.linuxPerf;

  bwSizes = [ 1024 4096 16384 65536 262144 1048576 ];
  latSizes = [ 64 256 1024 4096 16384 65536 ];
  bidiSizes = [ 4096 65536 1048576 ];
  allQps = [ 1 2 4 8 ];
  readOuts = [ 1 2 4 8 16 32 64 128 ];
  bothDirections = [ "forward" "reverse" ];

  # Full RC bin set. Used by strix↔strix native runs. RDMA READ is RC-only
  # by InfiniBand spec, so it never appears in the UC bin lists below.
  bwBins = [ "ib_write_bw" "ib_read_bw" "ib_send_bw" ];
  latBins = [ "ib_write_lat" "ib_read_lat" "ib_send_lat" ];

  # UC bin set, for Apple-compatible runs (macOS Thunderbolt RDMA is UC-only;
  # `Connection_type: RC` errors on QP modify against rdma_en* devices). No
  # ib_read_* because UC has no RDMA READ.
  bwBinsUc = [ "ib_write_bw" "ib_send_bw" ];
  latBinsUc = [ "ib_write_lat" "ib_send_lat" ];

  bwIters = size:
    if size >= 1048576 then 300
    else if size >= 262144 then 800
    else 1000;

  bwTxDepth = size:
    if size >= 1048576 then 16
    else if size >= 262144 then 32
    else 128;

  # ib_send_{bw,lat} need a posted receive queue to absorb the SENDs.
  sendRxDepthOpt = bin:
    optionalAttrs (hasPrefix "ib_send_" bin) { "rx-depth" = 512; };

  bwOpts = { bin, size, qp, connection ? "RC" }: {
    inherit connection qp size;
    iters = bwIters size;
    "tx-depth" = bwTxDepth size;
  } // sendRxDepthOpt bin;

  latOpts = { bin, size, connection ? "RC" }: {
    inherit connection size;
    iters = 1000;
    qp = 1;
  } // sendRxDepthOpt bin;

  renderOption = flag: value:
    if value == null || value == false then [ ]
    else if value == true then [ "--${flag}" ]
    else [ "--${flag}" (toString value) ];

  renderOptions = options:
    concatMap (name: renderOption name options.${name}) (builtins.attrNames options);

  mkCase = { name, bin, options, directions ? [ "forward" ] }: {
    inherit name bin directions;
    argv = renderOptions options;
  };

  mkBwCases =
    { prefix, bins, sizes, qps, directions ? [ "forward" ], bidirectional ? false, connection ? "RC" }:
    let
      one = bin: qp: size: mkCase {
        name = "${prefix}.${bin}.size${toString size}.qps${toString qp}";
        inherit bin directions;
        options = bwOpts { inherit bin size qp connection; }
          // optionalAttrs bidirectional { bidirectional = true; };
      };
    in
    concatMap (bin: concatMap (qp: map (one bin qp) sizes) qps) bins;

  mkLatCases = { prefix, bins, sizes, directions ? [ "forward" ], connection ? "RC" }:
    let
      one = bin: size: mkCase {
        name = "${prefix}.${bin}.size${toString size}";
        inherit bin directions;
        options = latOpts { inherit bin size connection; };
      };
    in
    concatMap (bin: map (one bin) sizes) bins;

  mkReadOutCases = { prefix, sizes, outs, directions ? [ "forward" ] }:
    let
      bin = "ib_read_lat";
      one = size: outstanding: mkCase {
        name = "${prefix}.${bin}.size${toString size}.outs${toString outstanding}";
        inherit bin directions;
        options = latOpts { inherit bin size; } // { outs = outstanding; };
      };
    in
    concatMap (size: map (one size) outs) sizes;

  mkBwOdd = { tag, bin, size, qp ? 1, opts ? { } }: mkCase {
    name = "odd.${tag}.${bin}.size${toString size}.qps${toString qp}";
    inherit bin;
    options = bwOpts { inherit bin size qp; } // opts;
  };

  mkLatOdd = { tag, bin, size, opts ? { } }: mkCase {
    name = "odd.${tag}.${bin}.size${toString size}";
    inherit bin;
    options = latOpts { inherit bin size; } // opts;
  };

  # One case per interesting perftest flag. Filter with --only 'odd.*'
  # (or --only 'odd.<tag>*' to pick one). Whatever is in `opts` overrides
  # the standard bw/lat option set, so e.g. `opts = { connection = "UC"; }`
  # flips RC to UC for that one case.
  oddCases = [
    # Verb path: WQE / MR / SRQ posting variants.
    (mkBwOdd  { tag = "inline256";   bin = "ib_write_bw"; size = 256;   opts = { inline_size = 256; }; })
    # post_list requires iters % post_list == 0, so override the default 1000.
    (mkBwOdd  { tag = "postlist64";  bin = "ib_write_bw"; size = 4096;  opts = { iters = 1024; post_list = 64; }; })
    (mkBwOdd  { tag = "postlist64";  bin = "ib_send_bw";  size = 4096;  opts = { iters = 1024; post_list = 64; recv_post_list = 64; }; })
    (mkBwOdd  { tag = "mrperqp";     bin = "ib_write_bw"; size = 65536; qp = 4; opts = { mr_per_qp = true; }; })
    (mkBwOdd  { tag = "srq";         bin = "ib_send_bw";  size = 4096;  qp = 4; opts = { "use-srq" = true; }; })
    (mkBwOdd  { tag = "oldpostsend"; bin = "ib_write_bw"; size = 4096;  opts = { use_old_post_send = true; }; })

    # Completion path: CQ batching / polling.
    (mkBwOdd  { tag = "cqmod16";     bin = "ib_write_bw"; size = 1024;  opts = { "cq-mod" = 16; }; })
    (mkBwOdd  { tag = "cqepoll64";   bin = "ib_write_bw"; size = 65536; qp = 4; opts = { cqe_poll = 64; }; })
    (mkBwOdd  { tag = "txd16";       bin = "ib_write_bw"; size = 65536; qp = 8; opts = { "tx-depth" = 16; }; })
    (mkBwOdd  { tag = "txd32";       bin = "ib_write_bw"; size = 65536; qp = 8; opts = { "tx-depth" = 32; }; })
    (mkBwOdd  { tag = "txd64";       bin = "ib_write_bw"; size = 65536; qp = 8; opts = { "tx-depth" = 64; }; })

    # Timing / reporting: warmup, lat gap, CPU%.
    (mkLatOdd { tag = "warmup";      bin = "ib_write_lat"; size = 1024; opts = { perform_warm_up = true; }; })
    (mkLatOdd { tag = "latgap100";   bin = "ib_write_lat"; size = 64;   opts = { latency_gap = 100; }; })
    (mkBwOdd  { tag = "cpuutil";     bin = "ib_write_bw";  size = 65536; opts = { cpu_util = true; }; })

    # Connection types. The native transport is RC/UC only; UD only works
    # under rxe_eth0 / rxe_tb0 (override --dev to run it).
    (mkBwOdd  { tag = "uc";          bin = "ib_write_bw"; size = 4096;  opts = { connection = "UC"; }; })
    (mkBwOdd  { tag = "uc";          bin = "ib_send_bw";  size = 4096;  opts = { connection = "UC"; }; })
    (mkBwOdd  { tag = "ud";          bin = "ib_send_bw";  size = 1024;  opts = { connection = "UD"; }; })
  ];

  plan = {
    name = "full";
    description = "Full perftest matrix (bandwidth, latency, bidirectional, read-outstanding) plus odd-option probes. Filter with --only and override topology bits (--dev, --backend, --expect-rails, --expect-speed, --tag) for ad-hoc runs.";
    defaults = {
      server = topology.head;
      client = topology.worker;
      dev = topology.rdmaDev;
      gidIndex = topology.gidIndex;
      backend = moduleProfile.nativeBackend or "native";
      expectRails = topology.expect.nativeRails or null;
      expectSpeed = topology.expect.nativeRailSpeed or "any";
      timeout = 90;
      basePort = 18700;
      serverStartDelay = 0.8;
      directions = "from-plan";
      resultDir = "thunderbolt-ibverbs/results";
      copyTools = true;
      stopOnFail = false;
    };
    # RC matrix: full coverage for strix↔strix native (RC + UC + RDMA READ).
    # Apple Thunderbolt RDMA is UC-only, so all these cases fail at
    # `ibv_modify_qp` RTR when the peer is macOS. For Apple-compatible runs
    # filter to `--only '*.uc.*' --only 'odd.uc.*' --only 'odd.ud.*'` (or
    # drop these axes entirely on profiles that only see Apple peers).
    cases =
      mkBwCases { prefix = "bw";   bins = bwBins;  sizes = bwSizes;   qps = allQps; directions = bothDirections; }
      ++ mkBwCases { prefix = "bidi"; bins = bwBins; sizes = bidiSizes; qps = allQps; bidirectional = true; }
      ++ mkLatCases { prefix = "lat"; bins = latBins; sizes = latSizes; directions = bothDirections; }
      # readouts = ib_read_lat outstanding sweep; RDMA READ is RC-only and
      # therefore unavailable on Apple peers — keep this axis here for
      # strix↔strix and skip via `--only '*.uc.*'` when the peer is mac.
      ++ mkReadOutCases { prefix = "readouts"; sizes = [ 4096 65536 ]; outs = readOuts; }

      # UC matrix: Apple-compatible BW + LAT + bidi at the same sizes/QPs.
      # ib_read_bw / ib_read_lat are intentionally absent — RDMA READ
      # requires RC, which Apple's stack does not implement.
      ++ mkBwCases { prefix = "bw.uc";   bins = bwBinsUc; sizes = bwSizes;   qps = allQps; directions = bothDirections; connection = "UC"; }
      ++ mkBwCases { prefix = "bidi.uc"; bins = bwBinsUc; sizes = bidiSizes; qps = allQps; bidirectional = true; connection = "UC"; }
      ++ mkLatCases { prefix = "lat.uc"; bins = latBinsUc; sizes = latSizes; directions = bothDirections; connection = "UC"; }

      ++ oddCases;
  };

  planFile = pkgs.writeText "tbv-perftest-plan.json" (builtins.toJSON plan);

  runner = pkgs.writeShellApplication {
    name = "tbv-perftest";
    runtimeInputs = [ pkgs.coreutils pkgs.nix pkgs.openssh pkgs.python3 ];
    text = ''
      export TBV_RDMA_CORE=${rdma-core-usb4}
      export TBV_PERFTEST=${perftest}
      ${lib.optionalString (perftestDarwin != null) "export TBV_PERFTEST_DARWIN=${perftestDarwin}"}
      exec ${pkgs.python3}/bin/python3 -u ${runnerSrc} --plan ${planFile} "$@"
    '';
    meta = {
      description = plan.description;
      maintainers = with lib.maintainers; [ georgewhewell ];
      platforms = lib.platforms.linux;
    };
  };
in
{
  inherit plan runner;
  app = {
    type = "app";
    program = getExe runner;
  };
}
