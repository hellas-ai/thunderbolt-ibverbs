# 2026-05-27 IOMMU-off Rerun (unified plan)

Re-run of the perftest matrix on the same two-host Strix Halo / Thunderbolt
setup as `2026-05-27-iommu-off-lowlevel-sweep/`, this time driven by the
collapsed `tbv-perftest` runner. The single plan runs all the bandwidth /
bidirectional / latency / read-outstanding / odd-option cases per backend
(see `bench/perftest.nix`).

Cluster identity, captured by the runner at startup and into every CSV row:

- kernel: `7.1.0-rc1`
- module sha256 (first 16 chars of the loaded `.ko`): `7d02444efccefc78`
- module taint: `O` (out-of-tree)
- iommu: `off` (from `/proc/cmdline`)
- 4 native rails per host at 20 Gb/s

This boot only had `usb4_rdma{0,1,5,6}` rails plus `rxe_eth0` available — no
`thunderbolt0` / `thunderbolt1` netdevs, so the prior `rxe-tb0` / `rxe-tb1`
sweeps are not reproduced here.

No CSVs or JSONL are checked in. `data/*.csv` are committed symlinks pointing
at `../result/<name>.csv`; the `result/` sibling is gitignored and populated
by the recreate commands below. The headline numbers from this run are
captured in the comparison table further down.

## Recreate native four-rail

```sh
out=bench/results/2026-05-27-iommu-off-rerun/result
mkdir -p "$out"
nix run .#tbv-perftest -- \
  --hosts strix-1,strix-2 \
  --expect-rails 4 --expect-speed 20Gb/s \
  --base-port 19000 \
  --tag native4rail-iommu-off \
  --csv "$out/native4rail.csv" \
  --jsonl "$out/native4rail.jsonl"
```

## Recreate RXE-over-LAN

```sh
ssh root@strix-1 'modprobe rdma_rxe; rdma link del rxe_eth0 2>/dev/null; rdma link add rxe_eth0 type rxe netdev br0.lan'
ssh root@strix-2 'modprobe rdma_rxe; rdma link del rxe_eth0 2>/dev/null; rdma link add rxe_eth0 type rxe netdev br0.lan'

out=bench/results/2026-05-27-iommu-off-rerun/result
mkdir -p "$out"
nix run .#tbv-perftest -- \
  --hosts strix-1,strix-2 \
  --dev rxe_eth0 --backend '' \
  --no-rail-check \
  --base-port 19100 \
  --tag rxe-ethernet-iommu-off \
  --csv "$out/rxe-ethernet.csv" \
  --jsonl "$out/rxe-ethernet.jsonl"
```

## Summary

Once you've recreated the CSVs locally, the committed symlinks resolve and you
can summarize:

```sh
python3 bench/summarize_perftest.py \
  bench/results/2026-05-27-iommu-off-rerun/data/*.csv
```

Headline:

- Native four-rail: 246 rows, 244 ok, 2 documented-fail probes
  (`odd.srq.ib_send_bw.*` — SRQ + 4 QPs over native transport,
  `odd.ud.ib_send_bw.*` — UD is not a supported QP type, see
  `kernel/ibdev.c:1405`).
- RXE over Ethernet bridge: 246 rows, 245 ok, 1 documented-fail probe
  (`odd.ud.ib_send_bw.*` times out at 90 s on RXE here too).

Like-for-like vs the prior sweep:

| case | prior | rerun |
|---|---:|---:|
| native `bw.ib_write_bw.size1048576.qps8` forward | 10.10 Gb/s | 10.78 Gb/s |
| native `bw.ib_write_bw.size1048576.qps8` reverse | 9.80 Gb/s  | 10.04 Gb/s |
| native `bidi.ib_write_bw.size1048576.qps8` combined | 48.34 Gb/s | 46.06 Gb/s |
| native `bw.ib_read_bw.size262144.qps8` reverse | 10.29 Gb/s | 9.94 Gb/s |
| native `bw.ib_send_bw.size262144.qps8` reverse | 10.30 Gb/s | 10.07 Gb/s |
| native 64 B `ib_write_lat` forward (typical) | ~6.9 µs | 7.41 µs |
| native 64 B `ib_read_lat` forward (typical) | ~13.6 µs | 15.37 µs |
| native 64 B `ib_send_lat` forward (typical) | ~7.6 µs | 8.34 µs |

Unidirectional bandwidth, bidirectional bandwidth, and short-message latency
all match the prior sweep within run-to-run variance.

## What's new in this dataset vs the prior sweep

- **One unified plan** instead of four separate runners. Same `tbv-perftest`
  command line for every backend; topology overrides at the CLI.
- **Odd-option probes** (`odd.*`): one case per non-default perftest flag
  (inline_size, post_list, mr_per_qp, use-srq, use_old_post_send, cq-mod,
  cqe_poll, perform_warm_up, latency_gap, cpu_util, UC, UD). Two of these
  surface transport limits we now have data points for (SRQ + UD on native).
- **Identity columns** (`kernel`, `module_sha256`, `iommu`) on every row so
  you can grep a CSV without going back to the run log.
