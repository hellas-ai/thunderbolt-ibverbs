# 2026-05-27 IOMMU-off Low-Level Sweep

Two Strix Halo hosts were booted with `iommu=off` and connected by two
Thunderbolt/USB4 cables, giving four native `usb4_rdma` rails at 20 Gb/s each.

No CSVs or JSONL are checked in. `data/*.csv` are committed symlinks pointing
at `../result/<name>.csv`; the `result/` sibling is gitignored and populated
by the recreate commands below. The runner used here is the unified
`tbv-perftest` (post-collapse); the originally-recorded values from this run
window are in the headline summary at the bottom.

## Recreate native four-rail

```sh
out=bench/results/2026-05-27-iommu-off-lowlevel-sweep/result
mkdir -p "$out"
nix run .#tbv-perftest -- \
  --hosts strix-1,strix-2 \
  --expect-rails 4 --expect-speed 20Gb/s \
  --base-port 19000 \
  --tag native4rail-iommu-off \
  --csv "$out/native4rail.csv" \
  --jsonl "$out/native4rail.jsonl"
```

## Recreate RXE-over-Ethernet

```sh
ssh root@strix-1 'modprobe rdma_rxe; rdma link del rxe_eth0 2>/dev/null; rdma link add rxe_eth0 type rxe netdev br0.lan'
ssh root@strix-2 'modprobe rdma_rxe; rdma link del rxe_eth0 2>/dev/null; rdma link add rxe_eth0 type rxe netdev br0.lan'

out=bench/results/2026-05-27-iommu-off-lowlevel-sweep/result
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

## Recreate RXE-over-TB-net

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

Then sweep each TB-net RXE device:

```sh
out=bench/results/2026-05-27-iommu-off-lowlevel-sweep/result
mkdir -p "$out"

nix run .#tbv-perftest -- \
  --hosts strix-1,strix-2 \
  --dev rxe_tb0 --backend '' \
  --no-rail-check \
  --base-port 19200 \
  --tag rxe-tb0-iommu-off \
  --csv "$out/rxe-tb0.csv" \
  --jsonl "$out/rxe-tb0.jsonl"

nix run .#tbv-perftest -- \
  --hosts strix-1,strix-2 \
  --dev rxe_tb1 --backend '' \
  --no-rail-check \
  --base-port 19300 \
  --tag rxe-tb1-iommu-off \
  --csv "$out/rxe-tb1.csv" \
  --jsonl "$out/rxe-tb1.jsonl"
```

## Summary

Once you've recreated the CSVs locally, the committed symlinks resolve and
the summarizer reproduces this table:

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
here, but the aggregate numbers are useful context.

The `64 in / 128 out` screen used matched vLLM shapes while varying
`--max-num-seqs`. Here, `n` is the total `--num-prompts`, while max seqs is
the scheduler's active sequence cap. RDMA leads most clearly at low active
sequence counts and the advantage narrows as vLLM batches more work:

| transport | shape | n | max seqs | output tok/s | total tok/s | req/s | vs TB-net |
|---|---|---:|---:|---:|---:|---:|---:|
| TB-net Socket over thunderbolt0 | random 64 in / 128 out | 16 | 1 | 9.92 | 14.88 | 0.078 | baseline |
| native RDMA, 4 HCA, QPS=2 split=1 | random 64 in / 128 out | 16 | 1 | 12.90 | 19.35 | 0.101 | +30.0% |
| TB-net Socket over thunderbolt0 | random 64 in / 128 out | 16 | 2 | 14.41 | 21.62 | 0.113 | baseline |
| native RDMA, 4 HCA, QPS=2 split=1 | random 64 in / 128 out | 16 | 2 | 17.22 | 25.82 | 0.134 | +19.5% |
| TB-net Socket over thunderbolt0 | random 64 in / 128 out | 16 | 4 | 21.27 | 31.90 | 0.166 | baseline |
| native RDMA, 4 HCA, QPS=2 split=1 | random 64 in / 128 out | 16 | 4 | 23.73 | 35.60 | 0.185 | +11.6% |
| TB-net Socket over thunderbolt0 | random 64 in / 128 out | 32 | 8 | 31.05 | 46.57 | 0.243 | baseline |
| native RDMA, 4 HCA, QPS=2 split=1 | random 64 in / 128 out | 32 | 8 | 32.75 | 49.13 | 0.256 | +5.5% |

This explains why the high-concurrency `64 in / 16 out` shape below looks
close: vLLM has enough active sequences to amortize transport overhead.

| transport | shape | max seqs | output tok/s | total tok/s | req/s |
|---|---|---:|---:|---:|---:|
| native RDMA, 4 HCA | random 64 in / 16 out, n=16 | 16 | 29.37 | 146.86 | 1.84 |
| TB-net Socket over thunderbolt0 | random 64 in / 16 out, n=16 | 64 | 30.87 | 154.34 | 1.93 |
| native RDMA, 4 HCA | random 64 in / 16 out, n=64 | 64 | 68.03 | 340.16 | 4.25 |
| TB-net Socket over thunderbolt0 | random 64 in / 16 out, n=64 | 64 | 62.42 | 312.11 | 3.90 |
| native RDMA, 4 HCA, QPS=2 split=1 | random 64 in / 16 out, n=128 | 64 | 68.81 | 344.04 | 4.30 |
| TB-net Socket over thunderbolt0 | random 64 in / 16 out, n=128 | 64 | 64.03 | 320.13 | 4.00 |

The TB-net runs confirmed `NCCL_NET=Socket`, `NCCL_IB_DISABLE=1`, and NCCL
channels using `NET/Socket/0` over `thunderbolt0`.
