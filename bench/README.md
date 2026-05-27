# Benchmarks

This directory contains the reproducible benchmark plan and compact historical
results for the Thunderbolt/USB4 RDMA transport.

The Nix flake exposes a single perftest runner (`tbv-perftest`) that copies the
matching `rdma-core-usb4` and `perftest` builds to both hosts, runs the matrix
over SSH, and writes two outputs:

- `*.csv`: compact metrics suitable for checking into Git.
- `*.jsonl`: full raw per-case logs and telemetry snapshots; useful locally,
  but intentionally not checked in.

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
