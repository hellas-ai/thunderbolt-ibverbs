# perftest — `strix-2x40-noiommu`

Topology assertion in the dir name:

- **strix**: two Strix Halo hosts, paired strix-1 ↔ strix-2.
- **2x40**: two Thunderbolt/USB4 cables, each negotiating 2 lanes at 20 Gb/s
  (40 Gb/s aggregate per cable, 4 native `usb4_rdma` rails total).
- **noiommu**: both hosts booted with `iommu=off` in the kernel cmdline.

These are also captured at runtime as CSV columns (`kernel`, `module_sha256`,
`iommu`) on every row, so deviations are visible without trusting the dir name.

## Layout

```
strix-2x40-noiommu/
├── perftest.md                            this file
├── perftest-tbverbs.csv      -> result/   native thunderbolt_ibverbs
├── perftest-rxe_eth.csv      -> result/   RXE over br0.lan
├── perftest-rxe_tbnet.csv    -> result/   RXE over thunderbolt_net
└── result/                                (gitignored; populated by `nix run`)
```

CSV symlinks dangle on a fresh clone — they resolve once a sweep writes the
named CSV into `result/` per the recreate commands. Future suites (e.g.
`jaccl.md` + `jaccl-<transport>.csv` siblings) plug into the same topology
without changing the directory shape.

## Recreate

### tbverbs (native four-rail)

```sh
out=bench/results/strix-2x40-noiommu/result
mkdir -p "$out"
nix run .#tbv-perftest -- \
  --hosts strix-1,strix-2 \
  --expect-rails 4 --expect-speed 20Gb/s \
  --base-port 19000 \
  --tag perftest-tbverbs \
  --csv "$out/perftest-tbverbs.csv" \
  --jsonl "$out/perftest-tbverbs.jsonl"
```

### rxe_eth (RXE over br0.lan)

```sh
ssh root@strix-1 'modprobe rdma_rxe; rdma link del rxe_eth0 2>/dev/null; rdma link add rxe_eth0 type rxe netdev br0.lan'
ssh root@strix-2 'modprobe rdma_rxe; rdma link del rxe_eth0 2>/dev/null; rdma link add rxe_eth0 type rxe netdev br0.lan'

out=bench/results/strix-2x40-noiommu/result
mkdir -p "$out"
nix run .#tbv-perftest -- \
  --hosts strix-1,strix-2 \
  --dev rxe_eth0 --backend '' \
  --no-rail-check \
  --base-port 19100 \
  --tag perftest-rxe_eth \
  --csv "$out/perftest-rxe_eth.csv" \
  --jsonl "$out/perftest-rxe_eth.jsonl"
```

### rxe_tbnet (RXE over thunderbolt_net)

`thunderbolt_ibverbs` and `thunderbolt_net` can't both own the TB services at
the same time. Swap modules first; this will tear down the native rails.

```sh
for h in strix-1 strix-2; do
  ssh root@$h 'rdma link del rxe_eth0 2>/dev/null; modprobe -r thunderbolt_ibverbs; modprobe thunderbolt_net'
done

ssh root@strix-1 'ip addr add 10.42.0.1/24 dev thunderbolt0; ip addr add 10.43.0.1/24 dev thunderbolt1; ip link set thunderbolt0 up mtu 65520; ip link set thunderbolt1 up mtu 65520'
ssh root@strix-2 'ip addr add 10.42.0.2/24 dev thunderbolt0; ip addr add 10.43.0.2/24 dev thunderbolt1; ip link set thunderbolt0 up mtu 65520; ip link set thunderbolt1 up mtu 65520'

for h in strix-1 strix-2; do
  ssh root@$h 'rdma link add rxe_tb0 type rxe netdev thunderbolt0; rdma link add rxe_tb1 type rxe netdev thunderbolt1'
done

out=bench/results/strix-2x40-noiommu/result
mkdir -p "$out"
nix run .#tbv-perftest -- \
  --hosts strix-1,strix-2 \
  --dev rxe_tb0 --backend '' \
  --no-rail-check \
  --base-port 19200 \
  --tag perftest-rxe_tbnet \
  --csv "$out/perftest-rxe_tbnet.csv" \
  --jsonl "$out/perftest-rxe_tbnet.jsonl"
```

## Headline (2026-05-27 capture)

- **tbverbs**: 246 rows, 244 ok. Documented fails: `odd.srq.ib_send_bw.*`
  (SRQ + 4 QPs over native transport), `odd.ud.ib_send_bw.*` (UD is not a
  supported QP type, see `kernel/ibdev.c:1405`).
- **rxe_eth**: 246 rows, 245 ok. Documented fail: `odd.ud.ib_send_bw.*`
  (timed out at 90 s on RXE here too).
- **rxe_tbnet**: not captured — `thunderbolt_net` data plane failed to come
  up between the two hosts during the most recent module-swap attempt. Slot
  reserved.

### Bandwidth peaks (Gb/s)

| case | tbverbs |
|---|---:|
| `bidi.ib_write_bw.size1048576.qps8` combined | 46.06 |
| `bw.ib_write_bw.size1048576.qps8` forward | 10.78 |
| `bw.ib_write_bw.size1048576.qps8` reverse | 10.04 |
| `bw.ib_read_bw.size262144.qps8` reverse | 9.94 |
| `bw.ib_send_bw.size262144.qps8` reverse | 10.07 |

### Latency typical (µs), 64 B forward

| verb | tbverbs |
|---|---:|
| `ib_write_lat` | 7.41 |
| `ib_send_lat` | 8.34 |
| `ib_read_lat` | 15.37 |

After recreating any backend locally, the committed symlink resolves and:

```sh
python3 bench/summarize_perftest.py bench/results/strix-2x40-noiommu/perftest-*.csv
```
