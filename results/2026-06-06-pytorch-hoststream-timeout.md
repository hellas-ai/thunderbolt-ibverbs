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

## Duplicate RX Refresh and Active/Reorder Merge

The next fix refreshed RX active/reorder timers when duplicate replay traffic
arrived for the same PSN. This prevents a retry stream from looking idle just
because the receiver is suppressing already-consumed duplicate fragments.

Deployed as:

```text
thunderbolt-ibverbs: 65932ff kernel: refresh RX timers on duplicate replay
nixos-config:        dc21b7b strix: deploy RX duplicate replay refresh
```

The 30-rep PyTorch hoststream run:

```text
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-dupreplayrefresh30-qptimeout14-20260606-163535
status: fail, 27/30 app reps passed
failed reps: 17,19,21
data_tx_posted/completed: balanced in every failed rep
```

All three failures retained the ordinary `status=5` class:

```text
data_wr_timeout/data_wr_retry_exhausted: 1/1
data_wr_rnr_retry_exhausted: 0
data_rx_active_timeout/active_retry: 1/1
data_rx_reorder_timeout/reorder_retry/reorder_dropped: 1/1/1
data_wr_retransmit_closing_qp/no_live_path/teardown_path: 0/0/0
data_rx_canceled: 0
data_tx_errors: 0
```

The duplicate-refresh counters fired heavily in the failed reps:

```text
rep 17: data_rx_reorder_duplicate_refresh=1165 data_rx_active_duplicate_refresh=1259
rep 19: data_rx_reorder_duplicate_refresh=1297 data_rx_active_duplicate_refresh=1736
rep 21: data_rx_reorder_duplicate_refresh=3742 data_rx_active_duplicate_refresh=6418
```

That is a useful falsifier. Duplicate replay traffic is real, and the refresh
logic is live, but refreshing duplicate activity does not guarantee completion
of the partially received WRITE. The receiver can continue seeing duplicate
fragments while still missing at least one required data fragment.

The failed kernel logs also showed same-PSN active and reorder state at timeout,
with the active WRITE missing an earlier offset and the reorder object holding
later fragments. Example:

```text
native RDMA_WRITE active timeout ... received=918896 total=1048576 last_offset=914848
native RX reorder timeout ... psn=0 received=906896 total=1048576 frags=225/260 last_offset=1048432
```

That suggested a possible stranded-fragment bug: buffered reorder fragments
might be contiguous with the active WRITE but never merged. A helper was added
to merge same-PSN buffered WRITE fragments into the active non-imm WRITE when
the next buffered fragment exactly matches `rx_write.received`.

Deployed as:

```text
thunderbolt-ibverbs: 73b3e58 kernel: merge buffered write fragments into active RX
nixos-config:        518d088 strix: deploy active write reorder merge
```

The 30-rep PyTorch hoststream run:

```text
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-activewritemerge30-qptimeout14-20260606-165020
status: fail, 29/30 app reps passed
failed rep: 22
data_tx_posted/completed: balanced in every rep
```

Rep 22 again had the ordinary WRITE timeout/exhaustion class:

```text
rank1 strix-2: USB4 GDA CQE error status=5 opcode=3 byte_len=2097152
rank0 strix-1: USB4 alltoall DATA wait timed out ctx=6 expected=6 observed=4

data_wr_retransmit: 17
data_wr_timeout/data_wr_retry_exhausted: 1/1
data_rx_active_timeout/active_retry: 1/1
data_rx_reorder_timeout/reorder_retry/reorder_dropped: 1/1/1
data_tx_ack_rnr/data_rx_ack_rnr: 2/0
data_tx_errors: 0
data_tx_posted/data_tx_completed: 10623/10623
```

The new merge counters stayed zero in every rep, including the failure:

```text
data_rx_active_write_reorder_merge: 0
data_rx_active_write_reorder_merge_bytes: 0
data_rx_active_write_reorder_merge_complete: 0
```

So the active/reorder merge hypothesis is falsified for this workload. The
receiver is not failing because it forgot to drain a contiguous buffered
fragment into the active WRITE. The missing data is the first gap at
`rx_write.received`; later fragments can be buffered and duplicate replay can
refresh timers, but the fragment needed to advance the active WRITE still does
not arrive before the sender exhausts the WR.

Current narrowed statement: the remaining PyTorch correctness failure is a
large non-imm `RDMA_WRITE` gap-repair problem. TX completion accounting is
balanced, teardown/no-QP guards are not firing, RNR-wait terminal buckets are
clear, and duplicate replay/merge does not close the gap. The next falsifier
should capture or change the first-gap repair policy: log the missing offset
and active/reorder rail/path IDs at timeout, or send an early targeted RNR when
the receiver first observes future fragments beyond an active WRITE gap.

## Targeted WRITE Gap RNR Probe

Added a disabled-by-default experiment:

```text
native_write_gap_rnr=N by default
data_rx_write_gap_rnr counter
active/reorder WRITE timeout logs now include gap_offset and first/last rail,
route, and path IDs
```

When enabled, the receiver sends `SEND_ACK_RNR` as soon as it observes a future
non-imm `RDMA_WRITE` fragment beyond the active gap. The RNR is targeted at the
missing byte offset, not always offset 0. Prefix replay fragments are then
suppressed until the retry reaches that gap; when the gap arrives, the existing
active/reorder merge path can advance the active WRITE.

Deployed as:

```text
thunderbolt-ibverbs: f06acf1 kernel: add WRITE gap RNR probe
deploy worktree:     df3d052 kernel: add WRITE gap RNR probe
nixos-config:        8892f24 strix: deploy WRITE gap RNR probe
```

After redeploy and reboot, both hosts came up healthy:

```text
strix-1 current system: /nix/store/6ybgwkf1n9qq5likla167dgv0grvd8g1-nixos-system-strix-1-26.11pre-git
strix-2 current system: /nix/store/046pqbwdswj7ndar9dfg7s290yw80fnw-nixos-system-strix-2-26.11pre-git
kernel: 7.0.10
verbs_qps: 4 on both hosts
native_write_gap_rnr: N by default
```

The knob was then enabled at runtime on both hosts and the same 30-rep PyTorch
hoststream gate was run:

```text
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-writegaprnr30-qptimeout14-20260606-171117
status: pass, 30/30
loaded RCCL: /mnt/Home/tmp/rccl-hoststream-waitbudget-install/lib/librccl.so.1.0
```

Aggregate summary:

```text
2MiB timing count/max: 30 / 221343.9 us
data_wr_retransmit: 0
data_wr_rnr_retransmit: 258
data_rx_ack_match_retried/data_rx_ack_match_over_64ms: 5163/1759
data_rx_write_gap_rnr: 10835
data_tx_ack_rnr/data_rx_ack_rnr: 10835/10835
data_rx_active_timeout/data_rx_reorder_timeout: 0/0
data_wr_timeout/data_wr_retry_exhausted: 0/0
data_wr_rnr_retry_exhausted: 0
dv_hard_error: 0
data_tx_errors: 0
data_tx_posted/data_tx_completed: 393356/393356
```

This is the first positive result against the remaining `status=5` WRITE gap
class. It does not prove the feature should be enabled by default yet, but it
does prove the targeted gap path is live and changes the failure dynamics:
large-WR gaps are repaired by bounded RNR retries before the receiver active or
reorder timeout fires. The cost is more RNR/control traffic and more
matched-after-retry ACK accounting, so the next validation should be a longer
run with the knob enabled plus a same-boot disabled control if we want a clean
A/B probability estimate.

A longer confidence sweep was then run with the knob enabled:

```text
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-writegaprnr100-qptimeout14-20260606-171729
status: pass, 100/100
loaded RCCL: /mnt/Home/tmp/rccl-hoststream-waitbudget-install/lib/librccl.so.1.0
```

Aggregate summary:

```text
2MiB timing count/max: 100 / 385475.6 us
data_wr_retransmit: 0
data_wr_rnr_retransmit: 1391
data_rx_ack_match_retried/data_rx_ack_match_over_64ms: 30095/10201
data_rx_write_gap_rnr: 60058
data_tx_ack_rnr/data_rx_ack_rnr: 60058/60058
data_rx_active_timeout/data_rx_reorder_timeout: 0/0
data_rx_reorder_duplicate_refresh: 5626
data_rx_active_duplicate_refresh: 83485
data_rx_active_write_reorder_merge: 0
data_wr_timeout/data_wr_retry_exhausted: 0/0
data_wr_rnr_retry_exhausted: 0
data_wr_rnr_wait_tx_pending/data_wr_rnr_wait_retry_exhausted: 10/0
dv_hard_error: 0
data_tx_errors: 0
data_tx_posted/data_tx_completed: 1571970/1571970
```

