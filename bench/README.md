# Benchmarks

This directory contains reproducible benchmark plans and compact historical
results for the Thunderbolt/USB4 RDMA transport.

The Nix flake exposes perftest runners that copy the matching `rdma-core-usb4`
and `perftest` builds to both hosts, run the selected matrix over SSH, and
write two outputs:

- `*.csv`: compact metrics suitable for checking into Git.
- `*.jsonl`: full raw per-case logs and telemetry snapshots; useful locally,
  but intentionally not checked in.

For a four-rail native run:

```sh
out=/tmp/tbv-native4rail
mkdir -p "$out"
nix run .#tbv-perftest-native4rail -- \
  --hosts strix-1,strix-2 \
  --directions both \
  --tag native4rail \
  --csv "$out/native4rail.csv" \
  --jsonl "$out/native4rail.jsonl"
```

Use `--list` or `--dry-run` to inspect the exact generated cases before
running them.

Historical checked-in result sets live under `bench/results/`.
