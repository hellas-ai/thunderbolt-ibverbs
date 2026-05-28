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

## Mac-side prerequisites

The runner pushes its Linux closure to strix hosts via `nix-copy-closure`.
The mac side isn't substitutable across architectures, so you need a Darwin
`perftest` build already in the store and you point the runner at it via the
`--host-perftest` / `--host-rdma-core` switches. Set `--no-copy <mac-host>`
so the runner doesn't try to push to it. Set `--host-rdma-core <mac-host>=`
(empty) so the runner skips `LD_LIBRARY_PATH` wrapping — the Apple build uses
dyld dynamic-lookup against Apple's runtime libs.

```sh
mac_host=<your-mac-ssh-target>            # e.g. user@host
mac_perftest=$(nix build --no-link --print-out-paths .#packages.aarch64-darwin.perftest)
```

## Recreate (strix-1 ↔ mac)

```sh
out=bench/results/strix-noiommu-mbp-1x40g/result
mkdir -p "$out"
nix run .#tbv-perftest -- \
  --hosts strix-1,"$mac_host" \
  --no-rail-check \
  --no-copy "$mac_host" \
  --host-perftest "$mac_host=$mac_perftest" \
  --host-rdma-core "$mac_host=" \
  --base-port 19400 \
  --tag perftest-tbverbs-strix1 \
  --csv "$out/perftest-tbverbs-strix1.csv" \
  --jsonl "$out/perftest-tbverbs-strix1.jsonl"
```

## Recreate (strix-2 ↔ mac)

```sh
out=bench/results/strix-noiommu-mbp-1x40g/result
mkdir -p "$out"
nix run .#tbv-perftest -- \
  --hosts strix-2,"$mac_host" \
  --no-rail-check \
  --no-copy "$mac_host" \
  --host-perftest "$mac_host=$mac_perftest" \
  --host-rdma-core "$mac_host=" \
  --base-port 19500 \
  --tag perftest-tbverbs-strix2 \
  --csv "$out/perftest-tbverbs-strix2.csv" \
  --jsonl "$out/perftest-tbverbs-strix2.jsonl"
```

Notes:
- `--no-rail-check` because the rail-count assertion defaults to the
  strix-strix native four-rail expectation; one cable yields 2 rails to the
  mac. Tighten with `--expect-rails 2 --expect-speed 20Gb/s` once we know
  what the apple-compatible backend reports here.
- Hostnames are passed through to ssh as-is; use `user@host` if your strix
  or mac account isn't ssh's default. Resolution is via your normal DNS.

## Headline

Not captured yet — slot reserved.