Post-run host counters stayed healthy after restoring the runtime knob to `N`:

```text
strix-1: verbs_qps=4 data_tx=951293/951293 data_tx_errors=0
         data_wr_timeout=0 data_wr_retry_exhausted=0
         data_rx_active_timeout=0 data_rx_reorder_timeout=0
         data_rx_canceled=0

strix-2: verbs_qps=4 data_tx=1014033/1014033 data_tx_errors=0
         data_wr_timeout=0 data_wr_retry_exhausted=0
         data_rx_active_timeout=0 data_rx_reorder_timeout=0
         data_rx_canceled=0
```

This materially strengthens the result. With targeted gap RNR enabled, the
large-WR failure class did not appear across 130 consecutive reps on the same
deployed build, while the repair path fired tens of thousands of times. The
remaining engineering question is policy, not localization: whether to enable
this in the Strix test profile now and then reduce/control the RNR pressure, or
keep it as an explicit benchmark knob until a same-boot disabled control gives
a cleaner probability comparison.

The Strix Nix profile was then updated to persist the knob:

```text
nixos-config: 8f56404 strix: enable WRITE gap RNR repair
```

After `colmena apply boot --on strix-1,strix-2 --reboot`, both hosts came back
with `native_write_gap_rnr=Y`, four verbs QPs, and clean counters:

```text
strix-1 current system: /nix/store/zwjp3rwrddl129cpv5myik1mrzixzhz2-nixos-system-strix-1-26.11pre-git
strix-2 current system: /nix/store/swsgxzfg6bxw3nlfwss7kkysd333n67i-nixos-system-strix-2-26.11pre-git
```

Post-persistence smoke:

```text
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-writegaprnr-persisted10-qptimeout14-20260606-173551
status: pass, 10/10
2MiB timing count/max: 10 / 114010.6 us
data_wr_retransmit: 0
data_wr_rnr_retransmit: 46
data_rx_ack_match_retried/data_rx_ack_match_over_64ms: 935/125
data_rx_write_gap_rnr: 1824
data_tx_ack_rnr/data_rx_ack_rnr: 1824/1824
data_rx_active_timeout/data_rx_reorder_timeout: 0/0
data_wr_timeout/data_wr_retry_exhausted: 0/0
data_wr_rnr_retry_exhausted: 0
dv_hard_error: 0
data_tx_errors: 0
data_tx_posted/data_tx_completed: 111025/111025
```

This makes the current Strix application-test baseline: native E2E disabled on
AMD, duplicate replay refresh present, RNR tx-pending defer present, and
targeted WRITE gap RNR enabled in the host profile.

## Broader Application Gate

With the persisted Strix profile still active (`native_write_gap_rnr=Y` on both
hosts), a broader app gate was run:

```text
log root: /mnt/Home/tmp/tbv-app-gate-logs/app-broad-writegaprnr-reps3-20260606-173906
status: pass
RCCL collectives: alltoall, alltoallv
RCCL modes: fallback, hoststream, device
RCCL sizes: 262144, 1048576, 2097152
PyTorch collectives: all_reduce, all_gather, all_to_all
PyTorch modes: fallback, hoststream, device
PyTorch sizes: 1048576, 2097152
reps: 3
```

RCCL coverage:

```text
18/18 RCCL test cases passed
all rccl-tests validation wrong counts: 0
alltoall 2MiB max algbw:
  fallback=1.78 GB/s, hoststream=1.52 GB/s, device=1.58 GB/s
alltoallv 2MiB max algbw:
  fallback=1.73 GB/s, hoststream=1.65 GB/s, device=1.56 GB/s
only WARNs were the expected RCCL_FORCE_ENABLE_DMABUF notices
```

PyTorch summary:

```text
fallback:   reps=3 t2_max=6779.5 us  wr_retx=0 rnr_retx=0  write_gap_rnr=0   dv_hard=0 wr_to=0 tx=102089/102089
hoststream: reps=3 t2_max=31145.3 us wr_retx=0 rnr_retx=1  write_gap_rnr=2   dv_hard=0 wr_to=0 tx=89476/89476
device:     reps=3 t2_max=79329.5 us wr_retx=0 rnr_retx=19 write_gap_rnr=508 dv_hard=0 wr_to=0 tx=96953/96953
```

Post-run host counters remained healthy:

```text
strix-1: verbs_qps=4 data_tx=465375/465375 data_tx_errors=0
         data_wr_timeout=0 data_wr_retry_exhausted=0
         data_rx_active_timeout=0 data_rx_reorder_timeout=0
         data_rx_canceled=0

strix-2: verbs_qps=4 data_tx=462035/462035 data_tx_errors=0
         data_wr_timeout=0 data_wr_retry_exhausted=0
         data_rx_active_timeout=0 data_rx_reorder_timeout=0
         data_rx_canceled=0
```

This is the first broad application-level pass after the correctness fixes. It
does not replace longer workload benchmarking, but it moves the branch from
"focused PyTorch all_to_all still fails" to "RCCL alltoall/alltoallv and
PyTorch all_reduce/all_gather/all_to_all all pass a small multi-mode gate."

## Focused Host-Stream Exchange Tail

The app-gate summarizer was extended so broad runs are reproducible without
one-off parsing:

```text
8b8a00b bench: summarize broad app gate logs
8555c1a bench: summarize hoststream phase timing
```

`tbv_app_gate_summarize.sh` now emits PyTorch timing aggregates, RCCL
rccl-tests timing/validation aggregates, compact counter aggregates, and
host-stream phase aggregates from
`RCCL_ROCSHMEM_HOST_STREAM_TIMING` lines. The phase table is keyed by app mode,
RCCL bench mode, `msgSize`, `rankOffset`, and `symId`, and reports exchange
p50/p90/p95/p99/max.

Fresh focused run on the persisted Strix profile:

```text
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-exchange-tail-writegaprnr20-qptimeout14-20260606-175409
status: pass, 20/20
collective: torch.distributed all_to_all_single
mode: hoststream, validated payloads
sizes: 1048576,2097152 bytes/rank
iters: 8
RCCL_ROCSHMEM_THRESHOLD=4194304
RCCL_ROCSHMEM_GDA_BENCH_MODE=0
RCCL_ROCSHMEM_HOST_STREAM_TIMING=1
ROCSHMEM_GDA_QP_TIMEOUT=14
loaded RCCL: /mnt/Home/tmp/rccl-hoststream-waitbudget-install/lib/librccl.so.1.0
```

Application timing aggregate:

```text
1MiB/rank: n=20 min=9652.0us avg=17350.5us max=86441.5us
2MiB/rank: n=20 min=21358.5us avg=35562.9us max=138420.2us
```

Host-stream phase aggregate:

```text
msgSize  rankOffset symId n   copyIn_avg exchange_avg exchange_p50 exchange_p90 exchange_p95 exchange_p99 exchange_max copyOut_avg total_avg total_p90 total_max
2097152  1048576    0     200 0.031      4.972        2.039        7.261        22.257       38.388       75.578       0.035       5.038     7.326     75.642
2097152  1048576    1     160 0.032      29.133       20.554       32.182       57.876       204.695      204.697      0.035       29.200    32.245    204.771
4194304  2097152    0     160 0.055      10.300       4.276        22.583       26.456       93.549       93.572       0.064       10.420    22.706    93.687
4194304  2097152    1     200 0.056      68.516       41.613       124.037      141.672      258.423      752.788      0.064       68.635    124.149   752.905
```

Counter aggregate:

```text
wr_retx=0
rnr_retx=183
ack_retry/ack64=2380/376
late_ack/dup_ack=4814/87
write_gap_rnr=6603
tx_rnr/rx_rnr=6603/6603
reorder_timeout/active_timeout=0/0
rnr_retry_exhausted=0
dv_hard_error=0
wr_timeout/wr_retry_exhausted=0/0
data_tx_errors=0
data_tx_posted/data_tx_completed=394854/394854
```

Post-run host health stayed clean:

```text
strix-1: native_write_gap_rnr=Y verbs_qps=4 dv_poll_wqes=1040
         data_tx=656928/656928 data_tx_errors=0
         data_wr_timeout=0 data_wr_retry_exhausted=0
         data_rx_no_qp=0 data_rx_canceled=0
         data_rx_active_timeout=0 data_rx_reorder_timeout=0

strix-2: native_write_gap_rnr=Y verbs_qps=4 dv_poll_wqes=1040
         data_tx=665336/665336 data_tx_errors=0
         data_wr_timeout=0 data_wr_retry_exhausted=0
         data_rx_no_qp=0 data_rx_canceled=0
         data_rx_active_timeout=0 data_rx_reorder_timeout=0
```

Interpretation: the focused app-level GDA window is correctness-clean on this
profile, but still performance-unstable. Copy-in/copy-out are negligible; the
dominant cost is the rocSHMEM exchange phase. The slow path is consistently
`symId=1`, and the 4 MiB RCCL message-size row has an especially large tail
(`exchange_p99=258ms`, `exchange_max=753ms`). The next bottleneck investigation
should target why the host-stream exchange alternates between fast `symId=0`
and much slower/tailier `symId=1` rather than changing kernel correctness policy.

### Fixed Scratch Slot A/B

The RCCL host-stream path uses two scratch slots:

```text
NUM_SYM_BUF=2
symId = fixedSymId >= 0 ? fixedSymId % comm->numSymBuf : comm->symId
src = sourceRshmem + symId * bufThreshold
dst = destRshmem + symId * bufThreshold
bufThreshold = rocshmemBufferSize / numSymBuf
```

With the current app-gate hoststream defaults, `ROCSHMEM_HEAP_SIZE=1GiB`,
`rocshmemBufferSize=256MiB`, and `bufThreshold=128MiB`; source and dest scratch
buffers are heap-backed (`RCCL_ROCSHMEM_SOURCE_HEAP=1`,
`RCCL_ROCSHMEM_DEST_HEAP=1`).

Ran matching fixed-slot probes with validated PyTorch all-to-all, 1MiB/2MiB per
rank, eight iterations, ten reps, and QP timeout exponent 14:

```text
symId=0 log: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-fixedsym0-writegaprnr10-qptimeout14-20260606-175933
symId=1 log: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-fixedsym1-writegaprnr10-qptimeout14-20260606-180115
status: both pass, 10/10
```

Application timing:

```text
fixed symId=0:
  1MiB/rank n=10 min=1607.6us  avg=8413.0us   max=34798.5us
  2MiB/rank n=10 min=3520.0us  avg=22963.0us  max=60189.8us

fixed symId=1:
  1MiB/rank n=10 min=21522.5us avg=65082.8us  max=169010.7us
  2MiB/rank n=10 min=70631.9us avg=227875.1us max=683096.4us
```

Host-stream exchange timing:

```text
fixed symId=0:
  msgSize=2097152  n=180 exchange_avg=8.657ms   p50=4.983ms   p90=21.097ms  p99=94.538ms  max=94.574ms
  msgSize=4194304  n=180 exchange_avg=22.270ms  p50=14.539ms  p90=58.883ms  p99=86.265ms  max=86.267ms

fixed symId=1:
  msgSize=2097152  n=180 exchange_avg=65.519ms  p50=29.318ms  p90=186.882ms p99=228.492ms max=228.496ms
  msgSize=4194304  n=180 exchange_avg=227.653ms p50=103.092ms p90=750.115ms p99=884.348ms max=884.425ms
```

Counters stayed correctness-clean in both legs:

```text
fixed symId=0:
  wr_retx=0 rnr_retx=331 write_gap_rnr=11867
  dv_hard=0 wr_timeout=0 wr_retry_exhausted=0
  reorder_timeout=0 active_timeout=0 tx=314120/314120

fixed symId=1:
  wr_retx=0 rnr_retx=458 write_gap_rnr=12824
  dv_hard=0 wr_timeout=0 wr_retry_exhausted=0
  reorder_timeout=0 active_timeout=0 tx=378643/378643
```

Post-run host counters remained healthy on both Strix hosts: no no-QP frames,
no RX cancels, no active/reorder timeouts, no DV hard errors, and balanced TX.

Interpretation: the slow/taily path is intrinsic to using scratch slot 1 in the
current heap-backed allocation, not merely an artifact of alternating slots.
Forcing slot 1 alone is much slower than alternation; forcing slot 0 alone is
faster than alternation. The next performance discriminator should compare
heap-backed scratch against host-coherent scratch, and/or log the actual local
and remote scratch addresses/keys per slot, because slot 1 is a fixed
`+128MiB` offset into the same symmetric allocations.

### Host-Coherent Scratch Slot A/B

Added app-gate knobs for the RCCL scratch backing and fixed scratch slot:

```text
--source-heap 0|1
--dest-heap 0|1
--hoststream-fixed-symid N
```

Then reran the fixed-slot PyTorch all-to-all probe with both source and
destination scratch forced to host-coherent memory instead of rocSHMEM heap
memory:

```text
symId=0 log: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-coherent-fixedsym0-writegaprnr5-qptimeout14-20260606-180804
symId=1 log: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-coherent-fixedsym1-writegaprnr5-qptimeout14-20260606-180908
status: both pass, 5/5
options: --source-heap 0 --dest-heap 0 --hoststream-fixed-symid {0,1}
```

Application timing:

```text
host-coherent fixed symId=0:
  1MiB/rank n=5 min=4013.7us  avg=13313.7us  max=31467.4us
  2MiB/rank n=5 min=8449.9us  avg=26804.7us  max=55542.0us

host-coherent fixed symId=1:
  1MiB/rank n=5 min=20184.1us avg=33354.9us  max=63318.9us
  2MiB/rank n=5 min=62957.3us avg=108596.8us max=233660.6us
```

Host-stream exchange timing:

```text
host-coherent fixed symId=0:
  msgSize=2097152  n=90 exchange_avg=13.254ms  p50=9.820ms  p90=30.773ms  p99=39.307ms  max=39.307ms
  msgSize=4194304  n=90 exchange_avg=26.152ms  p50=15.616ms p90=52.488ms  p99=75.614ms  max=75.614ms

host-coherent fixed symId=1:
  msgSize=2097152  n=90 exchange_avg=33.470ms  p50=21.757ms p90=64.699ms  p99=84.108ms  max=84.108ms
  msgSize=4194304  n=90 exchange_avg=105.277ms p50=83.815ms p90=185.558ms p99=421.988ms max=421.988ms
```

Counters again stayed correctness-clean:

```text
host-coherent fixed symId=0:
  wr_retx=0 rnr_retx=196 write_gap_rnr=5954
  dv_hard=0 wr_timeout=0 wr_retry_exhausted=0
  reorder_timeout=0 active_timeout=0 tx=166469/166469

host-coherent fixed symId=1:
  wr_retx=0 rnr_retx=86 write_gap_rnr=4958
  dv_hard=0 wr_timeout=0 wr_retry_exhausted=0
  reorder_timeout=0 active_timeout=0 tx=122817/122817
```

Post-run host counters remained healthy on both Strix hosts: no no-QP frames,
no RX cancels, no active/reorder timeouts, no DV hard errors, and balanced TX.

Interpretation: host-coherent scratch improves the fixed `symId=1` tail versus
heap-backed slot 1, but it does not remove the slot asymmetry. Slot 1 remains
substantially slower than slot 0 when source/destination backing is held
constant. That rules out "rocSHMEM heap allocation only" as the whole cause and
keeps the next discriminator focused on the per-slot address/translation path:
log the actual scratch addresses and registration keys for each slot, then
split source-vs-destination backing if needed.

### Host-Coherent Source/Destination Split

Split the fixed `symId=1` host-stream probe by scratch backing:

```text
source coherent, destination heap:
  log: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-srccoh-destheap-fixedsym1-writegaprnr5-qptimeout14-20260606-181355
source heap, destination coherent:
  log: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-srcheap-destcoh-fixedsym1-writegaprnr5-qptimeout14-20260606-181502
status: both pass, 5/5
```

Application timing:

