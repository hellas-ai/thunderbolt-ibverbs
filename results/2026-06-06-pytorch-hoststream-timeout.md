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

## ACK Recovery Source Check

The current ACK-loss recovery path is full data retransmission:

```text
tbv_qp_timeout_reap_tx() moves an expired pending send to retry_sends
tbv_qp_timeout_work() reposts it through tbv_native_send_ctx_post_frames()
the receiver's duplicate-data path re-ACKs from ACK history
the sender then counts data_rx_ack_match_retried when the ACK matches
```

This matches the qptimeout14 run after adding `data_rx_duplicate_ack` to the
summary: most ACKs matched after retry are paired with duplicate-data re-ACKs,
not delayed original ACKs. That means the common tail is expensive because the
sender resends the whole operation to recover a missing control ACK.

There is a `data_rx_ack_cumulative` counter in debugfs, but no implementation
uses it today. It would also be a poor fit for the dominant PyTorch tail: the
failure logs commonly show new QPs at `psn=0`, so there is no earlier PSN range
for a cumulative ACK to cover.

A lower-latency fix should avoid blind extra fanout. The next plausible design
is an explicit ACK-query/re-ACK request before full data retransmission, with a
fallback to the existing data retransmit path if the query is lost or the peer
cannot answer from ACK history. That would need a new native control opcode and
new counters; it should be gated behind a module parameter until validated.

## Post-Reboot Focused Smoke

After both Strix hosts were rebooted and checked healthy, a shorter PyTorch
hoststream smoke was run with the current application baseline:

```text
ROCSHMEM_GDA_QP_TIMEOUT=14
ROCSHMEM_GDA_QP_RETRY_CNT=7
ROCSHMEM_GDA_QP_RNR_RETRY=7
native_ack_repeat=1
sizes=1048576,2097152
iters=4
reps=5
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-qptimeout14-postreboot-reps5-20260606-133623
status: pass
```

Per-rep summary from `tbv_app_gate_summarize.sh`:

```text
rep 1: 1MiB=34106.4us 2MiB=157485.1us wr_retx=8 ack_retry=7 dup_ack=7 reord_to=0 dv_hard=0 tx=12778/12778
rep 2: 1MiB=36238.4us 2MiB=115362.4us wr_retx=5 ack_retry=5 dup_ack=5 reord_to=0 dv_hard=0 tx=11163/11163
rep 3: 1MiB=12255.4us 2MiB=24373.7us  wr_retx=1 ack_retry=1 dup_ack=1 reord_to=0 dv_hard=0 tx=9020/9020
rep 4: 1MiB=14557.6us 2MiB=22638.1us  wr_retx=0 ack_retry=0 dup_ack=0 reord_to=0 dv_hard=0 tx=8748/8748
rep 5: 1MiB=29387.6us 2MiB=104482.8us wr_retx=6 ack_retry=5 dup_ack=5 reord_to=0 dv_hard=0 tx=11431/11431
```

The loader check again mapped exactly one RCCL:

```text
10 /mnt/Home/tmp/rccl-hoststream-waitbudget-install/lib/librccl.so.1.0
```

This is a clean application-level smoke result for the current baseline. It
does not remove the ACK tail: four of five reps still needed ACK-loss recovery,
and the matched-after-retry ACKs again track duplicate-data re-ACKs. The smaller
run did not exercise the heavier RX reorder/RNR failure mode seen in the
10-rep sweep.

## READY ACK Recovery Redeploy and ACK Probe A/B

After the next paired Strix reboot, the topology initially reproduced a native
READY_ACK loss during rail bring-up: some rails had `remote_ready=1`,
`ready_sent=0`, and `last_error=-110`. The active Nix profile had also not been
setting the new native READY recovery module parameters on strix-1/strix-2.

Committed and deployed:

```text
thunderbolt-ibverbs: 5cad50b kernel: tolerate READY ACK loss after peer READY
nixos-config:        1f72d54 strix: restore native READY timeout handling
nixos-config:        2fc83c0 strix: enable native READY recovery options
```

