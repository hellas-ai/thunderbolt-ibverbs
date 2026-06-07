# 2026-06-07 Post-Rebase PR24 Stack App Gate

## Context

Branch `codex/gda-on-correctness-stack-20260605` was rebased onto
`origin/main` after PR #24/#25 and the reliability-engine spine landed.

Deployed with:

```bash
cd /mnt/Home/src/nixos-config
nix flake lock --update-input thunderbolt-ibverbs-kernel
nix run .#colmena -- apply --on strix-1,strix-2 --reboot boot
```

Colmena completed and both hosts rebooted.

```text
strix-1: /nix/store/13w4gabknb0sjyqk02inbhap79hvlwda-nixos-system-strix-1-26.11pre-git
strix-2: /nix/store/226xridw3xvl90y8jslzrk3bwcy2z70j-nixos-system-strix-2-26.11pre-git
kernel: 7.0.10
```

Module parameters after reboot:

```text
profile=linux_perf
native_e2e=-1
native_qp_tombstone_max=4096
native_qp_tombstone_reack=Y
native_unsafe_retransmit_teardown_guard_disable=N
native_write_gap_rnr=Y
native_ack_drop_every=0
dv_poll_interval_us=10
dv_poll_budget=64
```

Note: the deployed path input included the current source worktree. At capture
time the worktree had uncommitted Apple-path/tunnel-gating edits in
`kernel/debugfs.c`, `kernel/ibdev.c`, `kernel/service.c`, and `kernel/tbv.h`.
The native Strix path tested below is not expected to depend on those Apple
changes, but this is the exact tree that was deployed.

## Mixed App Gate

Command:

```bash
TBV_APP_TIMEOUT=420 bash userspace/bench/tbv_app_gate.sh \
  --hosts 192.168.23.136,192.168.23.192 \
  --counter-hosts root@192.168.23.136,root@192.168.23.192 \
  --iface eno1 \
  --log-root /mnt/Home/tmp/tbv-app-gate-logs/post-rebase-pr24-stack-mixed-20260607-024441 \
  --rccl-tests-dir /nix/store/4bhvq0qphnq9ardka495pmji5f5130a0-rccl-tests-usb4-hostheap-gfx1151-2.14.1-local/bin \
  --rccl-install /mnt/Home/tmp/rccl-hoststream-waitbudget-install \
  --rocshmem-install /mnt/Home/tmp/rocshmem-waitbudget-install \
  --rocm-path /nix/store/263sdskvmyld0qqcz8f7qf0zsx11i6l8-therock-rocm-sdk-gfx1151-7.13.0a20260515 \
  --mpi-home /nix/store/ciq3sjjgih6p38rlyfjsd2jjkzl8nfz1-openmpi-5.0.10 \
  --rdma-lib /nix/store/wc6j2l3k5qdjzwkvd27nb4v490qn0i9w-rdma-core-usb4-62.0/lib \
  --numactl-lib /nix/store/8xlwd35bpmj7n6bzjwfnr6vidpwicjdd-numactl-2.0.18/lib \
  --sizes 262144,4194304 \
  --iters 3 --warmup 2 --reps 1 \
  --collectives alltoall \
  --modes hoststream \
  --pytorch \
  --pytorch-wrapper /nix/store/3mr3fgrn6znah88jrc42r2wh692x24km-vllm-env-therock-usb4-hostheap-gfx1151 \
  --pytorch-sizes 1048576 \
  --pytorch-iters 2 \
  --torch-collectives all_to_all \
  --master-port 29631 \
  --dv-check auto \
  --pytorch-dv-check forbid \
  --expected-rccl-lib /mnt/Home/tmp/rccl-hoststream-waitbudget-install/lib/librccl.so.1.0
```

Outcome:

```text
TBV app gate complete: status=0
log_root=/mnt/Home/tmp/tbv-app-gate-logs/post-rebase-pr24-stack-mixed-20260607-024441
```

RCCL-tests `alltoall` hoststream route:

```text
dv_poll_wqes:              +144
data_tx_posted/completed:  +36044/+36044
data_wr_retransmit:        +0
data_wr_rnr_retransmit:    +0
data_wr_timeout:           +0
data_wr_retry_exhausted:   +0
data_tx_errors:            +0
data_rx_canceled:          +0
data_rx_no_qp:             +0
data_rx_no_qp_reack:       +0
data_qp_tombstone_evicted: +0
data_rx_write_gap_rnr:     +0
```

RCCL timings:

```text
262144:  261.32 us out-of-place,  233.60 us in-place
524288:  374.95 us out-of-place,  367.04 us in-place
1048576: 657.64 us out-of-place,  645.79 us in-place
2097152: 677.42 us out-of-place,  583.31 us in-place
4194304: 1291.19 us out-of-place, 1255.64 us in-place
wrong results: 0
```

PyTorch `all_to_all_single` hoststream copied-native route:

```text
loaded_collective_lib=/mnt/Home/tmp/rccl-hoststream-waitbudget-install/lib/librccl.so.1.0
all_to_all_single 1048576: 761.9 us/iter, gpu=752.6 us/iter, 11.01 Gb/s logical/rank
dv_poll_wqes:              +0
data_tx_posted/completed:  +2808/+2808
data_wr_retransmit:        +0
data_wr_rnr_retransmit:    +0
data_wr_timeout:           +0
data_wr_retry_exhausted:   +0
data_tx_errors:            +0
data_rx_canceled:          +0
data_rx_no_qp:             +0
data_rx_no_qp_reack:       +0
data_qp_tombstone_evicted: +0
data_rx_write_gap_rnr:     +0
```

## Interpretation

The rebase onto the restored portable Thunderbolt patch stack did not regress
the mixed application gate after reboot. The RCCL-tests hoststream ladder still
exercises the module DV path, PyTorch still takes the copied-native route, both
rows have balanced TX completion counters, and the teardown/retransmit/tombstone
hard counters stayed quiet.

## RCCL Six-Row Smoke

Command:

```bash
TBV_APP_TIMEOUT=420 bash userspace/bench/tbv_app_gate.sh \
  --hosts 192.168.23.136,192.168.23.192 \
  --counter-hosts root@192.168.23.136,root@192.168.23.192 \
  --iface eno1 \
  --log-root /mnt/Home/tmp/tbv-app-gate-logs/post-rebase-pr24-stack-rccl-smoke-20260607-024644 \
  --rccl-tests-dir /nix/store/4bhvq0qphnq9ardka495pmji5f5130a0-rccl-tests-usb4-hostheap-gfx1151-2.14.1-local/bin \
  --rccl-install /mnt/Home/tmp/rccl-hoststream-waitbudget-install \
  --rocshmem-install /mnt/Home/tmp/rocshmem-waitbudget-install \
  --rocm-path /nix/store/263sdskvmyld0qqcz8f7qf0zsx11i6l8-therock-rocm-sdk-gfx1151-7.13.0a20260515 \
  --mpi-home /nix/store/ciq3sjjgih6p38rlyfjsd2jjkzl8nfz1-openmpi-5.0.10 \
  --rdma-lib /nix/store/wc6j2l3k5qdjzwkvd27nb4v490qn0i9w-rdma-core-usb4-62.0/lib \
  --numactl-lib /nix/store/8xlwd35bpmj7n6bzjwfnr6vidpwicjdd-numactl-2.0.18/lib \
  --sizes 65536,262144 \
  --iters 1 --warmup 1 --reps 1 \
  --collectives alltoall,alltoallv \
  --modes fallback,hoststream,device \
  --dv-check auto
```

Outcome:

```text
TBV app gate complete: status=0
log_root=/mnt/Home/tmp/tbv-app-gate-logs/post-rebase-pr24-stack-rccl-smoke-20260607-024644
```

Timing aggregate:

```text
alltoall  fallback   65536..262144: 132.32, 104.26, 120.31 us
alltoall  hoststream 65536..262144: 394.00, 183.74, 234.32 us
alltoall  device     65536..262144: 547.32, 599.93, 853.35 us
alltoallv fallback   65536..262144: 129.38, 101.41, 126.84 us
alltoallv hoststream 65536..262144: 433.74, 227.39, 387.71 us
alltoallv device     65536..262144: 639.89, 645.55, 963.47 us
wrong results: 0 in every row
```