```text
source coherent, destination heap:
  1MiB/rank n=5 min=20996.7us avg=57137.3us  max=147567.6us
  2MiB/rank n=5 min=45793.8us avg=144160.3us max=414666.3us

source heap, destination coherent:
  1MiB/rank n=5 min=21156.5us avg=49574.8us  max=143106.6us
  2MiB/rank n=5 min=61456.5us avg=137322.9us max=367258.1us
```

Host-stream exchange timing:

```text
source coherent, destination heap:
  msgSize=2097152  n=90 exchange_avg=54.951ms  p50=21.290ms p90=119.791ms p99=244.185ms max=244.185ms
  msgSize=4194304  n=90 exchange_avg=137.898ms p50=83.486ms p90=233.906ms p99=895.682ms max=895.682ms

source heap, destination coherent:
  msgSize=2097152  n=90 exchange_avg=48.026ms  p50=24.553ms p90=82.395ms  p99=302.088ms max=302.088ms
  msgSize=4194304  n=90 exchange_avg=137.345ms p50=90.149ms p90=237.898ms p99=1030.954ms max=1030.954ms
```

Counters stayed correctness-clean:

```text
source coherent, destination heap:
  wr_retx=0 rnr_retx=135 write_gap_rnr=3049
  dv_hard=0 wr_timeout=0 wr_retry_exhausted=0
  reorder_timeout=0 active_timeout=0 tx=139653/139653

source heap, destination coherent:
  wr_retx=0 rnr_retx=120 write_gap_rnr=4007
  dv_hard=0 wr_timeout=0 wr_retry_exhausted=0
  reorder_timeout=0 active_timeout=0 tx=135518/135518
```

Post-run host counters remained healthy on both Strix hosts: no no-QP frames,
no RX cancels, no active/reorder timeouts, no DV hard errors, and balanced TX.

Interpretation: the slow fixed `symId=1` path is not caused solely by source
heap backing or solely by destination heap backing. Host-coherent backing on
both sides is the best `symId=1` result so far, but every fixed `symId=1`
variant remains much slower than fixed `symId=0`. The next useful
instrumentation is address-level: log the base pointer, per-slot pointer,
slot offset, backing mode, and any available registration/remote-key metadata
for source and destination scratch so the `+128MiB` slot can be tied to actual
local and remote translations.

### Host-Stream Address-Log Tooling Validation

Added an opt-in RCCL diagnostic:

```text
RCCL_ROCSHMEM_HOST_STREAM_ADDR_LOG=1
```

When enabled, RCCL emits one `RCCL_ROCSHMEM_HOST_STREAM_ADDR` line per
host-stream all-to-all call with rank, mode, message size, selected `symId`,
`fixedSymId`, source/destination backing mode, base pointer, slot pointer,
`numSymBuf`, `bufThreshold`, and `slotOffset`. The app gate now exposes this as
`--hoststream-addr-log`, forwards it through MPI/PyTorch, and the summarizer
collapses the per-call lines into unique address layouts.

Built and installed the instrumented RCCL into the existing waitbudget prefix:

```text
/mnt/Home/tmp/rccl-hoststream-waitbudget-install/lib/librccl.so.1.0
marker: RCCL_ROCSHMEM_HOST_STREAM_ADDR present
```

Validation run:

```text
log: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-addrlog-coherent-auto-writegaprnr1-qptimeout14-20260606-183306
options: --source-heap 0 --dest-heap 0 --hoststream-addr-log 1
status: pass, 1/1
```

Address-layout summary:

```text
rank=0 msgSize=2097152 symId=0 fixed=-1 count=2 sourceBase=0x7e5173e08d00 sourceSlot=0x7e5173e08d00 destBase=0x7e5183e08d80 destSlot=0x7e5183e08d80 slotOffset=0
rank=0 msgSize=2097152 symId=1 fixed=-1 count=1 sourceBase=0x7e5173e08d00 sourceSlot=0x7e517be08d00 destBase=0x7e5183e08d80 destSlot=0x7e518be08d80 slotOffset=134217728
rank=1 msgSize=2097152 symId=0 fixed=-1 count=2 sourceBase=0x7bd1d1c08d00 sourceSlot=0x7bd1d1c08d00 destBase=0x7bd1e1c08d80 destSlot=0x7bd1e1c08d80 slotOffset=0
rank=1 msgSize=2097152 symId=1 fixed=-1 count=1 sourceBase=0x7bd1d1c08d00 sourceSlot=0x7bd1d9c08d00 destBase=0x7bd1e1c08d80 destSlot=0x7bd1e9c08d80 slotOffset=134217728
```

Phase timing in this small validation still showed the same direction:
`symId=1` exchange was slower than `symId=0` even with coherent scratch on both
sides (`4194304` msgSize: `symId=0 exchange_avg=7.919ms`,
`symId=1 exchange_avg=58.615ms`). Counters stayed correctness-clean:
`wr_retx=0`, `wr_timeout=0`, `wr_retry_exhausted=0`, `dv_hard=0`, no
active/reorder timeouts, no no-QP frames, no RX cancels, and `tx=6902/6902`.

Interpretation: the new diagnostic proves the selected slow slot is exactly the
second half of the symmetric scratch allocation on both ranks. The next
performance discriminator should move below RCCL's local pointer view and log
the rocSHMEM USB4 backend's chosen remote address/rkey/lkey or memory-region
classification for those same source/destination slots.

### rocSHMEM USB4 Post-Key Diagnostic

Added an opt-in rocSHMEM/GDA device-side diagnostic:

```text
ROCSHMEM_GDA_USB4_A2A_POST_LOG=N
```

When enabled, the USB4 all-to-all path prints one `USB4_GDA_A2A_POST` line for
the first `N` all-to-all payload posts. The line includes the selected local
source address, remote destination address, remote pSync/signal address,
source/destination memory classification, heap/sync offsets, selected lkey/rkey,
the heap and sync keys stored in the QP, QPN, and SQ/CQ cursors. The app gate
now exposes this as `--usb4-a2a-post-log`, forwards it through MPI/PyTorch, and
the summarizer collapses the per-rank lines into unique post layouts.

Built and installed the instrumented rocSHMEM and RCCL into the existing
waitbudget prefixes, then ran a small coherent-scratch PyTorch host-stream
all-to-all with the post logger enabled:

```text
log: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-a2apost-coherent-auto-writegaprnr1-qptimeout14-20260606-185742
options: --source-heap 0 --dest-heap 0 --hoststream-addr-log 1 --usb4-a2a-post-log 8
status: pass, 1/1
loaded RCCL: /mnt/Home/tmp/rccl-hoststream-waitbudget-install/lib/librccl.so.1.0
```

Phase timing still reproduced the slot asymmetry in the exchange phase:

```text
msgSize=2097152 symId=0 exchange_avg=3.676ms
msgSize=2097152 symId=1 exchange_avg=22.550ms
msgSize=4194304 symId=0 exchange_avg=3.461ms
msgSize=4194304 symId=1 exchange_avg=89.618ms
```

The RCCL address logger again showed coherent scratch selection with two
128 MiB slots:

```text
source_backing=coherent
dest_backing=coherent
num_sym_buf=2
buf_threshold=134217728
symId=0 slot_offset=0
symId=1 slot_offset=134217728
```

The rocSHMEM USB4 post logger showed the actual WQEs use heap-classified
payload addresses on both sides, while only the pSync signal target is in the
remote sync pool:

```text
all payload posts:
  src_heap=1 src_sync=0
  dst_heap=1 dst_sync=0
  sync_remote_sync=1
  qpn=0
  src_lkey=dst_rkey=heap_lkey=heap_rkey=sync_lkey=sync_rkey=39871

rank0 symId=0:
  src_heap_off=1084672/2133248
  dst_heap_off=268471680
  sync_remote_off=16400

rank0 symId=1:
  src_heap_off=135302400/136350976
  dst_heap_off=402689408
  sync_remote_off=17424

rank1 symId=0:
  src_heap_off=36096
  dst_heap_off=269520256/270568832
  sync_remote_off=16464

rank1 symId=1:
  src_heap_off=134253824
  dst_heap_off=403737984/404786560
  sync_remote_off=17488
```

Counters stayed correctness-clean in this diagnostic run:

```text
wr_retx=0 rnr_retx=1 write_gap_rnr=37
dv_hard=0 wr_timeout=0 wr_retry_exhausted=0
reorder_timeout=0 active_timeout=0
data_tx_errors=0 data_tx_posted/completed=4442/4442
```

