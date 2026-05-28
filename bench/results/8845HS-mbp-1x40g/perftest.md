# perftest — `8845HS-mbp-1x40g`

Asymmetric topology between a bare-metal Linux 8845HS (non-Strix AMD USB4
NHI) and a MacBook Pro talking Apple Thunderbolt RDMA. This is the
reference Mac↔Linux pair — the 2026-05-17 e14/e17/e19 results were on the
same physical link (8845HS VFIO VM at the time; this is the same hardware
running the closure natively now).

- **8845HS**: AMD 8845HS / NHI controller separate from the Strix Halo
  family. NixOS host with the thunderbolt_ibverbs module loaded with
  `profile=mac_compat, tbnet_identity=stock_proxy, roce_netdev=ardma0,
  lanes=1, native_data=0, apple_data=1`. Stock `thunderbolt_net` brings
  up `thunderbolt0` at `10.0.3.2/24`; the dummy `ardma0` carries the
  IPv4-mapped RDMA GID at `10.0.3.44/32`.
- **mbp**: Apple Silicon (Mac16,5 / M4 Max) running macOS 26.5 with
  `rdma_ctl enable` (Apple TN3205). Active TB cable is mbp's `en3` /
  `rdma_en3`, IP pinned at `10.0.3.3/24`.
- **1x40g**: one Thunderbolt cable between this 8845HS host and the mac,
  reporting `link_speed=20Gb/s link_width=0x2` (40 Gb/s aggregate, 2
  lanes × 20 Gb/s). Apple's stack uses UC only; RC is not supported.

## Layout

```
8845HS-mbp-1x40g/
├── perftest.md                            this file
├── perftest-tbverbs-8845HS.csv -> result/ 8845HS ↔ mbp, thunderbolt_ibverbs
└── result/                                (gitignored; populated by `nix run`)
```

The CSV symlink dangles until a sweep populates `result/` per the recreate
command. The runner also writes `kernel`, `module_sha256`, `iommu`, and
`server`/`client` columns per row, so the 8845HS side identifies itself
even if the file is moved or renamed.

## Recreate

