# 2026-06-07 Post-Atomic RCCL Sweep

Branch state:

- kernel/module contains native atomic PSN ordering and teardown/tombstone fixes.
- Strix hosts were booted into the refreshed GDA kernel closure (`uname -r`
  `7.0.10` on both hosts).
- `native_e2e` remains auto-disabled on AMD; no TX-completion wedge observed.

Command:

```bash
TBV_APP_TIMEOUT=420 bash userspace/bench/tbv_app_gate.sh \
  --hosts 192.168.23.136,192.168.23.192 \
  --counter-hosts root@strix-1.local,root@strix-2.local \
  --iface eno1 \
  --log-root /mnt/Home/tmp/tbv-app-gate-logs/post-atomic-rccl-sweep-rootcounters-20260607-012329 \
  --rccl-tests-dir /nix/store/4bhvq0qphnq9ardka495pmji5f5130a0-rccl-tests-usb4-hostheap-gfx1151-2.14.1-local/bin \
  --rccl-install /mnt/Home/tmp/rccl-hoststream-waitbudget-install \
  --rocshmem-install /mnt/Home/tmp/rocshmem-waitbudget-install \
  --rocm-path /nix/store/263sdskvmyld0qqcz8f7qf0zsx11i6l8-therock-rocm-sdk-gfx1151-7.13.0a20260515 \
  --mpi-home /nix/store/ciq3sjjgih6p38rlyfjsd2jjkzl8nfz1-openmpi-5.0.10 \
  --rdma-lib /nix/store/wc6j2l3k5qdjzwkvd27nb4v490qn0i9w-rdma-core-usb4-62.0/lib \
  --numactl-lib /nix/store/8xlwd35bpmj7n6bzjwfnr6vidpwicjdd-numactl-2.0.18/lib \
  --sizes 262144,4194304 \
  --iters 3 --warmup 2 --reps 2 \
  --collectives alltoall,alltoallv \
  --modes fallback,hoststream,device
```

Outcome:

```text
TBV app gate complete: status=0
summary: /mnt/Home/tmp/tbv-app-gate-logs/post-atomic-rccl-sweep-rootcounters-20260607-012329/summary.txt
```

Correctness/counter profile:

- `#wrong=0` for all 12 cases.
- GDA modes (`hoststream`, `device`) moved `dv_poll_wqes` by `+144` per case
  (`+72` on each host). Fallback stayed at `+0`.
- All hard counters stayed clean in every case:
  `dv_hard_error`, `data_wr_retransmit`, `data_wr_timeout`,
  `data_wr_retry_exhausted`, retransmit teardown guards, `data_tx_errors`, and
  `data_rx_canceled` all had `+0` deltas.
- `data_tx_posted == data_tx_completed` in every case.
- GDA modes still show benign recovery/backpressure activity in the summary
  aggregates. For example, summed over two reps:
  - `alltoall device`: `rnr_retx=44`, `ack_retry=269`,
    `write_gap_rnr=616`, `tx_post=77036`, `tx_comp=77036`.
  - `alltoallv hoststream`: `rnr_retx=25`, `ack_retry=68`,
    `write_gap_rnr=270`, `tx_post=75055`, `tx_comp=75055`.
  None of these recovered events turned into failed WRs or hard counters.

Selected 4 MiB timings (`out-of-place us / in-place us`):

```text
collective  mode        rep1            rep2
alltoall    fallback    1333.93/1296.85 1285.05/1286.63
alltoall    hoststream  1320.92/1297.36 1446.96/1415.28
alltoall    device      1312.78/1313.65 1308.03/1254.20

alltoallv   fallback    1344.11/1321.51 1340.04/1293.38
alltoallv   hoststream  1317.70/1366.35 1397.32/1376.86
alltoallv   device      1339.83/1281.40 1326.69/1323.17
```

Interpretation:

- This is the first post-atomic wider app-level gate that both passes and
  demonstrably exercises the GDA DV path.
- At 4 MiB, fallback, hoststream, and device modes are broadly similar
  (~1.25-1.45 ms). Smaller sizes are noisy and GDA modes still show high
  latency variance, so treat this as a correctness/app-gate milestone rather
  than a final performance claim.
- The copied-DV split remains consistent with the previous SG-cursor fix: copy
  time is small and the remaining kernel span is post-copy/native TX completion
  (`bucket 0` about `0.006-0.008 ms` copy and `0.107-0.114 ms` post-copy in this
  sweep; `bucket 2` about `0.015-0.019 ms` copy and `0.307-0.311 ms`
  post-copy). The next performance target is still native post-copy/TX drain and
  recovered RNR/write-gap pressure, not source-MR scatterlist walking.

## Hoststream Timing Probe

Tried a narrower timing-enabled RCCL-tests probe:

```bash
RCCL_ROCSHMEM_HOST_STREAM_TIMING=1 ROCSHMEM_DEBUG_LEVEL=INFO \
  TBV_APP_TIMEOUT=420 bash userspace/bench/tbv_app_gate.sh \
  --log-root /mnt/Home/tmp/tbv-app-gate-logs/post-atomic-hoststream-timing-20260607-012737 \
  --hosts 192.168.23.136,192.168.23.192 \
  --counter-hosts root@strix-1.local,root@strix-2.local \
  --sizes 1048576,1048576 --iters 10 --warmup 3 --reps 3 \
  --collectives alltoall,alltoallv --modes hoststream \
  --hoststream-addr-log 1 --usb4-a2a-timing-log 1
```

Outcome:

```text
TBV app gate complete: status=0
summary: /mnt/Home/tmp/tbv-app-gate-logs/post-atomic-hoststream-timing-20260607-012737/summary.txt
```

This remained clean (`#wrong=0`, no WR retransmit/timeout/retry exhaustion, no
TX errors, no RX cancels, balanced TX posted/completed), but no
`RCCL_ROCSHMEM_HOST_STREAM_TIMING`, `RCCL_ROCSHMEM_HOST_STREAM_ADDR`, or
`USB4_GDA_A2A_TIMING` records were emitted by this RCCL-tests path. The timing
knobs were present in the `mpirun` command, so this is a negative result for
using RCCL-tests to gather those phase logs.

The counter and kernel timing aggregates still help:

```text
rccl alltoall hoststream, 1 MiB, 3 reps:
  time_us_avg: 3161.35
  rnr_retx=81, ack_retry=390, write_gap_rnr=1183
  tx_post=34669, tx_comp=34669

rccl alltoallv hoststream, 1 MiB, 3 reps:
  time_us_avg: 6151.62
  rnr_retx=50, ack_retry=311, write_gap_rnr=649
  tx_post=29943, tx_comp=29943

dv_write_tx_mr_bucket:
  bucket 0 copy/postcopy: about 0.011-0.012 ms / 0.184 ms
  bucket 2 copy/postcopy: about 0.029 ms / 0.527-0.530 ms
```

The next useful performance probe needs either the PyTorch path that already
emits phase timing, or lower-level module/rocSHMEM instrumentation around the
post-copy/TX-drain span and the RNR/write-gap recovery loop.