Counter aggregate by row:

```text
collective mode       dv_poll_wqes tx_posted tx_completed hard_errors
alltoall   fallback   0            2150      2150         0
alltoall   hoststream 84           1062      1062         0
alltoall   device     84           1064      1064         0
alltoallv  fallback   0            2150      2150         0
alltoallv  hoststream 84           1065      1065         0
alltoallv  device     84           1063      1063         0
```

For every row:

```text
data_wr_retransmit:          0
data_wr_rnr_retransmit:      0
data_wr_timeout:             0
data_wr_retry_exhausted:     0
data_wr_rnr_retry_exhausted: 0
data_tx_errors:              0
data_rx_canceled:            0
data_rx_no_qp:               0
data_rx_no_qp_reack:         0
data_qp_tombstone_evicted:   0
data_rx_write_gap_rnr:       0
```

This re-establishes the pre-rebase RCCL app correctness baseline across
fallback, hoststream GDA, and device GDA modes.

## vLLM Lifecycle Smoke

Command wrapper:

```bash
log=/mnt/Home/tmp/tbv-vllm-smoke/post-rebase-pr24-stack-default-20260607-024825
for h in 192.168.23.136 192.168.23.192; do
  ssh root@$h cat /sys/kernel/debug/thunderbolt_ibverbs/summary > "$log/counters/before.$h.summary"
done

bash userspace/bench/tbv_vllm_smoke.sh \
  --hosts 192.168.23.136,192.168.23.192 \
  --iface eno1 \
  --log-root "$log" \
  --wrapper /nix/store/3mr3fgrn6znah88jrc42r2wh692x24km-vllm-env-therock-usb4-hostheap-gfx1151 \
  --model /mnt/Home/tmp/tbv-vllm-tiny-qwen3-gda \
  --prepare-tiny-model 1 \
  --rccl-install /mnt/Home/tmp/rccl-hoststream-waitbudget-install \
  --rocshmem-install /mnt/Home/tmp/rocshmem-waitbudget-install \
  --num-prompts 2 --input-len 8 --output-len 4 --max-model-len 512 \
  --kv-cache-bytes 16777216 --tp-size 2 --single-first 1

for h in 192.168.23.136 192.168.23.192; do
  ssh root@$h cat /sys/kernel/debug/thunderbolt_ibverbs/summary > "$log/counters/after.$h.summary"
done
```

Resolved commands:

```text
python=/nix/store/ckc6lc8s0mx7qfmskdlad3kndlf94ir3-vllm-env-therock-gfx1151/bin/python3
vllm=/nix/store/ckc6lc8s0mx7qfmskdlad3kndlf94ir3-vllm-env-therock-gfx1151/bin/vllm
ray=/nix/store/ckc6lc8s0mx7qfmskdlad3kndlf94ir3-vllm-env-therock-gfx1151/bin/ray
```

Results:

```text
single-node:
  elapsed_time: 0.116934898 s
  requests_per_second: 17.103533968
  tokens_per_second: 205.242407617

Ray TP=2:
  elapsed_time: 2.406009642 s
  requests_per_second: 0.831251864
  tokens_per_second: 9.975022369
```

Counter deltas were zero on both hosts for:

```text
dv_poll_wqes
dv_hard_error
data_wr_retransmit
data_wr_rnr_retransmit
data_wr_timeout
data_wr_retry_exhausted
data_tx_errors
data_rx_canceled
data_tx_posted
data_tx_completed
data_rx_no_qp
data_rx_no_qp_reack
data_qp_tombstone_evicted
native_atomic_req
native_atomic_resp
```

The command wrapper returned nonzero after the successful vLLM run because the
local `bash -lc` wrapper used `set -u` and then `/etc/bash_logout` referenced
`__ETC_BASHLOGOUT_SOURCED`. The vLLM script itself reached
`vLLM smoke complete` and produced both JSON result files.

Interpretation: the rebase did not regress the vLLM/Ray lifecycle smoke. As
before, this tiny vLLM workload does not exercise GDA verbs traffic; it is an
application-environment gate, not a GDA throughput benchmark.
