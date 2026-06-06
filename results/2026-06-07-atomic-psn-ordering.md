# 2026-06-07 Native Atomic PSN Ordering Fix

## Bug

The rocSHMEM fence-order signal tests exposed a native receive-side ordering
bug in `ATOMIC_REQ` handling.

Failing shape before the fix:

```text
operation: rocSHMEM fenceorderputwavesignalpefence
PEs: 2
z: 32
symptom: a later 4-byte WRITE was buffered behind an unconsumed atomic PSN
```

The receiver executed and responded to native atomic requests, but did not make
them part of the QP PSN stream. In particular, `ATOMIC_REQ` did not consume
`rx_expected_psn`, did not publish replay history, and did not drain the
receive reorder queue. A later WRITE on the same QP could therefore be treated
as a future PSN and remain buffered until the sender timed out.

The pre-fix counter shape matched that:

```text
data_rx_op_write                         +10
data_tx_ack_ok                           +2
data_rx_reorder_buffered                 +1
data_rx_reorder_duplicate_refresh        +7
```

## Fix

Native atomic requests now participate in the same QP ordering model as SEND,
WRITE, and READ request messages:

```text
expected ATOMIC_REQ     execute once, store old_value/status history, advance PSN
duplicate ATOMIC_REQ    replay the stored ATOMIC_RESP without re-executing
future ATOMIC_REQ       buffer in the receive reorder queue
no-QP duplicate         replay from the QP tombstone atomic history when present
missing history         send an error ATOMIC_RESP and count the miss
```

New counters:

```text
native_rx_atomic_req
native_rx_atomic_resp
data_rx_atomic_req
data_rx_atomic_replay
data_rx_atomic_history_miss
data_rx_atomic_reorder_buffered
data_rx_atomic_reorder_delivered
data_tx_atomic_resp_ok
data_tx_atomic_resp_error
```

The rocSHMEM example runner's default counter list now includes the atomic and
receive-reorder counters needed to see this path directly.

## Validation

Both Strix hosts were deployed and rebooted with the fix:

```text
strix-1: /nix/store/g4hb7hpy5n97wcvycmj7r9yfpj8nnv5m-nixos-system-strix-1-26.11pre-git
strix-2: /nix/store/ciaa5ng0acml95gqzddbx9vjlrd5a6vx-nixos-system-strix-2-26.11pre-git
kernel: 7.0.10
module: thunderbolt_ibverbs loaded, atomic counters exposed in debugfs
```

The module also builds cleanly against the reusable patched-kernel recipe:

```text
nix build .#thunderbolt-ibverbs-linux-thunderbolt
kernel recipe: linux-thunderbolt 7.1.0-rc1
result: thunderbolt_ibverbs.ko compiled successfully
```

Focused PE-specific fence-order repro:

```text
log root: /mnt/Home/tmp/tbv-rocshmem-example/fence-order-pe-z32-20260607-004003
test: fenceorderputwavesignalpefence
status: pass
```

All-PE fence-order repro:

```text
log root: /mnt/Home/tmp/tbv-rocshmem-example/fence-order-allpe-z32-20260607-004019
test: fenceorderputwavesignal
status: pass
```

Post-fix alltoall health check:

```text
log root: /mnt/Home/tmp/tbv-rocshmem-example/alltoall-post-20260607-004107
test: rocshmem_alltoall_test 4
status: pass
```

Focused repro after updating the runner counter list:

```text
log root: /mnt/Home/tmp/tbv-rocshmem-example/fence-order-pe-z32-counters-20260607-004141
test: fenceorderputwavesignalpefence
status: pass
```

Focused repro after the final `atomic_shape_valid` duplicate-buffer
discriminator and refreshed NixOS flake lock:

```text
log root: /mnt/Home/tmp/tbv-rocshmem-example/fence-order-pe-z32-post-shape-20260607-010140
test: fenceorderputwavesignalpefence
status: pass
```

Counter deltas from the final retained focused log:

```text
native_rx_atomic_req                    +16
native_rx_atomic_resp                   +16
data_rx_atomic_req                      +16
data_tx_atomic_resp_ok                  +16
data_rx_atomic_replay                   +0
data_rx_atomic_history_miss             +0
data_rx_atomic_reorder_buffered         +0
data_rx_atomic_reorder_delivered        +0
data_tx_atomic_resp_error               +0
data_rx_reorder_buffered                +0
data_rx_reorder_delivered               +0
data_rx_reorder_timeout                 +0
data_wr_retransmit                      +0
data_wr_timeout                         +0
data_wr_retry_exhausted                 +0
data_tx_errors                          +0
data_rx_canceled                        +0
data_tx_posted/completed                +56/+56
```

Final post-shape alltoall smoke:

```text
log root: /mnt/Home/tmp/tbv-rocshmem-example/alltoall-post-shape-20260607-010156
test: rocshmem_alltoall_test 4
status: pass
data_wr_retransmit                      +0
data_wr_timeout                         +0
data_wr_retry_exhausted                 +0
data_tx_errors                          +0
data_rx_canceled                        +0
data_tx_posted/completed                +12/+12
```

One host showed `data_rx_canceled=4096` in its direct summary after the first
two fence tests, but that value was already present in the first test's
`before` snapshot. It is therefore a reboot/topology baseline artifact from the
remote host coming back while rings were live, not a delta caused by these
rocSHMEM runs.
