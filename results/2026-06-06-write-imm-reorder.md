# 2026-06-06 WRITE_WITH_IMM Reorder Fix

## Bug

The post-correctness-stack RCCL `alltoallv` host-stream gate exposed a real
receive-side correctness bug in native `RDMA_WRITE_WITH_IMM` handling.

Failing reproducer:

```text
log root: /tmp/tbv-app-gate-rccl-alltoallv-hoststream-diag-sizes1m2m-reps30-20260606-115928
collective: alltoallv
mode: hoststream
sizes: 1048576,2097152
reps: 30
failure rep: 11
app symptom: strix-2 ibv_post_send() failed with Invalid argument
```

Per-run counter deltas localized the failure:

```text
strix-1 data_tx_ack_error              +1
strix-2 data_wr_post_reject_status     +1
strix-1 data_rx_active_write_imm_flush +1
strix-1 data_rx_write_imm_gap          +7
strix-1 data_rx_write_imm_nonzero_first +1
```

The receiver used `hdr->imm_data` for the user immediate value, so
`RDMA_WRITE_WITH_IMM` did not carry an independent total length. Out-of-order
fragments therefore skipped the normal WRITE reorder path, hit a gap, flushed
the active write, and eventually sent an ERROR ACK.

## Fix

Commit:

```text
a0771a5 kernel: reorder RDMA write-with-imm fragments
```

The fix reuses the existing receive-side reorder queue for
`RDMA_WRITE_WITH_IMM`, learns total length from the LAST fragment, supports both
normal native and raw-stream chunk sizes, and sets the receive completion
`byte_len` for WRITE-with-imm completions.

New counters:

```text
data_rx_write_imm_reorder_buffered
data_rx_write_imm_reorder_delivered
```

## Validation

Focused post-fix rerun after deploying and rebooting both Strix hosts:

```text
log root: /tmp/tbv-app-gate-rccl-alltoallv-hoststream-write-imm-fix-sizes1m2m-reps30-20260606-121511
status: pass
collective: alltoallv
mode: hoststream
sizes: 1048576,2097152
reps: 30
```

Aggregated deltas from retained gate logs:

```text
data_tx_ack_error                    +0
data_wr_post_reject_status           +0
data_rx_active_write_imm_flush       +0
data_rx_write_imm_future_psn         +0
data_rx_write_imm_gap                +0
data_rx_write_imm_nonzero_first      +0
data_rx_write_imm_reorder_buffered   +53760
data_rx_write_imm_reorder_delivered  +53760
data_wr_timeout                      +0
data_wr_retry_exhausted              +0
data_tx_errors                       +0
```

Persistent broad RCCL gate:

```text
log root: /mnt/Home/tmp/tbv-app-gate-logs/rccl-broad-write-imm-fix-sizes1m2m-reps3-20260606-123643
status: pass
collectives: alltoall,alltoallv
modes: fallback,hoststream,device
sizes: 1048576,2097152
reps: 3
```

Aggregated broad-gate deltas:

```text
data_tx_ack_error                    +0
data_wr_post_reject_status           +0
data_rx_active_write_imm_flush       +0
data_rx_write_imm_future_psn         +0
data_rx_write_imm_gap                +0
data_rx_write_imm_nonzero_first      +0
data_rx_write_imm_reorder_buffered   +32256
data_rx_write_imm_reorder_delivered  +32256
data_rx_reorder_timeout              +0
data_rx_reorder_retry                +0
data_wr_retransmit                   +0
data_rx_ack_match_retried            +0
data_wr_timeout                      +0
data_wr_retry_exhausted              +0
data_tx_errors                       +0
data_tx_posted/completed             +354056/+354056
```

No `thunderbolt_ibverbs`, `BUG`, `Oops`, panic, watchdog, lockup, or kernel
`WARNING` line appeared in either host's kernel log during the persistent broad
run.

## Operational Note

After one broad run was lost because the default log root was under `/tmp`, the
gate gained `TBV_APP_LOG_PARENT` so unattended runs can default dated log
directories under a persistent parent while preserving the existing
`TBV_APP_LOG_ROOT` override.