The Strix profile now sets:

```text
native_control_trace=Y
native_ready_timeout_optimistic=Y
hardware.thunderbolt-ibverbs.check.minReadyRails=4
```

The optimistic READY timeout path is intentionally narrow: it only converts a
READY_ACK timeout to success after peer READY has already been observed on that
rail. After redeploy and reboot, both hosts came back with 4 QPs, all rails
`data_ready=1 ready_sent=1 remote_ready=1 last_error=0`, and clean TX/RX
counters.

With the recovered topology, a 5-rep PyTorch hoststream A/B was run at the
current application baseline (`ROCSHMEM_GDA_QP_TIMEOUT=14`,
`native_ack_repeat=1`).

ACK probe off:

```text
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-ackprobe-off-20260606-141906
status: pass
wr_retx total: 49
ack_retry/ack64 total: 33/33
data_wr_timeout/data_wr_retry_exhausted: 0/0
data_tx_posted/completed: balanced in every rep
loaded RCCL: /mnt/Home/tmp/rccl-hoststream-waitbudget-install/lib/librccl.so.1.0
```

ACK probe on:

```text
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-ackprobe-on-20260606-142030
status: pass
wr_retx total: 5
ack_retry/ack64 total: 4/4
ack_probe/ack_probe_fb: 4/4
tx_ack_req/tx_ack_req_err: 4/0
rx_ack_req/rx_ack_req_reack/rx_ack_req_miss: 4/0/4
data_wr_timeout/data_wr_retry_exhausted: 0/0
data_tx_posted/completed: balanced in every rep
loaded RCCL: /mnt/Home/tmp/rccl-hoststream-waitbudget-install/lib/librccl.so.1.0
```

The A/B does not prove that ACK-query recovery works yet. Every ACK probe
reached the peer, but every peer lookup missed ACK history and fell back to full
data retransmission. The lower retransmit count in the probe-on run is therefore
not attributable to successful ACK-query repair; with only five reps it should
be treated as workload variance or timing. The useful new fact is narrower:
the ACK-probe request path is live and non-fatal under PyTorch hoststream load,
but the re-ACK lookup path is not matching the missing ACKs.

## ACK Probe Miss Classifier

Added live-QP ACK_REQ miss classifiers:

```text
data_rx_ack_req_miss_past     psn < rx_expected_psn, delivered but history miss
data_rx_ack_req_miss_current  psn == rx_expected_psn, receiver has not consumed it
data_rx_ack_req_miss_future   psn > rx_expected_psn, request raced ahead
```

Also added them to `tbv_app_gate_summarize.sh`. The first classifier run showed
the kernel counters but exposed a bench-tool allowlist gap: the app-gate
`TBV_COUNTER_KEYS` list did not capture the new fields, so the summarizer
printed `NA`. Live post-run counters on strix-2 nevertheless showed:

```text
data_rx_ack_req=14
data_rx_ack_req_reack=0
data_rx_ack_req_miss=14
data_rx_ack_req_miss_past=0
data_rx_ack_req_miss_current=14
data_rx_ack_req_miss_future=0
```

After adding the classifiers to `tbv_app_gate.sh`, the logged rerun was:

```text
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-ackprobe-classify2-20260606-143500
status: pass
wr_retx total: 2
ack_retry/ack64 total: 2/2
ack_probe/ack_probe_fb: 2/2
tx_ack_req/tx_ack_req_err: 2/0
rx_ack_req/rx_ack_req_reack/rx_ack_req_miss: 2/0/2
rx_ack_req_miss_past/current/future: 0/2/0
data_wr_timeout/data_wr_retry_exhausted: 0/0
data_tx_posted/completed: balanced in every rep
```

