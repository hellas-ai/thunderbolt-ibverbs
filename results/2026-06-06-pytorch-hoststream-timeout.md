# 2026-06-06 PyTorch Hoststream Timeout Sweep

## Context

The `RDMA_WRITE_WITH_IMM` reorder fix made the focused RCCL gates pass, so the
next application-level target was PyTorch `all_to_all` over the RCCL
rocSHMEM/GDA hoststream path.

The PyTorch gate must bypass the outer vLLM wrapper's Python because that
wrapper preloads its packaged RCCL while the gate explicitly loads the
waitbudget RCCL. Running the inner Python directly maps exactly one RCCL:

```text
/mnt/Home/tmp/rccl-hoststream-waitbudget-install/lib/librccl.so.1.0
```

All timeout-sweep runs below used:

```text
collective: all_to_all
mode: hoststream
sizes: 1048576,2097152
iters: 4
reps: 10
RCCL_ROCSHMEM_THRESHOLD=4194304
RCCL_INIT_CHANNELS=1
ROCSHMEM_GDA_QP_RETRY_CNT=7
ROCSHMEM_GDA_QP_RNR_RETRY=7
```

The tested variable was `ROCSHMEM_GDA_QP_TIMEOUT`, which maps to the verbs
RC QP `attr.timeout`. The module uses that timeout for TX retry cadence and
uses `tx_timeout * (retry_cnt + 1)` as the RX reorder lifetime.

## Tooling

Added:

```text
userspace/bench/tbv_app_gate_summarize.sh
```

It summarizes retained `tbv_app_gate.sh` PyTorch logs into a flat per-rep
timing/counter table. This avoids redoing the ad hoc AWK reductions from shell
history and makes future timeout sweeps comparable.

## Runs

### QP timeout 14

```text
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-innerpython-reps10-qptimeout14-20260606-131326
status: pass
```

Aggregate summary from `tbv_app_gate_summarize.sh`:

```text
reps=10
2MiB timing count=10
2MiB max=396768.1 us/iter
data_wr_retransmit=62
data_wr_rnr_retransmit=1
data_rx_ack_match_retried=33
data_rx_ack_match_over_64ms=33
data_rx_reorder_timeout=1
data_tx_ack_rnr/data_rx_ack_rnr=1/1
dv_hard_error=0
data_wr_timeout=0
data_wr_retry_exhausted=0
data_wr_rnr_retry_exhausted=0
data_tx_errors=0
data_tx_posted/completed=114576/114576
```

The worst rep combined ordinary ACK retry latency with one retryable RX reorder
timeout/RNR recovery:

```text
rep 9: 1MiB=135312.7 us/iter, 2MiB=396768.1 us/iter
       wr_retx=28 rnr_retx=1 ack_retry=11 reord_to=1
```

### QP timeout 15

```text
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-innerpython-reps10-qptimeout15-20260606-131630
status: pass
```

Aggregate summary:

```text
reps=10
2MiB timing count=10
2MiB max=1945121.4 us/iter
data_wr_retransmit=146
data_wr_rnr_retransmit=15
data_rx_ack_match_retried=64
data_rx_ack_match_over_64ms=64
data_rx_reorder_timeout=15
data_tx_ack_rnr/data_rx_ack_rnr=15/15
dv_hard_error=0
data_wr_timeout=0
data_wr_retry_exhausted=0
data_wr_rnr_retry_exhausted=0
data_tx_errors=0
data_tx_posted/completed=163325/163325
```

Increasing the timeout did not make the tail safer. It preserved correctness,
but increased the number of retryable reorder/RNR recoveries and stretched the
worst 2 MiB iteration to about 1.95 seconds.

### QP timeout 13

```text
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-innerpython-reps10-qptimeout13-20260606-131907
status: fail
```

Aggregate summary:

```text
reps=10
2MiB timing count=9
2MiB max=1107100.4 us/iter
data_wr_retransmit=193
data_wr_rnr_retransmit=23
data_rx_ack_match_retried=90
data_rx_ack_match_over_64ms=76
data_rx_reorder_timeout=28
data_rx_active_timeout=1
data_tx_ack_rnr/data_rx_ack_rnr=29/27
dv_hard_error=1
data_wr_timeout=0
data_wr_retry_exhausted=0
data_wr_rnr_retry_exhausted=1
data_tx_errors=0
data_tx_posted/completed=181835/181835
```