Interpretation: this falsifies the most direct sync-pool/key-classification
theory for the slow host-stream slot. Even when RCCL labels the scratch buffers
as host-coherent, the rocSHMEM USB4 all-to-all payload posts classify the
source and destination payloads as heap memory and select the same key for heap
and sync in this run. The slow `symId=1` path tracks the `+128MiB` source and
destination payload heap offsets, not a switch to sync-pool payload keys. The
remaining likely bottleneck is below RCCL's scratch backing choice and below
rocSHMEM's lkey/rkey selection: an address/offset effect in the USB4 DV/RDMA
or host-coherent heap mapping path.

### Exchange-Only Scratch Slot A/B

Ran a follow-up discriminator with RCCL host-stream bench mode 2, which skips
copy-in and copy-out and times only the rocSHMEM exchange phase. PyTorch payload
validation was disabled for this run because mode 2 intentionally leaves the
normal all-to-all result incomplete.

```text
RCCL_ROCSHMEM_GDA_BENCH_MODE=2
RCCL_ROCSHMEM_HOST_STREAM_TIMING=1
--torch-validate 0
--source-heap 0 --dest-heap 0
--hoststream-addr-log 1
--usb4-a2a-post-log 4
iters=4
reps=5
```

Fixed `symId=0`:

```text
log: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-exchangeonly-coherent-fixedsym0-writegaprnr1-qptimeout14-20260606-190426
status: pass, 5/5
1MiB/rank avg/max: 19434.4us / 42874.1us
2MiB/rank avg/max: 47255.5us / 109736.5us
msgSize=2097152 exchange_avg/p90/max: 19.284ms / 40.672ms / 48.869ms
msgSize=4194304 exchange_avg/p90/max: 47.113ms / 109.283ms / 115.431ms
wr_retx=0 rnr_retx=126 write_gap_rnr=4186
dv_hard=0 wr_timeout=0 wr_retry_exhausted=0 tx=95979/95979
```

Fixed `symId=1`:

```text
log: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-exchangeonly-coherent-fixedsym1-writegaprnr1-qptimeout14-20260606-190508
status: pass, 5/5
1MiB/rank avg/max: 69791.9us / 144131.7us
2MiB/rank avg/max: 185788.7us / 564498.9us
msgSize=2097152 exchange_avg/p90/max: 69.636ms / 146.422ms / 228.877ms
msgSize=4194304 exchange_avg/p90/max: 185.715ms / 552.793ms / 599.126ms
wr_retx=0 rnr_retx=85 write_gap_rnr=2782
dv_hard=0 wr_timeout=0 wr_retry_exhausted=0 tx=74652/74652
```

The post-key layouts stayed consistent with the previous diagnostic. Fixed
`symId=0` payload posts used `src_heap_off=1084672` on rank 0 and
`src_heap_off=36096` on rank 1; fixed `symId=1` used
`src_heap_off=135302400` on rank 0 and `134253824` on rank 1. In both legs,
payload source/destination addresses classified as heap, the pSync target
classified as remote sync, and lkey/rkey selection remained internally
consistent.

Interpretation: the slot-1 penalty survives after removing host-stream copy-in
and copy-out from the timed path. Mode 2 is not an application-correctness mode,
but as a performance discriminator it tightens the target: the slow slot is in
the rocSHMEM USB4 exchange/address path itself, not in CPU/GPU staging copies
around the scratch buffer.

### Corrected Scratch Slot Spacing Discriminator

Added an RCCL diagnostic knob, `RCCL_ROCSHMEM_NUM_SYM_BUF`, and exposed it in
`tbv_app_gate.sh` as `--rocshmem-num-sym-buf`. The first attempt to run
`numSymBuf=4` was invalid: the configured RCCL build tree regenerated
`hipify/src/init.cc` from a staged source copy under
`/mnt/Home/tmp/rccl-hoststream-timing-src-a9c630`, and that staged copy still
contained the old `#define NUM_SYM_BUF 2`. The app gate forwarded
`RCCL_ROCSHMEM_NUM_SYM_BUF=4`, but the runtime logs still showed
`numSymBuf=2`, with fixed sym IDs 2/3 wrapping back to slots 0/1. Those rows
must not be used for slot-spacing conclusions.

After patching the staged source and rebuilding/installing RCCL, a smoke run
proved the knob end to end:

```text
log: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-exchangeonly-nsym4-fixedsym2-knobsmoke4
status: pass
fixedSymId=2 numSymBuf=4 bufThreshold=67108864 slotOffset=134217728
dv_poll_wqes=4 tx=700/700 wr_timeout=0 wr_retry_exhausted=0
```

Then ran the corrected exchange-only discriminator:

```text
RCCL_ROCSHMEM_GDA_BENCH_MODE=2
RCCL_ROCSHMEM_HOST_STREAM_TIMING=1
RCCL_ROCSHMEM_THRESHOLD=4194304
--torch-validate 0
--source-heap 0 --dest-heap 0
--hoststream-addr-log 1
--usb4-a2a-post-log 4
--pytorch-sizes 1048576
--pytorch-iters 3
--reps 3
```

Corrected same-binary results:

```text
numSymBuf sym slot_offset exchange_avg exchange_p50 exchange_p90 exchange_max torch_avg
2         0   0           19.022ms     13.858ms     47.382ms     47.440ms     34.000ms
2         1   134217728   113.599ms    83.689ms     194.895ms    195.130ms    184.348ms
4         0   0           21.394ms     18.892ms     39.174ms     39.281ms     31.461ms
4         1   67108864    35.635ms     35.999ms     51.094ms     51.138ms     43.031ms
4         2   134217728   62.949ms     44.850ms     146.516ms    146.551ms    106.567ms
4         3   201326592   100.249ms    78.996ms     246.412ms    246.497ms    140.790ms
```

All six corrected roots passed with `wr_timeout=0`, `wr_retry_exhausted=0`,
`dv_hard_error=0`, `data_tx_errors=0`, no `data_rx_no_qp` gate failures, and
state-level TX posted/completed equal. Non-fatal RNR/write-gap and late-ACK
counters still move during these exchange-only runs, but they do not explain
the monotonic slot-offset timing curve.

Interpretation: the slow path follows absolute symmetric-heap slot offset, not
just "the second slot" or sym-ID parity. Splitting the 256MiB scratch region
into four 64MiB slots makes the 64MiB slot intermediate, the 128MiB slot slow,
and the 192MiB slot slower still. The same rebuilt RCCL still reproduces the
two-slot 128MiB penalty. This points below RCCL's sym-ID selection and below
copy-in/copy-out: the next target is the rocSHMEM USB4 address/chunking path or
the thunderbolt-ibverbs mapping/DV post path as heap offsets grow.

### USB4 All-to-All Payload vs Signal/Wait Split

Exposed rocSHMEM's existing USB4 all-to-all phase selector through the app gate:

```text
--usb4-alltoall-mode N    -> ROCSHMEM_GDA_USB4_ALLTOALL_MODE
--usb4-alltoall-ack 0|1   -> ROCSHMEM_GDA_USB4_ALLTOALL_ACK
```

Then re-ran the corrected `numSymBuf=4` exchange-only discriminator with
validation disabled, `RCCL_ROCSHMEM_THRESHOLD=4194304`, `iters=3`, `reps=3`,
`--source-heap 0 --dest-heap 0`, and the same rebuilt RCCL/rocSHMEM installs.
Mode 1 posts only the payload RDMA write; mode 2 performs only the signal/wait
phase.

```text
mode sym slot_offset phase_count exchange_avg exchange_p50 exchange_p90 exchange_max total_avg torch_avg
1    0   0           18          17.331ms     16.932ms     30.313ms     31.729ms     17.336ms  21.787ms
1    1   67108864    18          46.686ms     43.850ms     86.815ms     102.689ms    46.691ms  48.708ms
1    2   134217728   18          74.402ms     52.024ms     200.658ms    249.081ms    74.407ms  52.547ms
1    3   201326592   18          83.095ms     68.271ms     157.248ms    228.393ms    83.101ms  74.744ms
2    0   0           18          0.172ms      0.063ms      0.931ms      0.999ms      0.178ms   0.305ms
2    1   67108864    18          0.175ms      0.073ms      0.821ms      0.843ms      0.181ms   0.323ms
2    2   134217728   18          0.087ms      0.081ms      0.127ms      0.158ms      0.092ms   0.215ms
2    3   201326592   18          0.069ms      0.062ms      0.096ms      0.101ms      0.075ms   0.190ms
```