This changes the ACK-probe interpretation. The ACK_REQ mechanism is not usually
probing a delivered PSN whose ACK history exists or was overwritten. In these
two runs, all misses were for the receiver's current expected PSN, which means
the receiver had not consumed that operation when the sender's first timeout
fired. Full data retransmission is therefore the correct recovery for these
events; a pure ACK query cannot repair them. The remaining tail should be
treated as data/fragments arriving late or being lost before receiver PSN
consumption, not just reverse-channel SEND_ACK loss.

## Current-PSN ACK Probe Split

Added a second split for `data_rx_ack_req_miss_current`:

```text
data_rx_ack_req_miss_current_active   current PSN is actively assembling
data_rx_ack_req_miss_current_reorder  current PSN is buffered in reorder
data_rx_ack_req_miss_current_idle     no active/reorder state for current PSN
```

Deployed as:

```text
thunderbolt-ibverbs: 8d4ea5b kernel: classify current ACK probe misses
deploy worktree:     620d1d3 kernel: classify current ACK probe misses
nixos-config:        2846cd5 strix: deploy current ACK probe classifier
```

The ACK-probe classifier run:

```text
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-ackprobe-current2-20260606-144201
status: fail, rep 3 failed
ack_probe/ack_probe_fb: 21/21
rx_ack_req/rx_ack_req_reack/rx_ack_req_miss: 21/0/21
rx_ack_req_miss_past/current/future: 0/21/0
rx_ack_req_miss_current_active/reorder/idle: 21/0/0
data_wr_timeout/data_wr_retry_exhausted: 0/0
data_tx_posted/completed: balanced
```

Rep 3 failed with the known heavier PyTorch/GDA symptom:

```text
rank0: USB4 alltoall DATA wait timed out ctx=7 expected=7 observed=5
rank1: USB4 GDA CQE error status=255 opcode=3 byte_len=2097152
dv_hard_error=1
data_wr_rnr_retry_exhausted=1
data_rx_active_timeout/data_rx_active_retry=1/1
data_rx_reorder_timeout/data_rx_reorder_retry/data_rx_reorder_dropped=1/1/1
```

Kernel timeout logs on strix-1 explain the active state:

```text
native RX reorder timeout qpn=0x9c2 expected_psn=0 psn=0 src_qp=0x9c3
  kind=1 complete=0 received=1060864 total=2097152 frags=263/519
  last_offset=2096864 last_len=288 with_imm=0

native RDMA_WRITE active timeout qpn=0x9c2 src_qp=0x9c3 psn=0
  received=1967328 total=2097152 last_offset=1963280 last_len=4048
  with_imm=0
```

So the first timeout is firing while the receiver has already started the PSN.
In the failed case it had received about half of one 2 MiB write in reorder and
about 1.88 MiB of another active 2 MiB write before the active timeout/RNR
path fired. This is not an idle receiver, not an overwritten ACK-history entry,
and not primarily missing reverse ACK delivery.

For contrast, the same classifier build with `native_ack_probe=N`:

```text
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-current2-probeoff-20260606-144454
status: pass
wr_retx total: 23
ack_retry/ack64 total: 15/15
ack_probe/ack_probe_fb: 0/0
rx_ack_req/rx_ack_req_reack/rx_ack_req_miss: 0/0/0
data_rx_active_timeout=0
data_rx_reorder_timeout=0
data_wr_timeout/data_wr_retry_exhausted=0/0
data_tx_posted/completed: balanced in every rep
```

Conclusion: keep `native_ack_probe` disabled. It is useful instrumentation, but
the current implementation cannot repair the dominant PyTorch tail because the
receiver is still assembling the data when probed. The next correctness target
is the RX active/reorder timeout and RNR recovery path for large RDMA_WRITE
operations: why a partially received 2 MiB write stalls near completion, and
why the RNR retry path can surface as a DV hard error even though host TX
completion accounting stays balanced.

## RX Timeout Multiplier Control

Added `qp_rx_timeout_multiplier` as a runtime module parameter so receiver
active/reorder timeout can be scaled without changing the sender's verbs
`attr.timeout` retry cadence. Default is `1`; `0` maps to `1`; the runtime cap
is `32`. This is a diagnostic knob, not a default behavior change.

