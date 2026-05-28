# perftest — `strix-noiommu-mbp-1x40g`

Asymmetric topology. Each endpoint contributes its own asserts to the dir
name, with a final link spec:

- **strix-noiommu**: one Strix Halo host (`strix-1` or `strix-2`), booted with
  `iommu=off` in the kernel cmdline.
- **mbp**: one MacBook Pro endpoint, talking to the strix over Thunderbolt.
- **1x40g**: one Thunderbolt/USB4 cable between this strix and the mac,
  negotiating 40 Gb/s aggregate (2 lanes × 20 Gb/s).

Strix-1 and strix-2 are tested individually — each one paired with the mac
via its own cable. Both sets of results land in this hw profile dir; the
strix identity goes into the CSV filename.

## Layout

```
strix-noiommu-mbp-1x40g/
├── perftest.md                              this file
├── perftest-tbverbs-strix1.csv -> result/   strix-1 ↔ mac, thunderbolt_ibverbs
├── perftest-tbverbs-strix2.csv -> result/   strix-2 ↔ mac, thunderbolt_ibverbs
└── result/                                  (gitignored; populated by `nix run`)
```

CSV symlinks dangle until a sweep populates `result/` per the recreate
commands. The runner also writes `kernel`, `module_sha256`, `iommu`, and
`server`/`client` columns per row, so the strix side identifies itself even
if the file is moved or renamed.

## Recreate

The runner ssh's each host to read `uname -s` and picks the linux or darwin
perftest closure baked into the flake. The darwin closure is built via your
local nix's remote-builder config, so a plain `nix run` is sufficient — no
manual staging.

```sh
out=bench/results/strix-noiommu-mbp-1x40g/result
mkdir -p "$out"

# strix-1 ↔ mac
nix run .#tbv-perftest -- \
  --hosts strix-1,<your-mac> \
  --no-rail-check \
  --base-port 19400 \
  --tag perftest-tbverbs-strix1 \
  --csv "$out/perftest-tbverbs-strix1.csv" \
  --jsonl "$out/perftest-tbverbs-strix1.jsonl"

# strix-2 ↔ mac
nix run .#tbv-perftest -- \
  --hosts strix-2,<your-mac> \
  --no-rail-check \
  --base-port 19500 \
  --tag perftest-tbverbs-strix2 \
  --csv "$out/perftest-tbverbs-strix2.csv" \
  --jsonl "$out/perftest-tbverbs-strix2.jsonl"
```

`--no-rail-check` because the rail-count assertion defaults to the
strix-strix native four-rail expectation; one cable yields 2 rails to the
mac. Tighten with `--expect-rails 2 --expect-speed 20Gb/s` once we know
what the apple-compatible backend reports here.

## Headline (initial UC probe, 2026-05-28)

First successful asymmetric strix-1 ↔ mbp run using
`--only 'odd.uc.*' --server-dev usb4_rdma4 --client-dev rdma_en2`.
Out of 4 directions tested:

| case | direction | result |
|---|---|---|
| `odd.uc.ib_write_bw.size4096.qps1` | forward (mbp writes → strix mem) | ✅ peak 70.32 Gb/s, avg 64.05 Gb/s |
| `odd.uc.ib_write_bw.size4096.qps1` | reverse (strix writes → mbp mem) | ❌ `Couldn't post send: qp 0 scnt=0` |
| `odd.uc.ib_send_bw.size4096.qps1` | forward (mbp sends → strix recv)  | ❌ strix times out: `Did not get Message for 120 Seconds, Total Received=0`, despite mbp reporting 70.17 Gb/s peak |
| `odd.uc.ib_send_bw.size4096.qps1` | reverse (strix sends → mbp recv)  | ❌ strix-side SEND path errors (`data_wr_path_send_error=190` of 192 posts) |

### Kernel module findings surfaced by these probes

1. **Linux → Apple WRITE not implemented.** `kernel/ibdev.c:tbv_post_apple_send`
   returns `-EOPNOTSUPP` for anything but `IB_WR_SEND`, so strix-side QPs on
   the apple-fa57 backend can never post `IB_WR_RDMA_WRITE`. This explains
   the reverse `ib_write_bw` failure.

2. **Apple → Linux SEND not seen on strix's RX.** During forward send_bw
   strix's `apple_rx_sof` / `data_rx_send` / `data_rx_*` all stay at 0 even
   though mbp reports 70 Gb/s of sends. Either the apple data RX handler
   isn't being entered on strix for SEND frames, or Apple converts sends to
   writes under the hood and mbp's perftest reports posted-bytes regardless.

3. **Linux → Apple SEND mostly errors at path send.** strix posted 192
   SEND WRs; `data_wr_path_send_error=190`, `data_wr_path_send=34`,
   `data_tx_posted=34`, `apple_sq_dequeued=2`. The TX path
   `tbv_path_send_marked_fill` rejects most posts before they reach the
   wire.

These three issues are the framework's headline finding so far — not bugs in
the bench code. Plain `ib_write_bw` forward (mbp → strix) works at ~70 Gb/s,
which is the one Apple-→Linux data path that's currently functional.