Log roots:

```text
/mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-usb4mode1-nsym4-fixedsym0-20260606-201348
/mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-usb4mode1-nsym4-fixedsym1-20260606-201417
/mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-usb4mode1-nsym4-fixedsym2-20260606-201442
/mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-usb4mode1-nsym4-fixedsym3-20260606-201506
/mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-usb4mode2-nsym4-fixedsym0-20260606-201530
/mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-usb4mode2-nsym4-fixedsym1-20260606-201554
/mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-usb4mode2-nsym4-fixedsym2-20260606-201618
/mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-usb4mode2-nsym4-fixedsym3-20260606-201642
```

All eight roots passed the app gate with `dv_hard_error=0`,
`wr_timeout=0`, `wr_retry_exhausted=0`, `data_tx_errors=0`, and
state-level TX posted/completed equal. Payload-only mode still has non-fatal
RNR/write-gap/late-ACK movement, but signal/wait-only has no RNR or write-gap
movement and posts only about 512 control/data frames per three-rep root.

Interpretation: the slot-offset penalty is in the payload write leg. The
pSync signal/wait phase is sub-millisecond and essentially flat across the
same slots, so it cannot explain the tens-to-hundreds of milliseconds observed
when the payload RDMA write is enabled. This narrows the next inspection to the
payload `put_nbi_single`/`quiet_single` path and the thunderbolt-ibverbs DV
RDMA WRITE handling for larger heap offsets.

### USB4 Payload Post vs Quiet Timing

Added `ROCSHMEM_GDA_USB4_A2A_TIMING_LOG` in rocSHMEM and exposed it through
the app gate as `--usb4-a2a-timing-log`. The device-side log wraps the USB4
all-to-all payload `put_nbi_single()` and its following `quiet_single()` with
`wall_clock64()` so the payload-only phase can be split into post cost and
completion-wait cost. This is a diagnostic log; in payload-only mode the
all-to-all sequence number does not advance, so a positive timing log count
prints every payload iteration for these short runs.

Ran payload-only mode with the rebuilt RCCL/rocSHMEM installs, `numSymBuf=4`,
validation disabled, `RCCL_ROCSHMEM_THRESHOLD=4194304`, `iters=3`, `reps=3`,
and `--usb4-a2a-post-log 1 --usb4-a2a-timing-log 1`.

```text
sym slot_offset exchange_avg post_avg_combined quiet_avg_combined quiet_p90_max total_avg_combined dirs tx_post tx_comp wr_retx rnr_retx ack_retry write_gap_rnr dv_hard wr_to wr_exh tx_err
0   0           19.543ms     654.7             1912165.4          8695832       1912820.0          2    15342   15342   0       32       511       953           0       0     0      0
1   67108864    55.576ms     580.9             5508802.2          11996532      5509383.1          2    17230   17230   0       39       712       1413          0       0     0      0
2   134217728   63.733ms     746.0             6323129.2          17631188      6323875.2          2    12700   12700   0       24       332       917           0       0     0      0
3   201326592   101.133ms    613.8             10056030.4         17887092      10056644.2         2    13349   13349   0       26       212       1025          0       0     0      0
```

Directional timing, in raw `wall_clock64()` ticks:

```text
sym slot_offset exchange_avg direction src_heap_off dst_heap_off count post_avg quiet_avg quiet_p90 quiet_max total_avg total_p90 total_max
0   0           19.543ms     0->1      1084672      268471680    9     681.8    1025081.8 2347532   2347532   1025763.6 2348384   2348384
0   0           19.543ms     1->0      36096        269520256    9     627.6    2799248.9 8695832   8695832   2799876.4 8696176   8696176
1   67108864    55.576ms     0->1      68193536     335580544    9     634.7    7239489.8 11996532  11996532  7240124.4 11996932  11996932
1   67108864    55.576ms     1->0      67144960     336629120    9     527.1    3778114.7 5002636   5002636   3778641.8 5002964   5002964
2   134217728   63.733ms     0->1      135302400    402689408    9     728.0    6728267.6 17631188  17631188  6728995.6 17631984  17631984
2   134217728   63.733ms     1->0      134253824    403737984    9     764.0    5917990.7 7909720   7909720   5918754.7 7910656   7910656
3   201326592   101.133ms    0->1      202411264    469798272    9     739.6    9536994.7 14567276  14567276  9537734.2 14567696  14567696
3   201326592   101.133ms    1->0      201362688    470846848    9     488.0    10575066.2 17887092  17887092  10575554.2 17887496  17887496
```

Log roots:

```text
/mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-a2atiming-mode1-nsym4-fixedsym0-20260606-203627
/mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-a2atiming-mode1-nsym4-fixedsym1-20260606-203652
/mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-a2atiming-mode1-nsym4-fixedsym2-20260606-203716
/mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-a2atiming-mode1-nsym4-fixedsym3-20260606-203740
```

All four timing roots passed with `dv_hard_error=0`, `wr_timeout=0`,
`wr_retry_exhausted=0`, `data_tx_errors=0`, and state-level TX
posted/completed equal.

Interpretation: `put_nbi_single()` posting is flat and tiny across slots and
directions, while `quiet_single()` accounts for the offset-dependent latency.
That rules out WQE construction, doorbell/post cost, and immediate lkey/rkey
lookup as the source of the slot penalty. The remaining target is the posted
RDMA WRITE completion path: rocSHMEM's wait on the GDA QP, thunderbolt-ibverbs'
DV WR completion path, NHI transfer/completion behavior, or an interaction
with the RNR/write-gap recovery that scales with the absolute heap address.

### USB4 Source vs Destination Slot Split

Added RCCL host-stream diagnostic knobs to decouple the source and destination
scratch slots:

```text
RCCL_ROCSHMEM_HOST_STREAM_FIXED_SRC_SYMID
RCCL_ROCSHMEM_HOST_STREAM_FIXED_DST_SYMID
```

The app gate exposes them as `--hoststream-fixed-src-symid` and
`--hoststream-fixed-dst-symid`. Defaults preserve the old behavior: if these
are unset, both sides use `RCCL_ROCSHMEM_HOST_STREAM_FIXED_SYMID` or the
rotating `comm->symId`.

Smoke root proving the new selectors reached RCCL and rocSHMEM:

```text
/mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-srcdst-smoke-src0-dst3-20260606-205832
srcSymId=0 dstSymId=3 srcSlotOffset=0 dstSlotOffset=201326592
```

Then ran payload-only mode with `numSymBuf=4`, fixed base sym ID 0,
validation disabled, `iters=3`, `reps=3`, and the same rebuilt RCCL/rocSHMEM
installs:

```text
src_sym dst_sym src_slot_offset dst_slot_offset exchange_avg exchange_p50 exchange_p90 exchange_max post_avg_combined quiet_avg_combined quiet_p90_max tx_post tx_comp wr_retx rnr_retx ack_retry write_gap_rnr dv_hard wr_to wr_exh tx_err
0       0       0               0               17.717ms     12.144ms     40.523ms     49.602ms     677.4             1724809.8          4912848       17913   17913   0       42       595       1082          0       0     0      0
3       0       201326592       0               60.608ms     62.503ms     96.732ms     99.077ms     766.7             6011486.9          9853668       9753    9753    0       15       144       385           0       0     0      0
0       3       0               201326592       24.721ms     20.406ms     34.501ms     101.730ms    579.8             2424401.1          10115992      17767   17767   0       42       428       1155          0       0     0      0
3       3       201326592       201326592       85.597ms     69.414ms     173.789ms    177.069ms    709.6             8503472.0          17586604      11387   11387   0       19       354       947           0       0     0      0
```

Directional USB4 timing, in raw `wall_clock64()` ticks:

```text
src dst direction src_heap_off dst_heap_off count post_avg quiet_avg quiet_p90 quiet_max total_avg total_p90 total_max
0   0   0->1      1084672      268471680    9     692.9    1480252.9 2832096   2832096   1480945.8 2833204   2833204
0   0   1->0      36096        269520256    9     661.8    1979366.7 4912848   4912848   1980028.4 4913636   4913636
3   0   0->1      202411264    268471680    9     830.7    7191348.9 9853668   9853668   7192179.6 9854012   9854012
3   0   1->0      201362688    269520256    9     702.7    4831624.9 6857992   6857992   4832327.6 6858792   6858792
0   3   0->1      1084672      469798272    9     672.4    2167304.0 3403700   3403700   2167976.4 3404564   3404564
0   3   1->0      36096        470846848    9     487.1    2691498.2 10115992  10115992  2691985.3 10116416  10116416
3   3   0->1      202411264    469798272    9     768.9    8291212.9 17586604  17586604  8291981.8 17587652  17587652
3   3   1->0      201362688    470846848    9     650.2    8715731.1 17304600  17304600  8716381.3 17305012  17305012
```

Log roots:

```text
/mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-srcdst-src0-dst0-20260606-205933
/mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-srcdst-src3-dst0-20260606-205958
/mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-srcdst-src0-dst3-20260606-210024
/mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-srcdst-src3-dst3-20260606-210049
```

All four roots passed. Post cost remains flat and tiny. The large effect follows
the local source slot: `src3/dst0` is about 3.4x slower than `src0/dst0`, while
`src0/dst3` is only about 1.4x slower than `src0/dst0`. `src3/dst3` is slower
again, so destination offset may add tail latency, but it is not the primary
driver.

Interpretation: the slot penalty is now localized further to the sender-side
payload completion path for RDMA WRITEs whose local source address is high in
the coherent rocSHMEM scratch allocation. The next kernel-side split should
measure DV WQE admission, native WRITE send completion, and DV CQE publication
by WQE local address bucket; remote destination offset is secondary.

### Full Host-Stream Source-Pinning Workaround Check

Re-ran full host-stream mode (`RCCL_ROCSHMEM_GDA_BENCH_MODE=0`) with PyTorch
validation enabled. The first split fixed both source and destination slots.
The second split compared normal rotating slots against only pinning the source
slot to 0 while leaving the destination slot on the normal rotating `symId`.

Fixed source/destination slots, `iters=3`, `reps=3`, validation enabled:

```text
src_sym dst_sym torch_avg_us exchange_avg exchange_p50 exchange_p90 exchange_max copyin_avg copyout_avg post_avg_combined quiet_avg_combined quiet_p90_max tx_post tx_comp wr_retx rnr_retx ack_retry write_gap_rnr dv_hard wr_to wr_exh tx_err
0       0       22442.0      22.916ms     19.672ms     41.709ms     45.639ms     0.033ms    0.035ms     866.7             1502731.4          4117984       20533   20533   0       43       752       1494          0       0     0      0
3       0       87100.7      88.705ms     93.180ms     102.142ms    132.018ms    0.033ms    0.035ms     790.7             7704908.7          9508628       14803   14803   0       24       149       865           0       0     0      0
0       3       20950.4      20.943ms     20.050ms     28.639ms     38.324ms     0.034ms    0.035ms     912.7             2016382.0          2455220       18619   18619   0       30       353       1117          0       0     0      0
3       3       103833.6     106.425ms    102.076ms    135.643ms    164.639ms    0.034ms    0.035ms     791.3             10253089.4         13495832      20109   20109   0       40       314       1863          0       0     0      0
```

Rotating destination, `iters=8`, `reps=3`, validation enabled:

```text
mode          torch_avg_us exchange_by_sym_ms                         total_tx wr_retx rnr_retx ack_retry write_gap_rnr dv_hard wr_to wr_exh tx_err
rotate-both   62826.3      sym0=16.287 sym1=59.021 sym2=84.627 sym3=91.667 39844    0       76       605       3186          0       0     0      0
fixed-src0    22955.3      sym0=14.928 sym1=29.477 sym2=16.367 sym3=31.078 38968    0       77       798       2565          0       0     0      0
```

Log roots:

```text
/mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-full-srcdst-src0-dst0-20260606-210412
/mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-full-srcdst-src3-dst0-20260606-210437
/mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-full-srcdst-src0-dst3-20260606-210507
/mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-full-srcdst-src3-dst3-20260606-210542
/mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-full-rotate-iters8-20260606-210718
/mnt/Home/tmp/tbv-app-gate-logs/pytorch-hoststream-full-src0-dstauto-iters8-20260606-210756
```

Interpretation: the source-slot result survives the real host-stream path with
copy-in, payload exchange, signal/wait, copy-out, and PyTorch validation. Simply
pinning the source scratch slot to 0 while letting the destination slot rotate
reduced the validated 1MiB all-to-all average from about 62.8ms to 23.0ms. This
is not the root-cause fix, but it is a credible short-term app-benchmark
workaround while the kernel/DV path is instrumented by local source address.

### USB4 Payload Chunking

Added a rocSHMEM runtime knob for the USB4 optimized all-to-all payload path:

```text
ROCSHMEM_GDA_USB4_A2A_CHUNK_BYTES
```

The default is `0`, which preserves the previous single-`put_nbi_single()`
payload WRITE. When the knob is non-zero and smaller than the payload, the
device path splits the payload into multiple `put_nbi_single(..., signal=true)`
WRITEs and keeps the existing single `quiet_single()` after the chunk loop.
The app gate now exposes the knob as:

```text
--usb4-a2a-chunk-bytes N
```

Because RCCL statically links rocSHMEM into `librccl.so.1.0`, rebuilding only
rocSHMEM is not enough for PyTorch/RCCL runs. The active rebuilt installs were:

```text
rocSHMEM: /mnt/Home/tmp/rocshmem-waitbudget-install
RCCL:     /mnt/Home/tmp/rccl-hoststream-waitbudget-install
```

`strings` on the rebuilt RCCL confirmed that `librccl.so.1.0` contains
`ROCSHMEM_GDA_USB4_A2A_CHUNK_BYTES`.

Payload-only 4 MiB discriminator, `ROCSHMEM_GDA_USB4_ALLTOALL_MODE=1`,
validation disabled, `iters=4`, `reps=2`:

```text
chunk   log root                                                          avg_us   min_us   max_us   wr_retx rnr_retx ack_retry late_ack dup_ack write_gap_rnr hard
1MiB    /mnt/Home/tmp/tbv-app-gate-logs/pytorch-chunk1m-payload-4m-20260606-2232   46655.9  5856.4  87455.4  0       5        24        152      4       123           0
512KiB  /mnt/Home/tmp/tbv-app-gate-logs/pytorch-chunk512k-payload-4m-20260606-2235  7001.0   5564.1  8437.9   0       5        5         218      5       79            0
256KiB  /mnt/Home/tmp/tbv-app-gate-logs/pytorch-chunk256k-payload-4m-20260606-2237  25989.5  6511.0  45468.1  1       4        5         474      69      19            0
```

The 512 KiB chunk is the best tested point so far: it keeps the fast run near
5.6ms and collapses the two-rep average from the old unchunked payload-only
4 MiB baseline of about 52.0ms to about 7.0ms. The 1 MiB chunk can be fast, but
still has an 87ms tail when write-gap/RNR recovery appears. The 256 KiB chunk
increases ACK/retry noise and produced one recovered WR retransmit, so smaller
chunks are not automatically better.

Validated full PyTorch hoststream runs with 512 KiB chunks:

```text
case             log root                                                             avg_us   min_us   max_us   wr_retx rnr_retx ack_retry late_ack dup_ack write_gap_rnr tx_post/tx_comp hard
4MiB single-size  /mnt/Home/tmp/tbv-app-gate-logs/pytorch-chunk512k-full-4m-validate-20260606-2240   11943.0  8834.9  15051.1  0       7        38        352      6       108           23504/23504   0
16MiB single-rep  /mnt/Home/tmp/tbv-app-gate-logs/pytorch-chunk512k-full-16m-validate-20260606-2242  65556.7  65556.7 65556.7  0       5        39        284      3       94            27217/27217   0
```

The previous SG-cursor-fixed full baseline was about 82.1ms at 4 MiB and
about 1.02s at 16 MiB. With 512 KiB chunks, the single-size validated runs
improved those to about 11.9ms and 65.6ms respectively.

The final validated sweep used 512 KiB chunks, sizes 1/4/16 MiB, `iters=4`,
`reps=2`, and validation enabled:

```text
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-chunk512k-full-sweep-20260606-2244

bytes     avg_us   min_us   max_us   gbps_max
1MiB      2822.4   2585.7   3059.2   3.24
4MiB      6693.6   5966.7   7420.5   5.62
16MiB     29032.8  27306.1  30759.6  4.92

aggregate counters:
wr_retx=0 rnr_retx=22 ack_retry=26 late_ack=1420 dup_ack=20
write_gap_rnr=359 dv_hard=0 wr_timeout=0 wr_retry_exhausted=0
data_tx_errors=0 data_tx_posted=119662 data_tx_completed=119662
```

Interpretation: the large-message bottleneck after the SG cursor fix was
strongly tied to posting one very large USB4 all-to-all payload WRITE. Splitting
the payload into 512 KiB WRITEs bounds the completion tail enough to make
validated PyTorch hoststream 4 MiB and 16 MiB all-to-all usable for the next
application-level gates. It does not eliminate the underlying native
write-gap/RNR recovery path; those counters still move, but they now recover
without hard errors, timeouts, TX errors, or TX completion leaks in the tested
matrix.

Longer durability check, same 512 KiB chunking, validated 4/16 MiB,
`iters=4`, `reps=5`:

```text
log root: /mnt/Home/tmp/tbv-app-gate-logs/pytorch-chunk512k-full-4m16m-reps5-20260606-2253

bytes     count avg_us   min_us   max_us   gbps_max
4MiB      5     5693.3   4306.1   7224.5   7.79
16MiB     5     22828.4  20228.1  27333.5  6.64

aggregate counters:
wr_retx=0 rnr_retx=29 ack_retry=30 late_ack=2956 dup_ack=28
write_gap_rnr=261 dv_hard=0 wr_timeout=0 wr_retry_exhausted=0
data_tx_errors=0 data_tx_posted=279698 data_tx_completed=279698
```

This makes 512 KiB chunking the current app-benchmark candidate setting. The
remaining risk is not correctness in this sample; it is that the non-fatal
write-gap/RNR path still activates under load and may show longer tails at
larger scale or under vLLM-style overlap.

### Reusable vLLM TP=2 Smoke

Added `tbv_vllm_smoke.sh` to the bench tools. It regenerates a tiny local
Qwen3-shaped dummy model from the cached `Qwen/Qwen3-4B` tokenizer/config,
starts a two-host Ray cluster, runs `vllm bench throughput`, records logs, and
cleans Ray up. The default model path is:

```text
/mnt/Home/tmp/tbv-vllm-tiny-qwen3-gda
```

Two setup details are load-bearing:

1. Do not prepend the TheRock SDK `lib` directory to `LD_LIBRARY_PATH` for
   vLLM. Doing so caused an immediate loader-time SIGSEGV, matching the earlier
   HSA-runtime-mixing warning. The runner prepends only the rebuilt RCCL and
   rocSHMEM install `lib` directories and leaves the vLLM wrapper's ROCm runtime
   in control.
2. Pin the CPU-side distributed interface with `GLOO_SOCKET_IFNAME=eno1`
   alongside `NCCL_SOCKET_IFNAME=eno1`/`RCCL_SOCKET_IFNAME=eno1`. Without the
   Gloo pin, TP=2 Ray startup formed a two-node cluster but failed the CPU
   process group by trying to connect to `127.0.0.x` across hosts.

The tiny model generation also needs `vocab_size=len(tokenizer)`, not
`tokenizer.vocab_size`, because the Qwen tokenizer's special token IDs are
above the base vocabulary size. `--runner generate` is required to keep vLLM
on the causal generation path instead of auto-converting the tiny config to an
embedding runner.

Passing scripted run:

```text
log root: /mnt/Home/tmp/tbv-vllm-smoke/gda-chunk512-20260606-2355
hosts: 192.168.23.136,192.168.23.192
RCCL: /mnt/Home/tmp/rccl-hoststream-waitbudget-install
rocSHMEM: /mnt/Home/tmp/rocshmem-waitbudget-install
ROCSHMEM_GDA_USB4_A2A_CHUNK_BYTES=524288
```

Results:

```text
single-node preflight:
  elapsed_time=0.0822s requests_per_second=24.33 tokens_per_second=291.94

Ray TP=2:
  elapsed_time=0.2791s requests_per_second=7.16 tokens_per_second=85.98
  world_size=2
  rank 0: 192.168.23.136 TP rank 0
  rank 1: 192.168.23.192 TP rank 1
  vLLM nccl version log: 2.28.3
```

Interpretation: the current Strix pair can run a reproducible two-node vLLM
TP=2 lifecycle smoke with the rebuilt RCCL/rocSHMEM libraries selected. This is
not evidence that a real vLLM workload is using the USB4 all-to-all GDA fast
path; the tiny dense Qwen smoke mostly validates vLLM/Ray/NCCL process
lifecycle, library selection, and interface binding. The next application-level
GDA benchmark still needs either an MoE workload that issues all-to-all or a
vLLM/RCCL path where the relevant collective routes through rocSHMEM/GDA.

### rocSHMEM Example Runner and LL_MoE Triage

Added `tbv_rocshmem_example.sh` to make upstream rocSHMEM examples repeatable
on the Strix pair. The runner sets the same USB4 GDA/OpenMPI environment used
by the app gate, forces `ROCSHMEM_BACKEND=gda`,
`ROCSHMEM_GDA_PROVIDER=usb4`, captures before/after
`/sys/kernel/debug/thunderbolt_ibverbs/summary`, and records per-host counter
deltas.

The existing rocSHMEM build was reconfigured with `BUILD_EXAMPLES=ON` and these
examples were built from the current GDA rocSHMEM tree:

```text
/mnt/Home/tmp/rocshmem-qptimeout-build-20260606-073230/examples/rocshmem_alltoall_test
/mnt/Home/tmp/rocshmem-qptimeout-build-20260606-073230/examples/rocshmem_put_signal_test
/mnt/Home/tmp/rocshmem-qptimeout-build-20260606-073230/examples/rocshmem_getmem_test
/mnt/Home/tmp/rocshmem-qptimeout-build-20260606-073230/examples/LL_MoE/ll_moe
```

Primitive USB4 GDA examples all passed cleanly:

```text
case        log root                                                    result  key deltas
alltoall    /mnt/Home/tmp/tbv-rocshmem-example/alltoall-debug-20260606-231654  PASS    dv_poll=+4 tx=+12/+12 ack_match=+4 hard=0
put_signal  /mnt/Home/tmp/tbv-rocshmem-example/put-signal-20260606-231712      PASS    dv_poll=+4 tx=+13/+13 ack_match=+4 hard=0
getmem      /mnt/Home/tmp/tbv-rocshmem-example/getmem-20260606-231737          PASS    dv_poll=+2 tx=+6/+6   hard=0
```

The debug all-to-all run confirmed real USB4 GDA selection and posting:
both PEs selected `usb4_rdma0`, the vendor string was
`rocSHMEM_v3.4.0_GDA(USB4)`, USB4 routes were created, and
`USB4_GDA_A2A_POST`/`USB4_GDA_A2A_TIMING` records appeared before both ranks
reported `[PASS]`.

The LL_MoE example is not a valid benchmark yet. Even the smallest two-host
case timed out:

```text
log root: /mnt/Home/tmp/tbv-rocshmem-example/ll-moe-min-20260606-231753
command: ll_moe -n 4 -h 64 -k 1 -e 2 -i 1 -m d
result: timeout after rank init
counter deltas: dv_poll=+8 tx=+24/+24 ack_match=+8 hard=0
```

That run proves the transport work it did issue completed cleanly. The stronger
local controls then scoped the hang away from USB4 transport:

```text
case              log root                                                    result
np=1 e=1 i=1      /mnt/Home/tmp/tbv-rocshmem-example/ll-moe-np1-20260606-231854     timeout, zero transport deltas
np=1 e=4 i=1      /mnt/Home/tmp/tbv-rocshmem-example/ll-moe-np1-e4-20260606-232004  timeout, zero transport deltas
np=1 e=4 i=0      /mnt/Home/tmp/tbv-rocshmem-example/ll-moe-i0-20260606-232107      PASS
```

Interpretation: rocSHMEM USB4 GDA primitives are usable as app-level probes,
but the upstream LL_MoE example hangs in its first `ll_dispatch()` kernel even
with one local PE and no transport traffic. Treat LL_MoE as an example-kernel
debug target, not as a GDA transport benchmark, until its dispatch-side GPU
synchronization/protocol issue is fixed.