Two same-length PyTorch hoststream runs were made with `native_ack_probe=N`,
`ROCSHMEM_GDA_QP_TIMEOUT=14`, `ROCSHMEM_GDA_QP_RETRY_CNT=7`,
`ROCSHMEM_GDA_QP_RNR_RETRY=7`, and 10 reps of 1 MiB + 2 MiB `all_to_all`.

`qp_rx_timeout_multiplier=2`:

```text
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-rxmult2-qptimeout14-20260606-145819
status: fail, 9/10 passed, rep 4 failed
wr_retx/rnr_retx: 113/1
ack_retry/ack64: 52/52
data_rx_reorder_timeout/retry/drop: 2/2/2
data_rx_active_timeout/retry: 1/1
data_tx_ack_rnr/data_rx_ack_rnr: 3/1
data_wr_timeout/data_wr_retry_exhausted: 1/1
data_wr_rnr_retry_exhausted: 0
dv_hard_error: 1
data_tx_posted/completed: balanced in every rep
```

Failed rep 4:

```text
rank0: USB4 alltoall DATA wait timed out ctx=8 expected=8 observed=6
rank1: USB4 GDA CQE error status=5 opcode=3 byte_len=2097152
```

Kernel logs showed the receiver timeout window was actually extended: one
RNR-assisted incomplete reorder on strix-1 was acknowledged by strix-2 around
1.08 s total age and eventually completed at 1.73 s. A later 2 MiB WRITE on
strix-1 still timed out while active/reorder state was partially populated:

```text
native RX reorder timeout qpn=0xa17 src_qp=0xa16
  received=1603296 total=2097152 frags=397/519

native RDMA_WRITE active timeout qpn=0xa17 src_qp=0xa16
  received=1562528 total=2097152
```

Matching `qp_rx_timeout_multiplier=1` control:

```text
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-rxmult1-control-qptimeout14-20260606-150243
status: fail, 9/10 passed, rep 2 failed
wr_retx/rnr_retx: 160/7
ack_retry/ack64: 83/83
data_rx_reorder_timeout/retry/drop: 9/9/9
data_rx_active_timeout/retry: 1/1
data_tx_ack_rnr/data_rx_ack_rnr: 10/9
data_wr_timeout/data_wr_retry_exhausted: 0/0
data_wr_rnr_retry_exhausted: 1
dv_hard_error: 1
data_tx_posted/completed: balanced in every rep
```

Failed rep 2:

```text
rank0: USB4 GDA CQE error status=255 opcode=3 byte_len=2097152
rank1: USB4 alltoall DATA wait timed out ctx=8 expected=8 observed=6
```

The control failure is also a receive-side partial-WR/RNR case. strix-2 logged
several incomplete reorder timeouts and one active WRITE timeout, including:

```text
native RX reorder timeout qpn=0xca2 src_qp=0xca3
  received=1983808 total=2097152 frags=491/519

native RDMA_WRITE active timeout qpn=0xca6 src_qp=0xca7
  received=841984 total=2097152
```

Interpretation: increasing receiver timeout to `2x` did not eliminate the
large-WR failure class. It may help some incomplete receives finish after RNR,
but it can also delay RNR recovery relative to the sender retry cadence and
still allows WR timeout/exhaustion. The more precise next question is not
"larger RX timeout?" but "why do large WRITE fragments repeatedly stop arriving
before completion, and is `data_wr_rnr_retry_exhausted` a cause or a
post-failure teardown/QP-error consequence?"

Added follow-up counters in `749bb23` to split the RNR `-EAGAIN` completion
branch:

```text
data_wr_rnr_complete_retry_exhausted
data_wr_rnr_complete_closing_qp
data_wr_rnr_complete_qp_error
```

The next failing app-gate run should report these. If
`data_wr_rnr_complete_retry_exhausted` fires with `rnr_retry=7`/infinite, the
RNR retry accounting is wrong. If `closing_qp` or `qp_error` fires, the
existing `data_wr_rnr_retry_exhausted` total is a downstream symptom after the
first hard error, not the original cause.

