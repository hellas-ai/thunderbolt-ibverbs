# Benchmarks

## How it works

Each suite (currently just `perftest`) is a Nix-defined case list — for
`tbv-perftest` see `perftest.nix`. `nix run .#tbv-perftest` copies the matching
`rdma-core-usb4` and `perftest` builds to both hosts, runs every case over SSH,
and writes a `--csv` summary plus a `--jsonl` per-case telemetry log. Run-time
state (kernel, loaded module sha256, IOMMU setting, rail counts) is captured
into the CSV row and a startup banner so a stray file is self-describing.

## How results are stored

```
bench/results/<hw-profile>/                e.g. strix-2p-noiommu-2x40g/
├── <suite>.md                             perftest.md — committed report
├── <suite>-<transport>.csv → result/…     committed symlink, dangling on a fresh clone
└── result/                                gitignored; populated by the recreate command
```

The hw-profile dir name asserts the topology — endpoints, their kernel/iommu
flags, and the link spec. Two shapes:

- **Symmetric**: `<endpoint>-Np-<asserts>-<link>` when both sides are the same,
  e.g. `strix-2p-noiommu-2x40g` (two strix peers, both iommu=off, 2×40g cables).
- **Asymmetric**: `<endpoint>-<asserts>-<endpoint>-<asserts>-<link>` when sides
  differ, e.g. `strix-noiommu-mbp-1x40g` (one strix iommu=off, one mac, 1×40g
  cable). The CSV filename grows a per-side disambiguator when needed, like
  `perftest-tbverbs-strix1.csv` and `perftest-tbverbs-strix2.csv`.

CSVs live as symlinks pointing into a sibling `result/` that's not checked in.
The `.md` holds recreate commands and headline numbers from the last capture.
Future suites (`jaccl.md` + `jaccl-<transport>.csv`) slot in as siblings without
changing the shape. The runner also writes `kernel` / `module_sha256` / `iommu`
columns into every row, so a stray CSV self-describes even if the dir name lies.

The plan is built in `perftest.nix` as five blocks of cases, each prefixed by
kind so `--only` patterns can target a slice cleanly:

- `bw.*` — bandwidth sweep (`ib_{write,read,send}_bw` × sizes × QPs, both directions)
- `bidi.*` — bidirectional bandwidth
- `lat.*` — latency sweep
- `readouts.*` — `ib_read_lat` varying outstanding RDMA READs
- `odd.*` — one case per interesting perftest flag (`inline_size`, `post_list`,
  `mr_per_qp`, `use-srq`, `use_old_post_send`, `cq-mod`, `cqe_poll`,
  `perform_warm_up`, `latency_gap`, `cpu_util`, plus UC / UD connection types)

The Thunderbolt transport supports RC and UC only; `odd.ud.*` only runs
correctly under `--dev rxe_eth0` or `--dev rxe_tb0`.

## Running the full suite

```sh
out=/tmp/tbv-full
mkdir -p "$out"
nix run .#tbv-perftest -- \
  --hosts strix-1,strix-2 \
  --directions both \
  --tag full \
  --csv "$out/full.csv" \
  --jsonl "$out/full.jsonl"
```

Use `--list` or `--dry-run` to inspect the generated cases before running them.

## Ad-hoc subsets

Filter cases with one or more `--only` fnmatch patterns:

```sh
# Smoke-equivalent: a couple of small BW + a couple of LAT cases
nix run .#tbv-perftest -- --hosts strix-1,strix-2 \
  --only 'bw.*size4096.qps1' --only 'lat.*size64' --only 'lat.*size4096' \
  --tag smoke --csv "$out/smoke.csv" --jsonl "$out/smoke.jsonl"

# Read-outstanding sweep only
nix run .#tbv-perftest -- --hosts strix-1,strix-2 \
  --only 'readouts.*' --timeout 120 --expect-rails 1 \
  --tag read-outs --csv "$out/read-outs.csv" --jsonl "$out/read-outs.jsonl"

# Four-rail native expectations
nix run .#tbv-perftest -- --hosts strix-1,strix-2 \
  --expect-rails 4 --expect-speed 20Gb/s \
  --tag native4rail --csv "$out/native4rail.csv" --jsonl "$out/native4rail.jsonl"

# RXE over the LAN bridge
nix run .#tbv-perftest -- --hosts strix-1,strix-2 \
  --dev rxe_eth0 --backend '' --expect-rails 0 --expect-speed any \
  --tag rxe-ethernet --csv "$out/rxe-ethernet.csv" --jsonl "$out/rxe-ethernet.jsonl"

# RXE over thunderbolt_net
nix run .#tbv-perftest -- --hosts strix-1,strix-2 \
  --dev rxe_tb0 --backend '' --expect-rails 0 --expect-speed any \
  --tag rxe-tbnet --csv "$out/rxe-tbnet.csv" --jsonl "$out/rxe-tbnet.jsonl"
```

Historical checked-in result sets live under `bench/results/`.