Apple's Thunderbolt RDMA is UC-only — `ibv_modify_qp` returns `-ENOTSUPP`
on RTR when the perftest binary sets `Connection_type: RC`. The full
plan still generates the RC matrix (it's exercised by strix↔strix native);
for this profile we filter to UC-compatible cases only:

```sh
out=bench/results/8845HS-mbp-1x40g/result
mkdir -p "$out"

# Mac is on wifi DHCP and renames between sessions; resolve at
# recreate time, or set MBP_IP=<your-mac-ip> in the environment.
MBP_IP=$(getent hosts mbp 2>/dev/null | awk '{print $1}')
MBP_IP=${MBP_IP:?set MBP_IP to your mac LAN ip}

nix run .#tbv-perftest -- \
  --hosts 8845HS,"$MBP_IP" \
  --no-rail-check \
  --base-port 19700 \
  --tag perftest-tbverbs-8845HS \
  --csv "$out/perftest-tbverbs-8845HS.csv" \
  --jsonl "$out/perftest-tbverbs-8845HS.jsonl" \
  --server-dev usb4_rdma4 \
  --client-dev rdma_en3 \
  --only '*.uc.*' --only 'odd.uc.*' --only 'odd.ud.*'
```

`--no-rail-check` because the rail-count assertion defaults to the
strix-strix native four-rail expectation; one cable to the mac yields one
apple rail. `--only '*.uc.*'` selects the apple-compatible BW/LAT/bidi
matrix; `--only 'odd.uc.*' --only 'odd.ud.*'` adds the connection-type
probes that exist outside the matrix.

## Apple verb support reference

| Verb | Connection | Apple support | Notes |
|---|---|---|---|
| `IBV_WR_SEND` | UC | ✅ end-to-end (verified `--check` bit-perfect) | The proven path. |
| `IBV_WR_SEND` | RC | ❌ — `ibv_modify_qp` RTR `-ENOTSUPP` | RC not implemented in `AppleThunderboltRDMA.kext`. |
| `IBV_WR_RDMA_WRITE` | UC | ⚠️ — mbp posts wire frames (8845HS sees `data_rx_completed` tick) but our module's apple RX path has no rkey/raddr dispatch, so bytes never land in the receiver's MR | `uc_write_verify` shows receiver buffer unchanged. |
| `IBV_WR_RDMA_WRITE` | RC | ❌ — same RTR `-ENOTSUPP` as RC SEND. |
| `IBV_WR_RDMA_READ` | RC | ❌ — RDMA READ requires RC, which Apple does not implement. |
| UD (`ib_send_bw -c UD`) | UD | ❌ — Apple does not expose UD QPs on `rdma_en*` devices. |

## Headline (2026-05-28)

87 UC cases × default plan directions = 147 rows total. 12 cases reported
`status=ok`. The BW numbers below are **perftest's internal measurement,
which on the Apple path overstates real wire delivery** — see "BW number
caveat" further down for why and what to trust instead.

Direction key: `fwd` = mbp client → 8845HS server (mac sends); `rev` =
8845HS client → mbp server (linux sends). The reverse rows have
`rxd=n/a` because the server-side counters live on mbp (Darwin, no
debugfs) — that's a runner reporting gap, not a delivery gap. Both
directions deliver real bit-perfect data; see the byte-verify probe
below.

| case | dir | perftest peak | perftest avg | lat avg | 8845HS rxd | note |
|---|---|---|---|---|---|---|
| `bw.uc.ib_write_bw.size1024.qps1` | fwd | 20.18 Gb/s | 13.19 Gb/s | — | 1000 | wire frames arrive at 8845HS but our module's apple RX has no WRITE dispatch — bytes don't actually land in receiver MR (`uc_write_verify` confirms). Treat write_bw "ok" rows as frame-count proof only. |
| `bw.uc.ib_write_bw.size4096.qps1` | fwd | 70.47 | 67.57 | — | 1000 | same as above |
| `bw.uc.ib_write_bw.size16384.qps1` | fwd | 29.04 | 29.00 | — | 4000 | same; 4 frames/msg × 1000 iters |
| `odd.uc.ib_write_bw.size4096.qps1` | fwd | 61.95 | 55.12 | — | 1000 | same as above |
| `bw.uc.ib_send_bw.size1024.qps1` | fwd | 18.33 | 16.84 | — | 1000 | SEND lands in posted RX WRs; bit-perfect content verified via `uc_oneway --check` separately. |
| `bw.uc.ib_send_bw.size4096.qps1` | fwd | 64.63 | 57.56 | — | 1000 | bit-perfect verified |
| `bw.uc.ib_send_bw.size16384.qps1` | fwd | 29.05 | 29.03 | — | 4000 | bit-perfect verified |
| `odd.uc.ib_send_bw.size4096.qps1` | fwd | 90.02 | 79.72 | — | 1000 | bit-perfect verified |
| `bidi.uc.ib_send_bw.size4096.qps1` | fwd | 191.18 | 61.60 | — | 1000 | both peak and avg are above link rate; perftest BW math is unreliable here. |
| `lat.uc.ib_send_lat.size4096` | fwd | — | — | 10.93 µs | 1000 | real round-trip; lat is honest because it measures completion deltas. |
| `bw.uc.ib_send_bw.size4096.qps1` | rev | 2.88 | 2.52 | — | n/a | bit-perfect verified separately (mac side reports 1000/1000 with `--check` OK). |
| `lat.uc.ib_send_lat.size4096` | rev | — | — | 9.59 µs | n/a | real round-trip |

## BW number caveat

A 1 cable × 2 lane × 20 Gb/s link tops out at 40 Gb/s aggregate. The
matrix above reports `bw.uc.ib_send_bw.size4096.qps1` forward at 57.56
Gb/s avg / 64.63 Gb/s peak — physically impossible. Reproducing the math:
`iters × size / msg_rate × 1000` = 1000 × 4096 / 1.756 Mpps = 569 µs of
internal "measured time" for 4 MB of payload, which would need ≥ 800 µs
on a 40 Gb/s wire. perftest computes BW as `iters × msg_size / (last
CQE time − first WR post time)`. Apple's stack fires CQEs when WRs
*leave the SQ ring* for the NHI DMA engine, not when bytes are actually
flushed to the wire, so the timer captures sender-side completion bursts
and undercounts wire time by a large multiplier.

Wall-clock truth, measured with `uc_oneway --check` at the same matrix
point (1000 × 4096 B, qps=1):

```
FORWARD  mac → 8845HS: 1000/1000 messages, --check PASS, rate 6.45 Gb/s, elapsed 0.005 s
REVERSE  8845HS → mac: 1000/1000 messages, --check PASS, rate 1.76 Gb/s, elapsed 0.019 s
```

Both directions deliver real bit-perfect data. Real rates are ~1/10 of
perftest's reported numbers. The asymmetry (forward 6.45 Gb/s vs reverse
1.76 Gb/s) is genuine — our apple TX path is bound by
`apple_tx_max_inflight_wr=1, apple_tx_max_inflight_frames=2`, so 8845HS
(linux-sender) serializes harder than mbp (mac-sender) which has Apple's
HW-managed SQ window.

Failure clusters:

- **`qps > 1` everywhere**: apple TX SQ window is hardcoded to
  `apple_tx_max_inflight_wr=1, apple_tx_max_inflight_frames=2`, so any
  case with >1 QP serialises so hard the SQ ring stalls out. All
  `*.qps2 / qps4 / qps8` runs failed.
- **`bw.uc.ib_write_bw.size >= 65536`**: WRITE messages larger than ~64 KB
  fragment into more frames than the apple SQ ring can drain in the test
  window. Failure mode is `Couldn't post send: qp 0 scnt=0` on the sender.
- **All `lat.uc.ib_write_lat.*`**: WRITE is a ping-pong here, and our
  module's apple RX path doesn't dispatch WRITE → no peer reply → both
  sides time out at the 90 s test cap. Maps directly to the WRITE
  receive gap; once that's implemented these should pass.
- **All `bidi.uc.*` at `qps > 1` or size ≥ 65536**: same apple SQ window
  saturation; bidi has both endpoints fighting for the 1-WR window.
- **`odd.ud.*`**: Apple's `rdma_en*` doesn't expose UD QPs; expected.

What's reliable today on this path (mbp → 8845HS, forward direction
only, qps=1, sizes ≤ 16 KB):

- UC SEND end-to-end, including bit-perfect content (verified with
  `uc_oneway --check`).
- UC SEND round-trip latency ~10 µs at 4 KiB.

What's not working yet (kernel-module gaps, not Apple gaps):

- UC WRITE receive — frames arrive at our apple RX ring but aren't
  dispatched as WRITE (no rkey/raddr decode). See `kernel/ibdev.c`
  `tbv_ibdev_rx_apple_frame` — every frame is unconditionally routed to
  the next posted RX WR (SEND semantics).
- Apple SQ window beyond 1 WR / 2 frames — would unlock higher QP counts
  and larger messages. Conservative defaults chosen in 2026-05-17 after
  the deferred-RX overflow fix; might be safe to raise.