## RNR Completion Classifier

The RNR ACK-time classifier was deployed on both Strix hosts and tested with the
same PyTorch hoststream baseline (`ROCSHMEM_GDA_QP_TIMEOUT=14`,
`native_ack_probe=N`, `qp_rx_timeout_multiplier=1`).

Initial 10-rep run:

```text
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-rnrclass-qptimeout14-20260606-151637
status: pass, 10/10
wr_retx/rnr_retx: 112/1
data_rx_reorder_timeout/retry/drop: 1/1/1
data_tx_ack_rnr/data_rx_ack_rnr: 1/1
data_wr_timeout/data_wr_retry_exhausted: 0/0
data_wr_rnr_retry_exhausted: 0
data_wr_rnr_complete_retry_exhausted/closing_qp/qp_error: 0/0/0
data_tx_posted/completed: balanced
```

The single RNR event recovered cleanly.

Longer 30-rep run:

```text
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-rnrclass30-qptimeout14-20260606-151943
status: fail, 21/30 passed
failed reps: 1,5,10,11,20,22,24,25,27
data_tx_posted/completed: balanced in every failed rep
```

The failures split into two signatures:

```text
status=5 failures: reps 1,5,10,20
  rank1: USB4 GDA CQE error status=5 opcode=3 byte_len=2097152
  rank0: USB4 alltoall DATA wait timed out
  data_wr_timeout/data_wr_retry_exhausted: 1/1
  data_wr_rnr_retry_exhausted: 0
  data_wr_rnr_complete_retry_exhausted/closing_qp/qp_error: 0/0/0

status=255 failures: reps 11,22,24,25,27
  rank1: USB4 GDA CQE error status=255 opcode=3 byte_len=2097152
  rank0: USB4 alltoall DATA wait timed out
  data_wr_timeout/data_wr_retry_exhausted: 0/0
  data_wr_rnr_retry_exhausted: 1
  data_wr_rnr_complete_retry_exhausted/closing_qp/qp_error: 0/0/0
```

The new split counters staying zero is the useful falsifier. The
`data_wr_rnr_retry_exhausted` status is not being produced by the RNR ACK
handler's retry-cap/closing-QP/QP-error branch. It must be coming from another
`-EAGAIN` send completion path. The most direct remaining candidate is the
`send->rnr_waiting` branch in `tbv_qp_timeout_reap_tx()`, which can mark an RNR
waiting send ready with `completion_status = -EAGAIN` when it decides the send
cannot be retried.

Next counter addition: classify that timeout-worker RNR-wait failure path by
the exact predicate that blocked retry (`not_retryable`, `retrying`,
`tx_pending`, `retry_exhausted`, `closing`, `QP error`, or `unknown`).

## RNR-Wait Classifier

Deployed the RNR-wait classifier and reran the same PyTorch hoststream baseline:

```text
thunderbolt-ibverbs: 1a9b4ce kernel: classify RNR wait completions
deploy worktree:     f8892d0 kernel: classify RNR wait completions
nixos-config:        2cf4999 strix: deploy RNR wait classifier
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-rnrwaitclass30-qptimeout14-20260606-154322
status: fail, 26/30 app reps completed, failed reps 6,7,20,22
loaded RCCL: /mnt/Home/tmp/rccl-hoststream-waitbudget-install/lib/librccl.so.1.0
data_tx_posted/completed: balanced in every rep
```

Rep 6 was a harness-policy failure, not an application failure. Both ranks
completed and printed 1 MiB and 2 MiB timings, but the counter gate rejected
late no-QP tombstone traffic:

```text
data_rx_no_qp: 1038
data_rx_no_qp_reack: 519
```

The existing `--allow-late-send-ack-no-qp` policy only allowed the OK SEND_ACK
half of this traffic. It did not allow the matching tombstone re-ack half, so
the harness produced a false hard failure.

