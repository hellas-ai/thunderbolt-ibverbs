# 2026-05-27 IOMMU-off Low-Level Sweep

Two Strix Halo hosts were booted with `iommu=off` and connected by two
Thunderbolt/USB4 cables, giving four native `usb4_rdma` rails at 20 Gb/s each.

Only compact CSV summaries are checked in. The original JSONL telemetry logs
were 25+ MB and are intentionally excluded; use the commands below to recreate
them.

## Recreate Native RDMA Artifacts

```sh
out=/tmp/tbv-iommu-off-sweep/native-rdma
mkdir -p "$out"
nix run .#tbv-perftest-native4rail -- \
  --hosts strix-1,strix-2 \
  --directions both \
  --tag native4rail-iommu-off \
  --timeout 90 \
  --base-port 19000 \
  --csv "$out/native4rail.csv" \
  --jsonl "$out/native4rail.jsonl"
```

## Recreate RXE-over-Ethernet Artifacts

Create the RXE link on both hosts first:

```sh
ssh root@strix-1 'rdma link del rxe_eth0 2>/dev/null || true; rdma link add rxe_eth0 type rxe netdev br0.lan'
ssh root@strix-2 'rdma link del rxe_eth0 2>/dev/null || true; rdma link add rxe_eth0 type rxe netdev br0.lan'
```

Then run:

```sh
out=/tmp/tbv-iommu-off-sweep/rxe-ethernet
mkdir -p "$out"
nix run .#tbv-perftest-rxe-ethernet -- \
  --hosts strix-1,strix-2 \
  --directions both \
  --tag rxe-ethernet-iommu-off \
  --timeout 90 \
  --base-port 19100 \
  --csv "$out/rxe-ethernet.csv" \
  --jsonl "$out/rxe-ethernet.jsonl"
```

## Recreate RXE-over-TB-net Artifacts

The native `thunderbolt_ibverbs` module and `thunderbolt_net` cannot own the
same Thunderbolt services at the same time. Switch to TB-net first:

```sh
ssh root@strix-1 'rdma link del rxe_eth0 2>/dev/null || true; modprobe -r thunderbolt_ibverbs; modprobe thunderbolt_net'
ssh root@strix-2 'rdma link del rxe_eth0 2>/dev/null || true; modprobe -r thunderbolt_ibverbs; modprobe thunderbolt_net'

ssh root@strix-1 'ip addr add 10.42.0.1/24 dev thunderbolt0 2>/dev/null || true; ip addr add 10.43.0.1/24 dev thunderbolt1 2>/dev/null || true; ip link set thunderbolt0 up mtu 65520; ip link set thunderbolt1 up mtu 65520'
ssh root@strix-2 'ip addr add 10.42.0.2/24 dev thunderbolt0 2>/dev/null || true; ip addr add 10.43.0.2/24 dev thunderbolt1 2>/dev/null || true; ip link set thunderbolt0 up mtu 65520; ip link set thunderbolt1 up mtu 65520'

ssh root@strix-1 'rdma link del rxe_tb0 2>/dev/null || true; rdma link del rxe_tb1 2>/dev/null || true; rdma link add rxe_tb0 type rxe netdev thunderbolt0; rdma link add rxe_tb1 type rxe netdev thunderbolt1'
ssh root@strix-2 'rdma link del rxe_tb0 2>/dev/null || true; rdma link del rxe_tb1 2>/dev/null || true; rdma link add rxe_tb0 type rxe netdev thunderbolt0; rdma link add rxe_tb1 type rxe netdev thunderbolt1'
```

Then run each TB-net RXE device:

```sh
out=/tmp/tbv-iommu-off-sweep/rxe-tbnet
mkdir -p "$out"
nix run .#tbv-perftest-rxe-tbnet -- \
  --hosts strix-1,strix-2 \
  --directions both \
  --dev rxe_tb0 \
  --tag rxe-tb0-iommu-off \
  --timeout 90 \
  --base-port 19200 \
  --csv "$out/rxe-tb0.csv" \
  --jsonl "$out/rxe-tb0.jsonl"

nix run .#tbv-perftest-rxe-tbnet -- \
  --hosts strix-1,strix-2 \
  --directions both \
  --dev rxe_tb1 \
  --tag rxe-tb1-iommu-off \
  --timeout 90 \
  --base-port 19300 \
  --csv "$out/rxe-tb1.csv" \
  --jsonl "$out/rxe-tb1.jsonl"
```

## Summary

Regenerate this table from the checked-in CSV data:

```sh
python3 bench/summarize_perftest.py \
  bench/results/2026-05-27-iommu-off-lowlevel-sweep/data/*.csv
```

Headline results:

- Native four-rail RDMA: 186 rows, 0 failures.
- RXE over Ethernet: 84 rows, 0 failures.
- RXE over TB-net: 84 rows per TB interface, 0 failures.
- Native `ib_write_bw` peaked at 48.32 Gb/s forward and 46.77 Gb/s reverse.
- Native 64B latency was about 6.9 us for WRITE, 7.6 us for SEND, and
  13.6-15.2 us for READ.
- RXE-over-TB-net showed a fixed-step latency shape: about 65 us for
  SEND/WRITE and 130 us for READ.

## MiniMax Application Snapshot

The same boot window was used for short MiniMax M2.7 AWQ TP=2 generation
checks through `nix-strix-halo`. These application logs are not checked in
here, but the aggregate numbers are useful context:

| transport | shape | output tok/s | total tok/s | req/s |
|---|---|---:|---:|---:|
| native RDMA, 4 HCA, QPS=2 split=1 | random 64 in / 16 out, n=128 | 68.81 | 344.04 | 4.30 |
| TB-net Socket over thunderbolt0 | random 64 in / 16 out, n=128 | 64.03 | 320.13 | 4.00 |

The TB-net run confirmed `NCCL_NET=Socket`, `NCCL_IB_DISABLE=1`, and NCCL
channels using `NET/Socket/0` over `thunderbolt0`.
