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

## Recreate (strix-1 ↔ mac)

```sh
out=bench/results/strix-noiommu-mbp-1x40g/result
mkdir -p "$out"
nix run .#tbv-perftest -- \
  --hosts strix-1,mbp \
  --no-rail-check \
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
  --hosts strix-2,mbp \
  --no-rail-check \
  --base-port 19500 \
  --tag perftest-tbverbs-strix2 \
  --csv "$out/perftest-tbverbs-strix2.csv" \
  --jsonl "$out/perftest-tbverbs-strix2.jsonl"
```

Notes:
- `--no-rail-check` because the rail-count assertion in the plan defaults to
  the strix-strix native four-rail expectation; one cable yields 2 rails.
  Tighten with `--expect-rails 2 --expect-speed 20Gb/s` once we know what
  the apple-compatible backend reports here.
- Replace `--hosts strix-N,mbp` once the mac's SSH alias / IP is known. The
  runner expects two hostnames it can reach via the LAN bridge.

## Headline

Not captured yet — slot reserved.
