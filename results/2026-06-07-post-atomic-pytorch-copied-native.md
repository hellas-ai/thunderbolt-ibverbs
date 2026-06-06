# 2026-06-07 Post-Atomic PyTorch Copied-Native Gate

Purpose:

Validate the PyTorch/RCCL all-to-all application path after the atomic ordering
fix, while explicitly checking that this row is copied-native TBV traffic rather
than the module DV poll path.

Command:

```bash
RCCL_ROCSHMEM_THRESHOLD=1 TBV_APP_TIMEOUT=420 \
  bash userspace/bench/tbv_app_gate.sh \
  --hosts 192.168.23.136,192.168.23.192 \
  --counter-hosts root@strix-1.local,root@strix-2.local \
  --iface eno1 \
  --log-root /mnt/Home/tmp/tbv-app-gate-logs/post-atomic-pytorch-hoststream-copied-1m-20260607-013240 \
  --skip-rccl --pytorch \
  --pytorch-wrapper /nix/store/3mr3fgrn6znah88jrc42r2wh692x24km-vllm-env-therock-usb4-hostheap-gfx1151 \
  --rccl-install /mnt/Home/tmp/rccl-hoststream-waitbudget-install \
  --rocshmem-install /mnt/Home/tmp/rocshmem-waitbudget-install \
  --rocm-path /nix/store/263sdskvmyld0qqcz8f7qf0zsx11i6l8-therock-rocm-sdk-gfx1151-7.13.0a20260515 \
  --mpi-home /nix/store/ciq3sjjgih6p38rlyfjsd2jjkzl8nfz1-openmpi-5.0.10 \
  --rdma-lib /nix/store/wc6j2l3k5qdjzwkvd27nb4v490qn0i9w-rdma-core-usb4-62.0/lib \
  --numactl-lib /nix/store/8xlwd35bpmj7n6bzjwfnr6vidpwicjdd-numactl-2.0.18/lib \
  --pytorch-sizes 1048576 \
  --pytorch-iters 10 --reps 3 \
  --torch-collectives all_to_all \
  --modes hoststream \
  --master-port 29619 \
  --dv-check forbid \
  --expected-rccl-lib /mnt/Home/tmp/rccl-hoststream-waitbudget-install/lib/librccl.so.1.0
```

Outcome:

```text
TBV app gate complete: status=0
summary: /mnt/Home/tmp/tbv-app-gate-logs/post-atomic-pytorch-hoststream-copied-1m-20260607-013240/summary.txt
```

PyTorch timing:

```text
hoststream all_to_all_single 1048576 bytes, 3 reps:
  min/avg/max: 588.8 / 630.8 / 708.8 us
  gpu min/avg/max: 588.0 / 630.0 / 708.1 us
  best logical per-rank bandwidth: 14.25 Gb/s
```

Library check:

```text
loaded_collective_lib counts:
  6 /mnt/Home/tmp/rccl-hoststream-waitbudget-install/lib/librccl.so.1.0
```

Counter profile:

```text
dv_poll_wqes: +0 in every rep
data_wr_retransmit: +0
data_wr_timeout: +0
data_wr_retry_exhausted: +0
data_tx_errors: +0
data_rx_canceled: +0
data_tx_posted/data_tx_completed total: 28461/28461
data_rx_reorder_buffered/delivered: +2304/+2304
native_tx_send_ack: +5760
data_rx_ack/data_rx_ack_matched: +8064/+2880
```

Interpretation:

- This is a valid PyTorch/RCCL application gate, but not a DV benchmark. The
  correct gate mode for this row is `--dv-check forbid`, matching the observed
  copied-native route.
- The earlier timing-probe attempts with `--dv-check auto` failed only because
  auto expects `dv_poll_wqes > 0` for hoststream mode. The application work
  completed and validated in those runs too, but the route assertion was wrong
  for this PyTorch shape.
- Compared with the RCCL-tests hoststream DV path, this PyTorch row is much
  faster at 1 MiB and has no RNR/write-gap recovery, but it is exercising a
  different transport route.