The real failures were reps 7, 20, and 22. Each had the same shape:

```text
rank1 strix-2: USB4 GDA CQE error status=255 opcode=3 byte_len=2097152
rank0 strix-1: USB4 alltoall DATA wait timed out
data_wr_timeout/data_wr_retry_exhausted: 0/0
data_wr_rnr_retry_exhausted: 1
data_wr_rnr_complete_retry_exhausted/closing_qp/qp_error: 0/0/0
data_wr_rnr_wait_not_retryable/retrying/tx_pending/retry_exhausted/closing_qp/qp_error/unknown:
  0/0/1/0/0/0/0
```

This is now localized. `status=255` is not an RNR retry-cap failure, not a
closing-QP failure, and not a QP-error failure. It is the timeout worker seeing
an RNR-waiting send whose previous TX frames are still pending and treating
that temporary `tx_pending` state as terminal `-EAGAIN`.

That is inconsistent with the normal timeout path, which already defers while
`tx_pending` is nonzero. It is also inconsistent with the run result:
TX posted/completed was balanced by the end of each failed rep, so the pending
TX completions were not permanently lost. The next fix is therefore to make the
RNR-wait branch defer and reschedule when `tx_pending > 0`, then retry/fail only
after TX completions drain.

## RNR-Wait Tx-Pending Fix

Implemented the RNR-wait timeout-worker fix:

```text
thunderbolt-ibverbs: d0a6b88 kernel: defer RNR retry while TX pending
deploy worktree:     40a2c7e kernel: defer RNR retry while TX pending
nixos-config:        693fbbd strix: deploy RNR tx-pending defer
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-rnrwaitdefer30-qptimeout14-20260606-160107
status: fail, 29/30 app reps passed
failed rep: 27
data_tx_posted/completed: balanced in every rep
```

The old `status=255` signature disappeared in this run. All former RNR-wait
terminal buckets stayed at zero across the 30 reps:

```text
data_wr_rnr_retry_exhausted: 0
data_wr_rnr_wait_not_retryable/retrying/tx_pending/retry_exhausted/closing_qp/qp_error/unknown:
  0/0/0/0/0/0/0
```

That validates the previous localization: the RNR-wait branch was prematurely
completing a send with `-EAGAIN` while old TX frames were still pending. After
the defer/reschedule fix, the heavy-retry rows that previously produced
`status=255` ran through without RNR retry exhaustion.

Rep 27 failed with the older `status=5` class instead:

```text
rank0 strix-1: USB4 GDA CQE error status=5 opcode=3 byte_len=2097152
rank1 strix-2: USB4 alltoall DATA wait timed out ctx=9 expected=9 observed=7

data_wr_retransmit: 26 on strix-1
data_wr_timeout/data_wr_retry_exhausted: 1/1 on strix-1
data_wr_rnr_retransmit: 1 on strix-1
data_wr_rnr_retry_exhausted: 0
data_wr_rnr_wait_*: all 0
data_wr_retransmit_closing_qp/no_live_path/teardown_path: 0/0/0
data_rx_active_timeout/active_retry: 1/1 on strix-2
data_rx_reorder_timeout/reorder_retry/reorder_dropped: 2/2/2 on strix-2
data_rx_no_qp*: 0
data_tx_errors: 0
data_tx_posted/data_tx_completed: 19648/19648
```

So the RNR tx-pending bug is fixed, but the application-level path is still not
stable enough for long benchmarks. The remaining correctness issue is ordinary
RDMA_WRITE retry exhaustion paired with receiver-side partial-write active and
reorder timeouts. In the failing rep, the receiver timed out partially received
2 MiB writes after seeing late fragments near the tail offset, e.g. one reorder
timeout at 492/519 fragments and one active timeout at about 1.89 MiB/2 MiB.

Next target: classify why the large WRITE retry path still exhausts after
partial receive progress. The existing counters prove it is not the RNR-wait
bug, not teardown/no-QP, and not a local TX completion imbalance.