Rep 1 failed before printing the 2 MiB timing. The user-space symptom was:

```text
USB4 GDA CQE error status=255 opcode=3 byte_len=2097152
USB4 alltoall DATA wait timed out ctx=9 my_pe=1 src_team=0 src_world=0 expected=9 observed=7
HSA_STATUS_ERROR_EXCEPTION
SIGABRT
```

The hosts stayed usable after the failure:

```text
thunderbolt-ibverbs-check: ok on strix-1 and strix-2
data_tx_posted == data_tx_completed on both hosts
data_tx_errors=0
data_rx_canceled=0
```

### QP timeout 14, ACK repeat 2

The native ACK repeat module parameter was then raised from 1 to 2 on both
hosts and restored to 1 after the run:

```text
native_ack_repeat=2
ROCSHMEM_GDA_QP_TIMEOUT=14
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-innerpython-reps10-qptimeout14-ackrepeat2-20260606-132635
status: fail
```

Aggregate summary:

```text
reps=10
2MiB timing count=8
2MiB max=275917.5 us/iter
data_wr_retransmit=119
data_wr_rnr_retransmit=3
data_rx_ack_match_retried=48
data_rx_ack_match_over_64ms=48
data_rx_reorder_timeout=6
data_rx_active_timeout=2
data_tx_ack_rnr/data_rx_ack_rnr=8/4
dv_hard_error=2
data_wr_timeout=2
data_wr_retry_exhausted=2
data_wr_rnr_retry_exhausted=0
data_tx_errors=0
data_tx_posted/completed=129573/129573
```

The failure symptoms were device-side ROCSHMEM failures, not a host wedge:

```text
rep 5: USB4 GDA CQE error status=5 opcode=3 byte_len=2097152
       USB4 alltoall DATA wait timed out ctx=7 expected=7 observed=5

rep 8: USB4 GDA CQE error status=5 opcode=3 byte_len=1048576
       USB4 alltoall DATA wait timed out ctx=1 expected=1 observed=0
```

Post-run host check after restoring `native_ack_repeat=1`:

```text
thunderbolt-ibverbs-check: ok on strix-1 and strix-2
verbs_registered=1
verbs_qps=4
data_tx_posted == data_tx_completed on both hosts
data_tx_errors=0
data_rx_canceled=0
data_rx_repost_failed=0
```

Doubling ACK repeat is therefore not a safe quick fix for the ACK-retry tail.
It adds control pressure and produced real PyTorch/GDA failures in this run.

## Conclusions

1. The inner-Python gate fixes the prior PyTorch loader abort. All timeout
   runs mapped exactly one RCCL path, the waitbudget install.

2. QP timeout 14 is the best current application-level setting among the three
   tested values. It passed 10/10, preserved TX completion balance, and had the
   lowest retryable reorder/RNR count.

3. QP timeout 15 is correct but slower under this workload. It increased both
   ACK retry and retryable reorder/RNR recovery counts and produced multi-second
   tail latency.

4. QP timeout 13 is too aggressive for correctness today. It can recover in
   later reps, but one rep hit RNR retry exhaustion and a ROCSHMEM device-side
   wait timeout.

5. ACK repeat 2 is not a viable default from this data. It increased control
   traffic and produced two device-side failures at the otherwise safer QP
   timeout 14 setting.

6. The next bottleneck is not `RDMA_WRITE_WITH_IMM` correctness. It is latency
   from the control/retry path: ordinary ACK retry latency appears in many reps,
   while retryable RX reorder timeouts/RNR events cause the worst tails.

## Next Questions

The useful next experiments are:

```text
1. Keep QP timeout 14 as the baseline for application-level benchmarks.
2. Instrument or improve ACK recovery latency without increasing blind ACK
   fanout; the ACK repeat 2 run shows that extra control pressure can make the
   data path fail.
3. For RX reorder/RNR tails, determine whether the missing fragments are true
   frame loss, delayed delivery beyond the reorder window, or receiver-side
   processing starvation under PyTorch hoststream pressure.
```
