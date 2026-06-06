# 2026-06-04 GDA Correctness Ledger

Goal: turn the current GDA workaround into a falsifiable correctness model.

## 2026-06-05 Stop Marker: Kernel Patch Stack Was Incomplete

New information supersedes the evidentiary weight of the hardware rows below:
the Strix machines may not have been booted with the full intended
Thunderbolt kernel patch stack. PR #24 (`codex/tb-upstream-patch-stack`) adds
the portable upstream Thunderbolt patch series back to this repo. Two other
pending correctness PRs are also relevant before retesting:

- PR #22 (`grw/reliability-engine-spine`): substantial module rewrite,
  reliability engine spine, configfs link model, and native RC ACK retry /
  correctness hardening.
- PR #23 (`grw/native-qpn-no-reuse`): robust native RC send teardown and QPN
  reuse/churn coverage.

Treat all rows below this marker as pre-corrected-stack baselines only. They
remain useful for hypotheses and instrumentation design, but they should not be
used as final evidence about GDA correctness until the branch is rebased onto
the PR #24 + PR #22 + PR #23 stack, the Strix hosts are rebuilt/rebooted with
that kernel/module set, and the minimal matrix is rerun from a clean boot.

## Current Known-Good Envelope

These statements are directly observed after both Strix hosts were rebooted
with `iommu=pt` and the patched module was reloaded.

1. `strix-1` and `strix-2` both boot `7.1.0-rc1`, expose `/dev/kfd` and
   `/dev/dri/renderD128`, and have two native rails in `data_ready=1`.
2. With `zcopy_min_bytes=0`, `write_zcopy=N`, `native_tx_max_inflight=8`,
   `send_ack_redundancy=3`, and `native_fragment_striping=N`, 64 KiB RDMA
   WRITE visibility from `strix-2` into `strix-1` passes for both
   `host-uncached` and `host-coherent` receiver memory.
3. In those passing 64 KiB runs, the sender completes every native TX
   descriptor it posts: `data_tx_posted=4608` and `data_tx_completed=4608`.
4. In those passing 64 KiB runs, sender WR completion is clean:
   `data_wr_send=512`, `data_wr_timeout=0`, `data_wr_copied=512`,
   `data_wr_zcopy=0`, and no verbs objects remain live after process exit.
5. Delayed duplicate ACKs can arrive after sender QP teardown. This is visible
   as `data_rx_no_qp` on the sender, but did not cause the two passing 64 KiB
   runs to fail.

## Historical Hard Facts

These are from the same revive branch before the current workaround.

1. A 64 KiB WRITE zcopy run failed with sender timeout and left raw-stream TX
   state stuck: `zcopy_inflight=3`, `raw_active=1`.
2. Disabling WRITE zcopy was not enough. A copied 64 KiB run still failed with
   all receiver WRITE frames accepted, but the sender had one missing local TX
   completion: `data_tx_posted=1962`, `data_tx_completed=1961`,
   `path_tx inflight=1`, `data_wr_timeout=1`.
3. Adding redundant immediate ACKs without TX throttling did not fix copied
   64 KiB. It increased observed ACK frames, but the sender still timed out
   and had missing TX completions.
4. Capping copied payload fragments at `TBV_NATIVE_DATA_MAX_PAYLOAD - 1` did
   not fix copied 64 KiB. It still failed with missing sender TX completions.
5. Adding `native_tx_max_inflight=8` without delayed ACK retries fixed the
   sender local TX completion leak in one run, but the WR still timed out with
   missing ACKs.

## Candidate Failure Classes

### A. WRITE zcopy/raw-stream lifetime bug

Claim: native WRITE zcopy can corrupt or stall raw-stream state independently
of ACK handling.

Predictions:

- Enabling `write_zcopy=1` with `zcopy_min_bytes=4096` should reintroduce
  `data_wr_zcopy > 0`.
- If this class is still present, a failure should show `zcopy_inflight > 0`
  or `raw_active=1` after timeout.
- If TX throttling alone fixes it, then zcopy should pass with
  `native_tx_max_inflight=8` and ACK redundancy enabled.

### B. Native TX completion pressure bug

Claim: native data TX descriptors can stop completing when too many are posted
on one path, independent of payload visibility.

Predictions:

- With `native_tx_max_inflight=0`, copied 64 KiB should be able to reproduce
  `data_tx_posted > data_tx_completed` and `path_tx inflight > 0`.
- With `native_tx_max_inflight=8`, the same copied 64 KiB run should leave
  `data_tx_posted == data_tx_completed` and `path_tx inflight=0`.
- ACK redundancy cannot repair this class; if TX completions are missing, WR
  timeout is expected even when ACK counters increase.

### C. SEND_ACK delivery loss/reorder bug

Claim: copied WRITE data reaches the receiver and local TX completes on the
sender, but the sender sometimes misses the SEND_ACK that completes the WR.

Predictions:

- With `native_tx_max_inflight=8` and `send_ack_redundancy=1`, copied 64 KiB
  can fail with `data_tx_posted == data_tx_completed` but insufficient
  `data_rx_ack` for the signaled writes.
- With `send_ack_redundancy=3`, the same run passes, but sender
  `data_rx_ack` exceeds the number required and late duplicates may appear as
  `data_rx_no_qp`.
- If `send_ack_redundancy=1` passes repeatedly, this claim is weakened and the
  earlier missing-ACK observation may have been coupled to another condition.

### D. GPU visibility/coherence bug

Claim: RDMA WRITE reaches host memory but the GPU does not observe the signal
or payload correctly.

Predictions:

- Receiver should time out while sender has clean WR completion and no driver
  errors.
- Counter evidence should not show missing TX completions or missing ACKs.
- This claim is contradicted when both host-coherent and host-uncached
  receiver memory pass with matching sequence checks and clean driver state.

Current status: contradicted for the known-good envelope. It may still apply
outside that envelope, but it is not the active 64 KiB failure signature.

## Test Matrix

All tests use `strix-1` receiver, `strix-2` sender, `usb4_rdma5`, GID index 1,
64 KiB WRITE, count 256, and a clean module reload before each row.

| ID | zcopy | tx limit | ack redundancy | Expected discriminator |
| --- | --- | --- | --- | --- |
| C1 | off | 8 | 1 | Is ACK redundancy actually required? |
| C2 | off | 8 | 3 | Known-good control after reload. |
| B1 | off | 0 | 3 | Does unthrottled copied TX still leak completions? |
| A1 | on | 8 | 3 | Does WRITE zcopy still break raw-stream lifetime? |

## Results

### C1: copied WRITE, TX limit 8, ACK redundancy 1

Command shape: `host-uncached`, 64 KiB, count 256.

Result: FAIL.

```text
sender:
send wc error wr_id=133 status=12 opcode=1 byte_len=0

receiver:
timeout waiting gpu_seen=67 got=66 signal=66
```

Post-run counters:

```text
strix-2 sender:
data_wr_send=132 data_wr_copied=132 data_wr_zcopy=0
data_wr_timeout=2
data_tx_posted=1188 data_tx_completed=1188
data_rx_ack=130
path_tx inflight=0 zcopy_inflight=0 raw_active=0

strix-1 receiver:
data_rx_completed=1188 data_rx_op_write=1188
data_tx_posted=169 data_tx_completed=169
path_tx inflight=0 zcopy_inflight=0 raw_active=0
```

Irrefutable statement: C1 is not a sender native TX completion failure. Every
sender TX descriptor completed, but two WRs timed out and the sender observed
two fewer ACKs than WRs.

Contradiction: this rules out "TX throttling alone is sufficient" for the
copied 64 KiB path. It also contradicts a pure GPU visibility failure: the
driver stopped because WR ACK completion was missing, not because the sender
completed cleanly while the GPU failed to observe memory.

Post-watchdog redeploy repeat: FAIL with the same signature.

```text
sender:
send wc error wr_id=133 status=12 opcode=1 byte_len=0

receiver:
timeout waiting gpu_seen=67 got=66 signal=66

strix-2 sender:
data_wr_send=132 data_wr_copied=132 data_wr_zcopy=0
data_wr_timeout=2
data_tx_posted=1188 data_tx_completed=1188
data_rx_ack=130
path_tx inflight=0 zcopy_inflight=0 raw_active=0
```

The repeat strengthens the ACK-delivery claim: the failure is stable across
reboot and watchdog hardening.

### C2: copied WRITE, TX limit 8, ACK redundancy 3

Command shape: `host-uncached`, 64 KiB, count 256.

Result: PASS.

```text
send_result ... size=65536 count=256 status=OK elapsed_sec=0.096421 avg_us=376.645
recv_result kind=host-uncached mode=normal size=65536 count=256 status=OK elapsed_sec=0.096454 avg_us=376.773
```

Post-run counters:

```text
strix-2 sender:
data_wr_send=512 data_wr_copied=512 data_wr_zcopy=0
data_wr_timeout=0
data_tx_posted=4608 data_tx_completed=4608
data_rx_ack=1508 data_rx_no_qp=28
path_tx inflight=0 zcopy_inflight=0 raw_active=0

strix-1 receiver:
data_rx_completed=4608 data_rx_op_write=4608
data_tx_posted=1680 data_tx_completed=1680
path_tx inflight=0 zcopy_inflight=0 raw_active=0
```

Irrefutable statement: changing only ACK redundancy from 1 to 3 converted the
same copied 64 KiB test from timeout to pass in this run sequence.

Contradiction: a model that only blames receiver GPU coherence cannot explain
C1 versus C2, because the receiver memory kind and data path were unchanged.

### B1: copied WRITE, no TX limit, ACK redundancy 3

Command shape: `host-uncached`, 64 KiB, count 256.

Result: PASS.

```text
send_result ... size=65536 count=256 status=OK elapsed_sec=0.094623 avg_us=369.622
recv_result kind=host-uncached mode=normal size=65536 count=256 status=OK elapsed_sec=0.094661 avg_us=369.768
```

Post-run counters:

```text
strix-2 sender:
data_wr_send=512 data_wr_copied=512 data_wr_zcopy=0
data_wr_timeout=0
data_tx_posted=4608 data_tx_completed=4608
data_rx_ack=1504 data_rx_no_qp=32
path_tx inflight=0 zcopy_inflight=0 raw_active=0
```

Irrefutable statement: after reboot, copied 64 KiB does not require
`native_tx_max_inflight=8` for every 256-count run. Unthrottled copied TX can
complete cleanly when delayed ACK redundancy is enabled.

Contradiction: the strong claim "unthrottled copied TX always leaks local
completions" is false.

### B2: copied WRITE, no TX limit, ACK redundancy 3, stress

Command shape: `host-uncached`, 64 KiB, count 1024.

Result: PASS.

```text
send_result ... size=65536 count=1024 status=OK elapsed_sec=0.349227 avg_us=341.042
recv_result kind=host-uncached mode=normal size=65536 count=1024 status=OK elapsed_sec=0.349261 avg_us=341.075
```

Post-run counters:

```text
strix-2 sender:
data_wr_send=2048 data_wr_copied=2048 data_wr_zcopy=0
data_wr_timeout=0
data_tx_posted=18432 data_tx_completed=18432
data_rx_ack=6118 data_rx_no_qp=26
path_tx inflight=0 zcopy_inflight=0 raw_active=0

strix-1 receiver:
data_rx_completed=18432 data_rx_op_write=18432
data_tx_posted=6720 data_tx_completed=6720
path_tx inflight=0 zcopy_inflight=0 raw_active=0
```

Irrefutable statement: unthrottled copied TX can also survive a larger
1024-count run with clean TX completion on this rebooted state.

Contradiction: the earlier copied-path TX completion leak is not reproduced by
removing the TX limit alone once delayed ACK retries are present. It is either
nondeterministic, coupled to another condition, or was influenced by an older
diagnostic patch/stale ring state.

### A1-small: WRITE zcopy enabled, TX limit 8, ACK redundancy 3

Command shape: `host-uncached`, 64 KiB, count 64,
`zcopy_min_bytes=4096`, `write_zcopy=1`.

Result: FAIL.

```text
sender:
send wc error wr_id=27 status=12 opcode=1 byte_len=0
ibv_destroy_qp: Connection timed out
ibv_destroy_cq: Device or resource busy
ibv_dealloc_pd: Device or resource busy

receiver:
timeout waiting gpu_seen=13 got=12 signal=12
```

Post-run counters:

```text
strix-2 sender:
verbs_pds=1 verbs_cqs=1 verbs_qps=1 verbs_mrs=1
data_wr_send=26 data_wr_copied=13 data_wr_zcopy=13
data_wr_live=1 data_wr_timeout=1
data_tx_posted=246 data_tx_completed=244
data_tx_credit_received=224 data_rx_ack=75
path_tx inflight=2 zcopy_inflight=2 raw_active=1

strix-1 receiver:
data_rx_completed=246 data_rx_op_write=233
data_tx_posted=82 data_tx_completed=82
path_tx inflight=0 zcopy_inflight=0 raw_active=0
```

Recent `strix-2` dmesg:

```text
thunderbolt_ibverbs: QP 2304 destroy timed out with 2 refs; leaving it closing for retry
WARNING: drivers/infiniband/core/rdma_core.c:957 at uverbs_destroy_ufile_hw+0xd3/0xf0 [ib_uverbs]
```

Recovery attempt:

```text
thunderbolt_ibverbs 249856 -1 - Unloading ... (O-)
rmmod thunderbolt_ibverbs in D state
```

Irrefutable statement: WRITE zcopy is an independent hard correctness bug. It
fails even with TX limit 8 and ACK redundancy 3, leaves zcopy/raw-stream state
active, leaks verbs objects, and can wedge module unload.

Contradiction: ACK redundancy is not enough to make zcopy safe. TX throttling
is not enough to make zcopy safe. The zcopy failure signature is different from
C1: C1 had complete sender TX and no live objects; A1-small has missing sender
TX completions, `zcopy_inflight`, `raw_active`, and leaked QP refs.

Code-level explanation:

- `tbv_qp_timeout_work()` times out the pending send and calls
  `tbv_cancel_send_ctx_packets()`.
- `tbv_cancel_send_ctx_packets()` maps the send context to the pinned rail and
  calls `tbv_path_cancel_data_owner_ctx()`.
- `tbv_path_cancel_data_match()` deliberately refuses to cancel queued or
  inflight packets whose `owner_ctx` is the currently active raw-stream owner:

```text
if (active_raw_owner && packet->owner_ctx == active_raw_owner)
        continue;
```

That rule protects framing for a raw stream that can still drain. In A1-small,
however, two zcopy TX completions never arrived. The rule therefore preserved
the active stream forever, left `tbv_send_tx_done()` callbacks uncalled for the
inflight zcopy packets, and kept QP refs live. This explains the userspace
destroy failures, the `QP 2304 destroy timed out with 2 refs` dmesg, and the
subsequent stuck module unload.

Recovery status: after `sudo systemctl reboot`, `strix-2` remained pingable but
SSH on port 22 returned `Connection refused`. This matches the earlier
post-wedge recovery symptom and requires console or out-of-band recovery before
more two-host tests.

## Updated Model

1. Copied 64 KiB has a reproducible ACK-delivery weakness. With TX limit 8 and
   ACK redundancy 1, the sender can miss ACKs despite complete local TX.
2. Delayed ACK redundancy fixes the copied 64 KiB failure in the tested rows,
   but introduces late duplicate ACK noise (`data_rx_no_qp`) after sender QP
   teardown.
3. WRITE zcopy/raw-stream has a separate lifetime bug. It cannot be treated as
   a performance option until `zcopy_inflight` and `raw_active` are guaranteed
   to drain or cancel under timeout/error.
4. The earlier unthrottled copied TX completion leak is not reproduced by
   removing `native_tx_max_inflight` alone after reboot and delayed ACK retries.
   Keep the TX limit as a conservative guard for now, but it is no longer the
   strongest active root-cause candidate for copied WRITE.

## SEND_ACK Observability Run

Added debugfs counters for SEND_ACK-only accounting:

```text
receiver side:
data_tx_send_ack
data_tx_send_ack_ok
data_tx_send_ack_error
data_tx_send_ack_retry
data_tx_send_ack_last_psn
data_tx_send_ack_error_last_psn

sender side:
data_rx_send_ack
data_rx_send_ack_matched
data_rx_send_ack_unmatched
data_rx_send_ack_error_status
data_rx_send_ack_last_psn
data_rx_send_ack_unmatched_last_psn

timeout side:
data_wr_send_timeout
data_wr_timeout_last_psn
```

These counters do not change protocol behavior. They separate:

- receiver wanted to send a SEND_ACK,
- local ACK control-frame software enqueue succeeded or failed,
- sender received a SEND_ACK frame,
- sender matched that ACK to an outstanding WR,
- sender timed out a WR and the last timeout PSN.

Important limitation: `data_tx_send_ack_ok` is not a wire-delivery counter. It
increments when `tbv_send_control_frame_on_path()` returns 0. For the native
path that means the frame was copied into the path's software
`tx_control_queue` and TX scheduling was requested. It does not prove the frame
was posted to the Thunderbolt ring, completed by the ring, delivered on the
wire, or decoded by the peer RX path.

### ACK redundancy 3 validates the counters

Command shape: 64 KiB, count 70, `native_tx_max_inflight=8`,
`send_ack_redundancy=3`.

Result: PASS.

```text
strix-1 receiver:
data_tx_send_ack=420 data_tx_send_ack_ok=420 data_tx_send_ack_error=0
data_tx_send_ack_retry=280

strix-2 sender:
data_wr_send=140 data_wr_timeout=0
data_rx_send_ack=420
data_rx_send_ack_matched=140
data_rx_send_ack_unmatched=280
data_rx_no_qp=30
```

Irrefutable statement: for 140 WRs, redundancy 3 queued 420 SEND_ACK frames in
the receiver's software control path. All 420 were observed by the sender.
Exactly 140 matched pending WRs; the rest were duplicate/late ACKs, including
some after QP teardown.

### ACK redundancy 1, TX pressure 8, original shape

Command shape: 64 KiB, count 256, `native_tx_max_inflight=8`,
`send_ack_redundancy=1`.

Result: FAIL.

```text
sender:
send wc error wr_id=23 status=12 opcode=1 byte_len=0

receiver:
timeout waiting gpu_seen=12 got=11 signal=11

strix-1 receiver:
data_rx_completed=198 data_rx_op_write=198
data_tx_send_ack=22 data_tx_send_ack_ok=22 data_tx_send_ack_error=0
data_tx_send_ack_last_psn=14049791

strix-2 sender:
data_wr_send=22 data_wr_send_timeout=2 data_wr_timeout_last_psn=14049791
data_tx_posted=198 data_tx_completed=197
data_rx_send_ack=20 data_rx_send_ack_matched=20
data_rx_send_ack_unmatched=0 data_rx_no_qp=0
```

Irrefutable statement: the receiver accepted all WRITE frames it saw and
successfully queued 22 SEND_ACK frames in software. The sender observed only 20
SEND_ACK frames. The two missing ACKs correspond to the payload/signal WR pair
for the timed-out sequence. This run also had one missing sender TX completion.

### ACK redundancy 1, lower TX pressure

Command shape: 64 KiB, count 256, `send_ack_redundancy=1`.

Results:

```text
native_tx_max_inflight=1: PASS
sender data_wr_send=512 data_wr_timeout=0
sender data_tx_posted=4608 data_tx_completed=4608
sender data_rx_send_ack=512 data_rx_send_ack_matched=512
receiver data_tx_send_ack=512 data_tx_send_ack_ok=512

native_tx_max_inflight=2: PASS
sender data_wr_send=512 data_wr_timeout=0
sender data_tx_posted=4608 data_tx_completed=4608
sender data_rx_send_ack=512 data_rx_send_ack_matched=512
receiver data_tx_send_ack=512 data_tx_send_ack_ok=512

native_tx_max_inflight=4: PASS
sender data_wr_send=512 data_wr_timeout=0
sender data_tx_posted=4608 data_tx_completed=4608
sender data_rx_send_ack=512 data_rx_send_ack_matched=512
receiver data_tx_send_ack=512 data_tx_send_ack_ok=512
```

Irrefutable statement: reducing native TX pressure can convert the ACK=1
256-count run from fail to pass with exact sender-side ACK observation and
complete local TX.

This is not a root-cause explanation. It is a discriminator and a possible
temporary containment mechanism.

### ACK redundancy 1, TX pressure 6

Command shape: 64 KiB, count 256, `native_tx_max_inflight=6`,
`send_ack_redundancy=1`.

Result: FAIL.

```text
sender:
send wc error wr_id=243 status=12 opcode=1 byte_len=0

receiver:
timeout waiting gpu_seen=122 got=121 signal=121

strix-1 receiver:
data_rx_completed=2178 data_rx_op_write=2178
data_tx_send_ack=242 data_tx_send_ack_ok=242 data_tx_send_ack_error=0
data_tx_send_ack_last_psn=8876261

strix-2 sender:
data_wr_send=242 data_wr_send_timeout=2 data_wr_timeout_last_psn=8876261
data_tx_posted=2178 data_tx_completed=2178
data_rx_send_ack=240 data_rx_send_ack_matched=240
data_rx_send_ack_unmatched=0 data_rx_no_qp=0
```

Irrefutable statement: SEND_ACK loss can occur even when sender native TX
completion is perfect. The receiver queued 242 ACK control frames in software;
the sender received only 240. Therefore a local sender TX completion leak is
not required for the copied-WR ACK failure.

Contradiction: the model "copied 64 KiB fails only because sender data TX
completions leak" is false.

### Existing-counter cheap check after critique

Command shape: no rebuild, 64 KiB, count 256,
`native_tx_max_inflight=6`, `send_ack_redundancy=1`.

Result: FAIL.

```text
sender:
send wc error wr_id=329 status=12 opcode=1 byte_len=0

receiver:
timeout waiting gpu_seen=165 got=164 signal=164

strix-1 receiver:
data_tx_send_ack=328 data_tx_send_ack_ok=328 data_tx_send_ack_error=0
data_tx_send_ack_last_psn=3455057
data_tx_posted=420 data_tx_completed=420 data_tx_errors=0
data_rx_repost_failed=0 data_rx_bad_frame=0 data_rx_bad_header=0
data_rx_credit_sent=2944 data_rx_credit_send_error=0

strix-2 sender:
data_wr_send=328 data_wr_send_timeout=1 data_wr_timeout_last_psn=3455057
data_tx_posted=2952 data_tx_completed=2951 data_tx_errors=0
data_rx_send_ack=327 data_rx_send_ack_matched=327
data_rx_send_ack_unmatched=0 data_rx_no_qp=0
data_rx_repost_failed=0 data_rx_bad_frame=0 data_rx_bad_header=0
data_tx_credit_received=2944
```

Irrefutable statement: this run did not show sender RX repost failure,
mis-decoded RX frames, bad native headers, or receiver state-level TX
post-without-completion. The receiver software-queued 328 SEND_ACKs and its
state-level TX completions drained. The sender observed only 327 SEND_ACKs.

Limitation: this row is not as clean as the previous `tx=6` row because sender
native TX completions also ended short by one (`2952/2951`). It still rules out
several cheap existing-counter explanations, but it does not by itself isolate
ACK loss from sender TX completion pressure.

### Receiver TX completion versus sender RX callback

Code facts:

- `tbv_path_tx_complete()` increments state-level `data_tx_completed` for both
  control and data frames. The per-path `data_tx_completed` excludes control,
  but the state-level counter does not.
- `tbv_path_rx_complete()` increments state-level `data_rx_completed` before
  native header decode, QP lookup, or ACK matching.
- Native defaults start with `e2e=false`, but `tbv_peer_add_rail()` enables
  `RING_FLAG_E2E` and sets `path_cfg.e2e=true` for native Linux peers. Live
  debugfs confirmed native rails with `tx_flags=0x6`, `rx_flags=0x6`,
  `e2e=1`, and `wire_flags=0x3`.

Fresh command shape: no rebuild, 64 KiB, count 256,
`native_tx_max_inflight=6`, `send_ack_redundancy=1`, device `usb4_rdma0`
after reload reset device numbering.

Result: FAIL.

```text
sender:
send wc error wr_id=37 status=12 opcode=1 byte_len=0

receiver:
timeout waiting gpu_seen=19 got=18 signal=18

strix-1 receiver:
data_rx_completed=324
data_rx_credit_sent=320
data_tx_send_ack=36 data_tx_send_ack_ok=36 data_tx_send_ack_error=0
data_tx_posted=46 data_tx_completed=46 data_tx_canceled=0 data_tx_errors=0
data_rx_repost_failed=0 data_rx_bad_frame=0 data_rx_bad_header=0
path_cfg e2e=1 tx_ring=1024 rx_ring=1024

strix-2 sender:
data_wr_send=36 data_wr_send_timeout=2 data_wr_timeout_last_psn=9442826
data_tx_posted=324 data_tx_completed=324 data_tx_canceled=0 data_tx_errors=0
data_rx_completed=44
data_tx_credit_received=320
data_rx_send_ack=34 data_rx_send_ack_matched=34
data_rx_send_ack_unmatched=0 data_rx_no_qp=0
data_rx_repost_failed=0 data_rx_bad_frame=0 data_rx_bad_header=0
path_tx inflight=0 data_q=0 ctrl_q=0 raw_active=0
path_cfg e2e=1 tx_ring=1024 rx_ring=1024
```

Irrefutable statement: receiver state-level TX posted and completed 46 reverse
frames. The sender RX callback processed only 44 reverse frames. The accounting
matches the semantic split: 320 receiver RX credits arrived at the sender, and
34 of 36 SEND_ACKs arrived at the sender. The two missing frames disappeared
after receiver TX completion and before sender `tbv_path_rx_complete()`.

This row also has clean sender data TX completion (`324/324`), zero sender TX
inflight, zero sender TX errors/canceled, and no RX repost/bad-frame/bad-header
evidence. It is therefore a clean receiver-TX-completed to sender-RX-callback
loss row.

Contradiction: the model "the missing ACK was dropped in receiver software
queueing or receiver `tb_ring_tx()` before TX completion" is false for this
row. The model "the missing ACK reached sender RX callback but was misdecoded
or mismatched above the ring" is also false for this row.

Important correction: the loss is not explained by native E2E being absent.
The live native paths are configured with E2E enabled. The next question is
whether E2E is effective for these native paths and hops, whether the E2E hop
configuration is actually valid for both directions, or whether another
Thunderbolt/NHI condition can still drop completed frames before peer RX
callback.

Separate state observation: after clean reload, debugfs `peers` listed two
native peers with two rails each, all active and data-ready, on the same route
and path ids. The traffic counters in this failing row advanced on only one
peer's rails. This may be benign source-aware xdomain enumeration, but it is a
routing/state invariant to verify before trusting higher-level path selection.

### RX-canceled counter and native E2E discriminator

Added two diagnostic changes:

- `data_rx_canceled` at the `tbv_path_rx_complete(canceled=true)` early return,
  exposed at both state and per-path debugfs levels.
- `native_e2e=<0|1>` load-time module parameter. Default is `1`, preserving
  the existing native Linux behavior. `0` skips `RING_FLAG_E2E` on native data
  TX/RX rings.

Local history correction: Strix Halo already had an empirically solid AMD NHI
E2E defect in the older apple_rdma work. The local notes characterize it as a
multiple-E2E-RX-ring credit-accounting collision: a second E2E RX ring on the
same NHI can wedge another ring's TX completions, and the refined model was
"at most one E2E-flagged RX ring active per NHI" before credits collide. That
history is directly relevant because the current native topology creates two
native peers with two rails each, and with `native_e2e=1` every rail has
`rx_flags=0x6 e2e=1`.

Architecture correction from the local Thunderbolt driver source:

- The NHI flag is named `RING_FLAG_E2E_FLOW_CONTROL`. RX ring options only
  program the E2E credit-return TX hop. There is no NHI retransmission or
  reliable-delivery layer.
- The upstream `QUIRK_E2E` reserved-hop workaround is applied only to Intel
  Falcon Ridge. AMD Strix Halo runs the un-quirked E2E path, so multiple E2E
  rings are not protected by that workaround.

Consequence: getting E2E "right" can prevent buffer overrun and may remove the
TX-completion wedge, but it cannot recover a genuinely dropped SEND_ACK. ACK
correctness needs software reliability: retransmit until the PSN-specific ACK
is matched, or an equivalent closed-loop control-frame ARQ.

#### `native_e2e=1` after adding `data_rx_canceled`

Fresh command shape: rebooted both hosts, reloaded patched module with default
`native_e2e=1`, then set `native_tx_max_inflight=6`,
`send_ack_redundancy=1`. 64 KiB, count 256, device `usb4_rdma0`.

Result: FAIL.

```text
sender:
send wc error wr_id=339 status=12 opcode=1 byte_len=0

receiver:
timeout waiting gpu_seen=170 got=169 signal=169

strix-1 receiver:
data_tx_posted=433 data_tx_completed=433 data_tx_canceled=0 data_tx_errors=0
data_tx_send_ack=338 data_tx_send_ack_ok=338 data_tx_send_ack_error=0
data_rx_completed=3042 data_rx_canceled=0
data_rx_credit_sent=3040

strix-2 sender:
data_wr_send=338 data_wr_send_timeout=1 data_wr_timeout_last_psn=4577728
data_tx_posted=3042 data_tx_completed=3039 data_tx_canceled=0 data_tx_errors=0
path_tx inflight=3
data_rx_completed=432 data_rx_canceled=0
data_tx_credit_received=3040
data_rx_send_ack=337 data_rx_send_ack_matched=337
data_rx_send_ack_unmatched=0
```

Irrefutable statement: the receiver completed 433 reverse TX frames and the
sender RX callback processed 432 frames. The missing reverse frame did not hide
behind the `canceled=true` RX callback branch; `data_rx_canceled=0` on both
hosts. This row also reproduced the older TX-side wedge shape on the sender:
three posted TX frames remained inflight indefinitely (`3042/3039`), with
`verbs_qps=1` until reboot. That makes this row less clean for ACK-loss
isolation, but it strongly connects the current native topology to the older
Strix E2E credit-completion hazard.

#### `native_e2e=0`

Fresh command shape: rebooted both hosts, reloaded patched module with
`native_e2e=0`, `native_tx_max_inflight=6`, `send_ack_redundancy=1`. 64 KiB,
count 256, device `usb4_rdma0`.

Live verification before traffic:

```text
native_e2e=N
send_ack_redundancy=1
native_tx_max_inflight=6
path_cfg tx_flags=0x2 rx_flags=0x2 e2e=0 wire_flags=0x1
all state counters zero
```

Result: FAIL.

```text
sender:
send wc error wr_id=179 status=12 opcode=1 byte_len=0

receiver:
timeout waiting gpu_seen=90 got=89 signal=89

strix-1 receiver:
data_tx_posted=228 data_tx_completed=228 data_tx_canceled=0 data_tx_errors=0
data_tx_send_ack=178 data_tx_send_ack_ok=178 data_tx_send_ack_error=0
data_rx_completed=1602 data_rx_canceled=0
data_rx_credit_sent=1600

strix-2 sender:
data_wr_send=178 data_wr_send_timeout=2 data_wr_timeout_last_psn=12676331
data_tx_posted=1602 data_tx_completed=1602 data_tx_canceled=0 data_tx_errors=0
path_tx inflight=0
data_rx_completed=226 data_rx_canceled=0
data_tx_credit_received=1600
data_rx_send_ack=176 data_rx_send_ack_matched=176
data_rx_send_ack_unmatched=0
```

Irrefutable statement: disabling native E2E did not eliminate reverse-frame
loss. Receiver completed 228 reverse TX frames, while sender RX callback
processed 226. The two missing frames correspond exactly to missing SEND_ACKs:
receiver sent 178 ACKs, sender received 176. `data_rx_canceled=0` again, and
this time sender TX completion was clean (`1602/1602`, inflight 0).

Conclusion from the pair: multiple native E2E rings remain a credible
explanation for the TX-completion wedge component, especially given the prior
Strix Halo history. ACK/control-frame loss also occurs with E2E disabled
globally, but the E2E-on and E2E-off loss mechanisms should not be assumed
identical. A plausible split is:

- E2E on with duplicate peers/multiple rings: AMD credit mis-accounting can
  over-advertise or stop returning credits, producing both reverse-frame loss
  and TX-completion wedge symptoms.
- E2E off: the channel has no hardware backpressure, so reverse control frames
  can still disappear under pressure.

The shared invariant across the clean rows is still receiver TX completion
exceeding sender RX callback count, with no RX canceled, bad-frame,
bad-header, or QP-mismatch evidence.

### Stopped State

After the E2E discriminator runs, both Strix hosts were reloaded back to the
branch defaults:

```text
write_zcopy=N
zcopy_min_bytes=0
native_e2e=Y
native_tx_max_inflight=8
send_ack_redundancy=3
native_fragment_striping=N
verbs_ucontexts/pds/cqs/qps/mrs/recv_wqes=0
data_wr_send=0 data_wr_timeout=0
data_tx_posted=0 data_tx_completed=0
data_rx_completed=0 data_rx_canceled=0
watchdog state=active timeout=15 nowayout=1
native path_cfg e2e=1 tx_ring=1024 rx_ring=1024
```

## Current Scientific Position

1. ACK redundancy is a workaround, not an explanation. It hides a real loss
   mode by making at least one duplicate ACK likely to survive.
2. Native TX pressure is a discriminator, not a root cause. `tx=4` passed in
   this matrix and `tx=6` failed, but this is a probability/pressure signal,
   not a proven safe threshold.
3. The copied-WR active root cause is now narrower: in a clean reproduced row,
   reverse frames disappeared between receiver state-level TX completion and
   sender `tbv_path_rx_complete()` callback.
4. The `canceled=true` RX callback path is not the explanation for the two
   freshest failures. `data_rx_canceled=0` in both the E2E-on and E2E-off rows.
5. E2E is flow-control only, not retransmission. Therefore no E2E setting can
   make one-shot SEND_ACK reliable after a real transport drop.
6. Native E2E is now split into two questions. Multiple E2E rings can plausibly
   explain the separate sender TX-completion wedge, while ACK/control-frame
   loss requires software reliability even if E2E flow control is made sane.
7. The next layer of proof must move below verbs/QP ACK handling into the path
   and Thunderbolt/NHI layer: prove whether TX completion is only local
   descriptor consumption, identify whether sender RX callback absence has an
   NHI interrupt/ring symptom, and separate control-frame reliability from
   data TX completion pressure.
8. WRITE zcopy remains a separate raw-stream lifetime bug and should stay
   disabled while the copied-control-frame loss is isolated.

## Watchdog Recovery Hardening

Recovered the removed `nix-strix-halo` watchdog module from commit
`82dbdcd275c50984896c3859772e99358aaaaa97` and reinstated the relevant policy
directly in `/mnt/Home/src/nixos-config/machines/x86/strix-halo/default.nix`.

Deployed settings:

```text
sp5100_tco heartbeat=30 nowayout=1 action=0
RuntimeWatchdogSec=15s
RebootWatchdogSec=30s
KExecWatchdogSec=30s
panic=5 panic_on_oops=1 softlockup_panic=1 hung_task_panic=1 nmi_watchdog=panic,1
```

Post-deploy verification on both `strix-1` and `strix-2`:

```text
systemd_watchdogs=15s,30s,30s
/sys/class/watchdog/watchdog0/identity=SP5100 TCO timer
/sys/class/watchdog/watchdog0/state=active
/sys/class/watchdog/watchdog0/timeout=15
/sys/class/watchdog/watchdog0/nowayout=1
kernel.panic = 5
kernel.watchdog = 1
kernel.panic_on_oops = 1
kernel.softlockup_panic = 1
kernel.hung_task_panic = 1
kernel.nmi_watchdog = 1
kernel.hardlockup_panic = 1
```

Safe post-watchdog RDMA baseline:

```text
send_result ... size=65536 count=256 status=OK elapsed_sec=0.102886 avg_us=401.900
recv_result kind=host-uncached mode=normal size=65536 count=256 status=OK elapsed_sec=0.102971 avg_us=402.230

strix-2 sender:
data_wr_send=512 data_wr_copied=512 data_wr_zcopy=0
data_wr_timeout=0
data_tx_posted=4608 data_tx_completed=4608
data_rx_ack=1504 data_rx_no_qp=32
path_tx inflight=0 zcopy_inflight=0 raw_active=0
```

Important operational note: after the NixOS reboot, `strix-2` booted the
installed `/run/booted-system/.../extra/thunderbolt_ibverbs.ko` from the
`thunderbolt-ibverbs-rewrite` input. That module lacks the GDA revive params
(`write_zcopy`, `native_tx_max_inflight`, `send_ack_redundancy`) and had
`native_fragment_striping=Y`. For the GDA tests, it was unloaded and replaced
with the rebuilt module from
`/mnt/Home/src/thunderbolt-ibverbs-gda-iommu-revive`.

## Next Falsifiers

1. Verify the native E2E setup rather than assuming it is absent. Live paths
   show `e2e=1`, and local history says Strix Halo can mis-account multiple
   E2E RX rings. Next useful discriminator is single-E2E-ring/single-native-peer
   topology, not merely E2E-on versus E2E-off.
2. Add path/NHI-level ACK tracepoints/counters. Required split: receiver
   `SEND_ACK` control-frame build, selected rail/path, path enqueue,
   Thunderbolt ring post, Thunderbolt TX completion, sender RX callback, native
   header decode, QP lookup, and QP match. Include opcode, PSN, src_qp,
   dest_qp, rail id, path id, route, ring hop, E2E hop, and peer id.
3. Verify the duplicate native peer state after reload. It may be harmless, but
   in a path/routing investigation the invariant should be explicit: exactly
   which peer/rail owns the QP and which duplicate peer, if any, can receive or
   transmit on the same route/path ids.
4. For WRITE zcopy, either keep it disabled or make timeout fatal to the raw
   stream owner: cancel remaining queued/inflight zcopy packets, complete their
   callbacks with `-ETIMEDOUT`/`-ECANCELED`, mark the path not data-ready, and
   force a ring/path reset before accepting later traffic on that rail.
5. Repeat the ACK=1 matrix only after path/NHI-level counters exist. The goal is
   not to find a lower "safe" TX limit; it is to find the exact layer where the
   ACK disappears.

## 2026-06-05 Correctness Stack Retest After Rebase

The missing HIP/GDA probes are now packaged in the flake so this test artifact
is reproducible instead of being a local binary:

```text
36d1fc3 nix: package HIP GDA probes
d83fc33 native: auto-disable E2E on AMD NHI
```

The new package is exposed as `.#tbv-hip-gda-probes`, included in the Linux
dev shell, and exported by the overlay as `tbv-hip-gda-probes`. The built output
used for this retest was:

```text
/nix/store/z9nf2cxbzh3d9c9bi4i8r8hjyampwndb-tbv-hip-gda-probes-0.1.0
```

Verification before running:

```text
nix build --no-link .#tbv-hip-gda-probes .#thunderbolt-ibverbs \
  .#checks.x86_64-linux.proto-smoke \
  .#checks.x86_64-linux.portable-kernel-patches
nix build --no-link .#bench-tools
nix flake check --no-build
git diff --check
```

Both Strix hosts were clean-reloaded with `thunderbolt-ibverbs-reload-system`,
then run with:

```text
native_e2e=-1
native_tx_max_inflight=6
```

On AMD/Strix, `native_e2e=-1` auto-disables hardware E2E for native rings. Live
paths showed `path_cfg ... e2e=0 wire_flags=0x1`.

Important caveat: this is not a literal replay of the old
`send_ack_redundancy=1` row. That parameter is gone in the rebased correctness
stack. OK SEND_ACKs are now sent on every live native path for the QP, so one
logical ACK produces two ACK frames in this two-rail run.

Five repeated runs of the old pressure row all passed:

```text
hip_rdma_write_visibility_probe \
  --kind host-uncached --mode normal --size 65536 --count 256 \
  --dev usb4_rdma0 --gid-index 1 --timeout-ms 15000
```

Each run returned `send_result ... status=OK` and
`recv_result ... status=OK`; observed elapsed times were about 0.052-0.061 s.

Cumulative post-run counters:

```text
strix-1 receiver:
data_tx_posted=5840 data_tx_completed=5840
data_tx_canceled=0 data_tx_errors=0
data_rx_completed=23040 data_rx_canceled=0 data_rx_repost_failed=0
data_rx_bad_frame=0 data_rx_bad_header=0
data_tx_ack_ok=2560 native_tx_send_ack=5120
data_tx_ack_send_error=0
data_rx_no_qp=0 data_rx_bad_peer=0 data_rx_unconnected_qp=0 data_rx_qp_error=0
path_tx inflight=0 on all paths

strix-2 sender:
data_wr_send=2560 data_wr_live=0 data_wr_timeout=0 data_wr_retransmit=0
data_tx_posted=23040 data_tx_completed=23040
data_tx_canceled=0 data_tx_errors=0
data_rx_completed=5840 data_rx_canceled=0 data_rx_repost_failed=0
data_rx_ack=5120 data_rx_ack_matched=2560
data_rx_late_ack=2560 data_rx_ack_miss=0
native_rx_send_ack=5120
data_rx_no_qp=0 data_rx_bad_peer=0 data_rx_unconnected_qp=0 data_rx_qp_error=0
path_tx inflight=0 on all paths
```

Interpretation:

1. The correctness stack no longer reproduces the old copied-WR ACK timeout in
   this pressure row across five runs.
2. The ACK path now has the expected duplicate shape: every logical ACK is
   matched once and observed once more as a late duplicate.
3. `data_rx_ack_miss=0` is not a lost-ACK metric; it only says no arriving ACKs
   were unexplained orphans. Lost ACKs are visible through retransmit/timeout
   behavior, not this counter.
4. The old late-duplicate teardown wart did not appear here: `data_rx_no_qp=0`
   after userspace exit on both hosts.
5. This does not prove the transport is reliable. It proves that the current
   software ACK replication plus AMD E2E auto-disable survives this formerly
   failing GDA visibility row.

## 2026-06-05 ACK-Loss Fault Injection

Added a disabled-by-default kernel fault injector:

```text
native_ack_drop_every=0
```

When set to `N`, the receiver silently drops every Nth logical native OK
SEND_ACK before the all-rail fan-out. This deliberately suppresses all rail
copies for that ACK and returns success to the caller, modeling a transport
loss rather than a local send error. Debugfs counters:

```text
data_tx_ack_drop_checked
data_tx_ack_drop_injected
```

The first forced-loss attempts exposed a test-harness bug rather than a clean
protocol result. With the original probe, the receiver exited as soon as the GPU
observed all signals, even if the sender still had WRs pending because their
ACKs had been dropped. That destroyed the receiver QP while the sender was
still retransmitting, producing `data_rx_no_qp` on the receiver and hard resets
on `strix-2` with no preserved previous-boot journal.

Bad/stressful attempts:

```text
count=64 native_ack_drop_every=16 qp_timeout_ms=200
receiver status=OK elapsed=0.721s
receiver data_tx_ack_drop_checked=225 data_tx_ack_drop_injected=14
receiver data_rx_duplicate_ack=97 data_rx_no_qp=119 data_rx_canceled=4096
strix-2 hard reset around 04:19

count=16 native_ack_drop_every=16 qp_timeout_ms=5000
receiver status=OK elapsed=0.075s
receiver data_tx_ack_drop_checked=33 data_tx_ack_drop_injected=2
receiver data_rx_duplicate_ack=1 data_rx_no_qp=119
strix-2 hard reset around 04:22
```

The HIP visibility probe was then fixed to include a final sender-completion
fence: after sending all per-sequence GPU-visible ACKs, the receiver keeps its
QP alive until the sender reports that all send completions have been observed
or until the probe timeout expires.

With that fenced probe, the single-drop validation passed:

```text
receiver:
native_ack_drop_every=32 qp_timeout_ms=5000
hip_rdma_write_visibility_probe --kind host-uncached --mode normal \
  --size 65536 --count 16 --timeout-ms 30000

send_result ... count=16 status=OK elapsed_sec=0.141131
recv_result ... count=16 status=OK elapsed_sec=0.141199
```

Post-run counters:

```text
strix-1 receiver:
data_tx_ack_ok=33
data_tx_ack_drop_checked=33
data_tx_ack_drop_injected=1
native_tx_send_ack=64
data_rx_duplicate_ack=1
data_rx_ack_history_miss=0
data_rx_no_qp=0
data_tx_posted=73 data_tx_completed=73
path_tx inflight=0 on all paths

strix-2 sender:
data_wr_send=32
data_wr_retransmit=1
data_wr_retry_enqueue_error=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_tx_posted=289 data_tx_completed=289
data_rx_ack=64
data_rx_ack_matched=32
data_rx_ack_match_retried=1
data_rx_ack_match_max_ms=138
data_rx_ack_miss=0
data_rx_late_ack=32
data_rx_no_qp=0
path_tx inflight=0 on all paths
```

Interpretation:

1. The ACK-loss injector now gives a repeatable way to force the reliability
   backstop.
2. The receiver duplicate-PSN re-ack path worked for the injected lost ACK:
   the WR completed after one retransmit and did not double-deliver to
   userspace-visible state.
3. The probe must keep the receiver QP alive until the sender has observed all
   send completions; otherwise ACK-loss testing can turn into teardown-induced
   `data_rx_no_qp` and sender reset instead of a protocol validation.
4. The aggressive `qp_timeout_ms=200` run is not a correctness baseline. It is
   a separate stress/failure case showing that retransmit storms and teardown
   races can still be dangerous.

## 2026-06-05 NixOS Retest After Strix LAN/Deployment Fixes

Host deployment changes before retest:

```text
nixos-config:
- strix-1/strix-2 LAN now uses eno1 directly; br0.lan removed.
- thunderbolt-ibverbs roce_netdev=eno1.
- netconsole sender enabled on eno1 for both Strix hosts.
- crashDump enabled with crashkernel=512M.
- thunderbolt-ibverbs-kernel lock overridden to the local
  thunderbolt-ibverbs-gda-iommu-revive branch.
- tbv-hip-gda-probes included in the Strix system profile.
```

Verification:

```text
strix-1 eno1: 192.168.23.136/24, netconsole-sender active
strix-2 eno1: 192.168.23.192/24, netconsole-sender active
hip_rdma_write_visibility_probe present on both hosts
module params present: native_e2e, native_tx_max_inflight,
  native_ack_drop_every, qp_timeout_ms
```

Netconsole was tested with emergency `/dev/kmsg` markers from both hosts; both
arrived on the local UDP listener. During the GDA retests below, the only
netconsole lines were those two deliberate test markers.

All tests used:

```text
native_e2e=-1
native_tx_max_inflight=6
roce_netdev=eno1
receiver=strix-1
sender=strix-2
dev=usb4_rdma0 gid-index=1 kind=host-uncached mode=normal size=65536
```

Fresh clean pressure-row baseline:

```text
count=256 timeout_ms=15000 native_ack_drop_every=0 qp_timeout_ms=5000
send_result status=OK elapsed_sec=0.053407
recv_result status=OK elapsed_sec=0.053490
```

Counters:

```text
strix-1 receiver:
data_tx_ack_ok=512 native_tx_send_ack=1024
data_tx_posted=1168 data_tx_completed=1168
data_rx_completed=4608 data_rx_canceled=0 data_rx_repost_failed=0
data_rx_bad_frame=0 data_rx_bad_header=0
data_rx_no_qp=0 data_rx_bad_peer=0 data_rx_unconnected_qp=0
path_tx inflight=0 on all paths

strix-2 sender:
data_wr_send=512 data_wr_retransmit=0 data_wr_timeout=0
data_tx_posted=4608 data_tx_completed=4608
data_rx_ack=1024 data_rx_ack_matched=512 data_rx_late_ack=512
data_rx_ack_match_max_ms=3 data_rx_ack_miss=0
data_rx_canceled=0 data_rx_repost_failed=0 data_rx_no_qp=0
path_tx inflight=0 on all paths
```

Single forced ACK-loss validation on the redeployed system:

```text
count=16 timeout_ms=30000 native_ack_drop_every=32 qp_timeout_ms=5000
send_result status=OK elapsed_sec=0.075117
recv_result status=OK elapsed_sec=0.075238
```

Counters:

```text
strix-1 receiver:
data_tx_ack_ok=33
data_tx_ack_drop_checked=33 data_tx_ack_drop_injected=1
native_tx_send_ack=64
data_rx_duplicate_ack=1 data_rx_ack_history_miss=0
data_rx_no_qp=0 data_rx_canceled=0 data_rx_repost_failed=0
data_tx_posted=73 data_tx_completed=73
path_tx inflight=0 on all paths

strix-2 sender:
data_wr_send=32 data_wr_retransmit=1 data_wr_timeout=0
data_wr_retry_enqueue_error=0 data_wr_retry_exhausted=0
data_tx_posted=289 data_tx_completed=289
data_rx_ack=64 data_rx_ack_matched=32 data_rx_late_ack=32
data_rx_ack_match_retried=1 data_rx_ack_match_max_ms=71
data_rx_ack_miss=0 data_rx_no_qp=0 data_rx_canceled=0
path_tx inflight=0 on all paths
```

Moderate retransmit stress:

```text
count=64 timeout_ms=30000 native_ack_drop_every=8 qp_timeout_ms=5000
send_result status=OK elapsed_sec=1.299223
recv_result status=OK elapsed_sec=1.299278
```

Counters:

```text
strix-1 receiver:
data_tx_ack_ok=290
data_tx_ack_drop_checked=290 data_tx_ack_drop_injected=36
native_tx_send_ack=508
data_rx_duplicate_ack=162 data_rx_ack_history_miss=0
data_rx_no_qp=0 data_rx_canceled=0 data_rx_repost_failed=0
data_tx_posted=549 data_tx_completed=549
path_tx inflight=0 on all paths

strix-2 sender:
data_wr_send=128 data_wr_retransmit=18 data_wr_timeout=0
data_wr_retry_enqueue_error=0 data_wr_retry_exhausted=0
data_tx_posted=1314 data_tx_completed=1314
data_rx_ack=508 data_rx_ack_matched=137 data_rx_late_ack=371
data_rx_ack_match_retried=18 data_rx_ack_match_max_ms=74
data_rx_ack_miss=0 data_rx_no_qp=0 data_rx_canceled=0
path_tx inflight=0 on all paths
```

Old hard-reset parameter row replayed with the fenced probe:

```text
count=64 timeout_ms=30000 native_ack_drop_every=16 qp_timeout_ms=200
send_result status=OK elapsed_sec=0.719094
recv_result status=OK elapsed_sec=0.719143
```

Counters:

```text
strix-1 receiver:
data_tx_ack_ok=199
data_tx_ack_drop_checked=199 data_tx_ack_drop_injected=12
native_tx_send_ack=374
data_rx_duplicate_ack=71 data_rx_ack_history_miss=0
data_rx_no_qp=0 data_rx_canceled=0 data_rx_repost_failed=0
data_tx_posted=412 data_tx_completed=412
path_tx inflight=0 on all paths

strix-2 sender:
data_wr_send=128 data_wr_retransmit=10 data_wr_timeout=0
data_wr_retry_enqueue_error=0 data_wr_retry_exhausted=0
data_tx_posted=1223 data_tx_completed=1223
data_rx_ack=374 data_rx_ack_matched=133 data_rx_late_ack=241
data_rx_ack_match_retried=10 data_rx_ack_match_max_ms=72
data_rx_ack_miss=0 data_rx_no_qp=0 data_rx_canceled=0
path_tx inflight=0 on all paths
```

Interpretation:

1. The redeployed GDA correctness branch reproduces the previous green baseline:
   AMD native E2E auto-disables to `path_cfg e2e=0`, ACK fan-out is 2x, and the
   former pressure row passes with no retransmits or teardown artifacts.
2. The forced-loss backstop now survives more than the single-drop toy case.
   `drop_every=8` drove 36 injected logical ACK drops and 18 sender
   retransmits, all completing without timeout, retry exhaustion, RX cancel,
   or QP teardown fallout.
3. Replaying the old hard-reset parameters with the fenced probe passed, so
   `qp_timeout_ms=200` retransmit pressure alone was not sufficient to crash
   this build.
4. This does not resolve the unfenced crash. The fenced probe is a test control:
   it avoids the legal peer-teardown-during-retransmit condition. The kernel
   still needs an unfenced capture run before claiming that arbitrary peer/QP
   teardown is crash-proof.

## 2026-06-05 Reusable Crash Capture + Unfenced Retest

NixOS capture setup added/redeployed:

```text
trex:
- netconsole collector switched on and persistent:
  /var/log/netconsole/strix.log

strix-1/strix-2:
- netconsole sender remains on eno1.
- sconfig.ramoops enabled with reserved RAM:
  memmap=2M$0x205d000000
  ramoops.mem_address=0x205d000000
  ramoops.mem_size=0x200000
  ramoops.record_size=0x40000
- efi_pstore disabled so ramoops can own the singleton pstore backend:
  efi_pstore.pstore_disable=1
  pstore.backend=ramoops
```

Post-reboot verification:

```text
strix-1:
pstore.backend=ramoops
efi_pstore.pstore_disable=Y
pstore: Registered ramoops as persistent store backend
ramoops: using 0x200000@0x205d000000, ecc: 16

strix-2:
pstore.backend=ramoops
efi_pstore.pstore_disable=Y
pstore: Registered ramoops as persistent store backend
ramoops: using 0x200000@0x205d000000, ecc: 16
```

The HIP visibility probe now has an explicit unsafe mode:

```text
--unsafe-no-final-fence
```

This skips the sender-to-receiver final completion fence and makes the teardown
condition visible in logs as `final_fence=0`.

Unfenced replay of the old hard-reset row:

```text
count=64 timeout_ms=30000 native_ack_drop_every=16 qp_timeout_ms=200
final_fence=0
send_result status=OK elapsed_sec=0.579185
recv_result status=OK elapsed_sec=0.579171
```

Counters:

```text
strix-1 receiver:
data_tx_ack_ok=200
data_tx_ack_drop_checked=200 data_tx_ack_drop_injected=12
data_rx_duplicate_ack=72
data_tx_posted=414 data_tx_completed=414 data_tx_errors=0
data_rx_completed=1224 data_rx_canceled=0

strix-2 sender:
data_wr_send=128 data_wr_retransmit=8 data_wr_timeout=0
data_wr_retry_enqueue_error=0 data_wr_retry_exhausted=0
data_tx_posted=1224 data_tx_completed=1224 data_tx_errors=0
data_rx_completed=414 data_rx_canceled=0
data_rx_ack=376 data_rx_ack_matched=132
data_rx_ack_match_retried=8 data_rx_late_ack=244
data_rx_ack_miss=0
```

Higher-loss unfenced stress:

```text
count=256 timeout_ms=60000 native_ack_drop_every=4 qp_timeout_ms=200
final_fence=0
send_result status=OK elapsed_sec=10.867799
recv_result status=OK elapsed_sec=10.867822
```

Counters:

```text
strix-1 receiver:
data_tx_ack_ok=1875
data_tx_ack_drop_checked=1875 data_tx_ack_drop_injected=468
data_rx_duplicate_ack=1363
data_tx_posted=3000 data_tx_completed=3000 data_tx_errors=0
data_rx_completed=5971 data_rx_canceled=0

strix-2 sender:
data_wr_send=512 data_wr_retransmit=150 data_wr_timeout=0
data_wr_retry_enqueue_error=0 data_wr_retry_exhausted=0
data_tx_posted=5971 data_tx_completed=5971 data_tx_errors=0
data_rx_completed=3000 data_rx_canceled=4096
data_rx_ack=2814 data_rx_ack_matched=594
data_rx_ack_match_retried=150 data_rx_late_ack=2220
data_rx_ack_miss=0
```

Netconsole and pstore:

```text
trex netconsole log contained only deliberate start markers; no crash/oops
records were emitted.
/sys/fs/pstore and /var/lib/pstore contained no crash records after the runs.
```

Follow-up reload check:

```text
Immediately after a clean post-test module reload:
strix-1 data_rx_canceled=0
strix-2 data_rx_canceled=0
```

Interpretation:

1. The unfenced old-row replay did not reproduce the previous hard reset on the
   current rebased/patched build. It did exercise the intended path:
   8 sender retransmits, 72 receiver duplicate re-acks, no timeout, no retry
   exhaustion, no crash.
2. A much stronger unfenced row also completed: 468 injected ACK drops,
   150 sender retransmits, 1363 receiver duplicate re-acks, no timeout, no
   retry exhaustion, no crash.
3. This is a non-reproduction, not a proof of absence. The old host reset
   remains historically real but is now uncaptured on this build and may have
   depended on code before the current patch/rebase state or on a narrower
   lifecycle race window.
4. The high-loss run exposed a new anomaly: `strix-2 data_rx_canceled=4096`.
   Because a clean reload immediately afterward produced `data_rx_canceled=0`,
   this is not a deterministic reload counter artifact. Treat it as a live
   lifecycle/ring-cancel observation to investigate, not as a settled cause.

## 2026-06-05 PR24 Kernel Patch Recipe + Retransmit/Teardown Guard Retest

Before this retest, one important negative result was reclassified:

```text
git log ba44378..HEAD -- kernel/
```

was empty at the time of the prior unfenced non-repro. The kernel
teardown/retry/peer code was therefore byte-identical to the crash-era
injector commit. That non-repro was not a fix; it dodged the timing window.

Deployment changes:

```text
nixos-config:
- restored the referenced thunderbolt-xdomain-bridge-hardening.patch in the
  Thunderbolt kernel recipe, fixed the malformed hunk metadata, staged it so
  flakes cannot silently drop it, and rebuilt the Strix kernel.
- thunderbolt-ibverbs-kernel input was locked to the local
  thunderbolt-ibverbs-gda-iommu-revive worktree.
- deployed with colmena switch, then colmena apply boot --reboot.

strix-1 booted:
/nix/store/ww15x29ycl0cmb9rnq1cmydsab0jsvqq-nixos-system-strix-1-26.11pre-git

strix-2 booted:
/nix/store/fzv7pkfvlfz1gv7q9cq758zgavc2svph-nixos-system-strix-2-26.11pre-git
```

Instrumentation added to the GDA module:

```text
data_wr_retransmit_closing_qp
data_wr_retransmit_no_live_path
data_wr_retransmit_teardown_path
```

The guard logs retransmit attempts that see a local closing QP, no live native
path, or a selected native path whose rail/path/rings are visibly tearing down.
It uses `pr_warn_ratelimited()` rather than `WARN_ONCE()` so the watchdog/panic
configuration does not turn instrumentation into a new crash trigger.

### Instrumented Unfenced High-Loss Row

```text
count=256 timeout_ms=60000 native_ack_drop_every=4 qp_timeout_ms=200
final_fence=0
receiver: status=OK elapsed_sec=11.593501
sender: wc error status=12 opcode=1 wr_id=513
```

Sender (`strix-2`) counters:

```text
data_wr_send=512
data_wr_retransmit=168
data_wr_retry_enqueue_error=0
data_wr_retry_exhausted=1
data_wr_timeout=1
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_tx_posted=6251 data_tx_completed=6251 data_tx_errors=0
data_rx_completed=3417 data_rx_canceled=0
data_rx_ack=3222 data_rx_ack_matched=616
data_rx_ack_match_retried=161 data_rx_late_ack=2606
data_rx_ack_miss=0
```

Receiver (`strix-1`) counters:

```text
data_tx_ack_ok=2148
data_tx_ack_drop_checked=2148 data_tx_ack_drop_injected=537
data_rx_duplicate_ack=1636
data_tx_posted=3417 data_tx_completed=3417 data_tx_errors=0
data_rx_completed=6251 data_rx_canceled=0
data_rx_no_qp=7
```

### Fenced Control With Same Loss Pressure

```text
count=256 timeout_ms=60000 native_ack_drop_every=4 qp_timeout_ms=200
final_fence=1
sender: status=OK elapsed_sec=12.022799
receiver: status=OK elapsed_sec=12.022945
```

Sender (`strix-2`) counters:

```text
data_wr_send=512
data_wr_retransmit=163
data_wr_retry_enqueue_error=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_tx_posted=6193 data_tx_completed=6193 data_tx_errors=0
data_rx_completed=3339 data_rx_canceled=0
data_rx_ack=3146 data_rx_ack_matched=608
data_rx_ack_match_retried=163 data_rx_late_ack=2538
data_rx_ack_miss=0
```

Receiver (`strix-1`) counters:

```text
data_tx_ack_ok=2097
data_tx_ack_drop_checked=2097 data_tx_ack_drop_injected=524
data_rx_duplicate_ack=1585
data_tx_posted=3339 data_tx_completed=3339 data_tx_errors=0
data_rx_completed=6193 data_rx_canceled=0
data_rx_no_qp=0
```

Netconsole and pstore remained quiet during both rows:

```text
trex netconsole tail contained only the earlier deliberate markers.
/sys/fs/pstore contained no crash records on either Strix host.
```

Interpretation:

1. The fenced control proves the live-peer retransmit backstop still works after
   the PR24 kernel patch reboot and guard instrumentation: 163 retransmits,
   524 injected ACK drops, no timeout, no retry exhaustion, no RX cancels, and
   no local retransmit/path teardown guard hits.
2. The unfenced failure is now a clean remote-QP-teardown failure, not a local
   sender path-teardown failure. The receiver finished and destroyed the QP,
   then received 7 retransmitted data frames with no QP (`data_rx_no_qp=7`).
   The sender never saw a local closing-QP/no-path/teardown-path condition, so
   it retried until one WR exhausted and surfaced `wc status=12`.
3. This is a useful contradiction to the earlier `data_rx_canceled=4096`
   near-miss: the deterministic artifact in this run is `data_rx_no_qp`, while
   `data_rx_canceled=0` on both hosts. The old hard reset is still not
   backtraced, but the most reproducible unfenced failure mode is now remote QP
   tombstone/lost-ACK teardown, not sender-side path/ring teardown.
4. A principled kernel hardening target is now visible: keep enough destroyed-QP
   tombstone/ACK history to re-ACK already-consumed duplicate PSNs briefly after
   QP teardown, or send an explicit native error ACK for no-QP retransmits so
   the sender completes with a bounded remote error instead of burning the full
   retry budget. Either way, the kernel must continue to treat arbitrary peer
   teardown as a clean completion/error path, never a host reset.

## 2026-06-05 Destroyed-QP Tombstone Retest

Change under test:

```text
When a native QP is destroyed, publish a short-lived tombstone keyed by
(local_qpn, remote_qpn) containing the QP ACK history. If later duplicate
SEND/WRITE frames arrive for the destroyed QP, re-ACK matched PSNs from the
tombstone. If no history entry exists, send an explicit SEND_ACK_ERROR instead
of silently dropping the frame.

New counters:
data_rx_no_qp_reack
data_rx_no_qp_error_ack
```

Deployment:

```text
nixos-config flake lock pointed thunderbolt-ibverbs-kernel at the local
gda-iommu-revive worktree, then both Strix hosts were deployed and rebooted
with colmena apply boot --reboot.

strix-1 booted:
/nix/store/lch1h99xwxlqdc1x22177vrv82gn4a9f-nixos-system-strix-1-26.11pre-git

strix-2 booted:
/nix/store/kbs0alh39160ds6fd9jddy2v74vdpsf0-nixos-system-strix-2-26.11pre-git
```

Netconsole remained live after reboot; emergency markers from both hosts were
received by the persistent collector on `trex`.

### Unfenced High-Loss Row, drop_every=4

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=200
receiver native_ack_drop_every=4
final_fence=0
sender:   status=OK elapsed_sec=10.871897
receiver: status=OK elapsed_sec=10.799840
```

Sender (`strix-2`) counters:

```text
data_wr_send=512
data_wr_retransmit=151
data_wr_retry_enqueue_error=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_tx_posted=6005 data_tx_completed=6005 data_tx_errors=0
data_rx_completed=3050 data_rx_canceled=0
data_rx_ack=2863 data_rx_ack_matched=595
data_rx_ack_match_retried=151 data_rx_late_ack=2268
data_rx_ack_miss=0
```

Receiver (`strix-1`) counters:

```text
data_tx_ack_ok=1909
data_tx_ack_drop_checked=1908 data_tx_ack_drop_injected=477
data_rx_duplicate_ack=1396
data_rx_ack_history_miss=0
data_rx_no_qp=1
data_rx_no_qp_reack=1
data_rx_no_qp_error_ack=0
data_tx_posted=3050 data_tx_completed=3050 data_tx_errors=0
data_rx_completed=6005 data_rx_canceled=0
```

Receiver dmesg recorded the intended mechanism:

```text
native RX no-QP re-acked dest_qp=0x900 src_qp=0x900 psn=15781175 opcode=3 ret=0 tombstone=1
```

### Stronger Unfenced Row, drop_every=2

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=200
receiver native_ack_drop_every=2
final_fence=0
sender:   status=OK elapsed_sec=18.427499
receiver: status=OK elapsed_sec=18.355733
```

Sender (`strix-2`) counters:

```text
data_wr_send=512
data_wr_retransmit=256
data_wr_retry_enqueue_error=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_tx_posted=6928 data_tx_completed=6928 data_tx_errors=0
data_rx_completed=3049 data_rx_canceled=0
data_rx_ack=2833 data_rx_ack_matched=657
data_rx_ack_match_retried=256 data_rx_late_ack=2176
data_rx_ack_miss=0
```

Receiver (`strix-1`) counters:

```text
data_tx_ack_ok=2832
data_tx_ack_drop_checked=2815 data_tx_ack_drop_injected=1407
data_rx_duplicate_ack=2303
data_rx_ack_history_miss=0
data_rx_no_qp=17
data_rx_no_qp_reack=17
data_rx_no_qp_error_ack=0
data_tx_posted=3049 data_tx_completed=3049 data_tx_errors=0
data_rx_completed=6928 data_rx_canceled=0
```

Netconsole and pstore remained quiet during both rows except for deliberate
test markers; `/sys/fs/pstore` was empty on both Strix hosts.

Interpretation:

1. The tombstone path is exercised, not inferred. The receiver accepted
   duplicate retransmits after userspace destroyed the QP, found matching
   destroyed-QP ACK history, and re-acked them.
2. This directly fixes the reproducible unfenced failure from the previous
   section: the old row had `data_rx_no_qp=7` and the sender exhausted one WR;
   the new rows have `data_rx_no_qp_reack == data_rx_no_qp` and no sender
   timeout/retry exhaustion.
3. The stronger row drove 256 retransmits and 17 post-destroy no-QP arrivals
   with no local sender teardown guard hits and no RX cancels. That separates
   this fixed no-QP lifecycle bug from the older uncaptured hard reset/ring
   cancel race, which remains a separate latent issue until a stack or a direct
   teardown guard trigger proves otherwise.

### Repeated Unfenced Stress After Commit

After committing the tombstone fix, ran three fresh module-reload trials with
the same aggressive teardown/loss shape:

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=200
receiver native_ack_drop_every=2
final_fence=0
ports=18531,18532,18533
```

All three trials passed:

```text
trial 1: sender OK elapsed_sec=18.433222 receiver OK elapsed_sec=18.361304
trial 2: sender OK elapsed_sec=18.430722 receiver OK elapsed_sec=18.359194
trial 3: sender OK elapsed_sec=18.432509 receiver OK elapsed_sec=18.360856
```

Sender (`strix-2`) invariants in each trial:

```text
data_wr_retransmit=256
data_wr_retry_exhausted=0
data_wr_timeout=0
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_rx_canceled=0
data_rx_ack_match_retried=256
data_rx_ack_miss=0
```

Receiver (`strix-1`) post-destroy behavior:

```text
trial 1: data_tx_ack_drop_injected=1583 data_rx_duplicate_ack=2655 data_rx_no_qp=16 data_rx_no_qp_reack=16
trial 2: data_tx_ack_drop_injected=1552 data_rx_duplicate_ack=2593 data_rx_no_qp=16 data_rx_no_qp_reack=16
trial 3: data_tx_ack_drop_injected=1544 data_rx_duplicate_ack=2577 data_rx_no_qp=17 data_rx_no_qp_reack=17

all trials:
data_rx_no_qp_error_ack=0
data_rx_ack_history_miss=0
data_rx_canceled=0
```

Pstore remained empty on both Strix hosts. Netconsole recorded only deliberate
test markers, with no panic/oops/watchdog output.

Interpretation:

1. The no-QP teardown fix is repeatable across reloads and not a one-off pass.
   The strict invariant held: every post-destroy no-QP arrival was re-acked from
   tombstone history, and the sender never exhausted or timed out.
2. The older `data_rx_canceled=4096` / hard-reset concern did not reproduce in
   this three-trial series. This is evidence that the reproducible unfenced
   artifact was the remote no-QP ACK-history gap, but it is not proof that the
   older ring-cancel race is impossible. It remains tracked as latent unless a
   direct ring teardown guard fires or a captured crash stack says otherwise.

### Longer Unfenced Stress Row

Ran one longer row with the same loss shape:

```text
count=1024 timeout_ms=120000 native_tx_max_inflight=6 qp_timeout_ms=200
receiver native_ack_drop_every=2
final_fence=0
port=18540
sender:   status=OK elapsed_sec=73.729803
receiver: status=OK elapsed_sec=73.657998
```

Sender (`strix-2`) counters:

```text
data_wr_send=2048
data_wr_retransmit=1024
data_wr_retry_enqueue_error=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_tx_posted=28716 data_tx_completed=28716 data_tx_errors=0
data_rx_completed=13230 data_rx_canceled=0
data_rx_ack=12333 data_rx_ack_matched=2741
data_rx_ack_match_retried=1024 data_rx_late_ack=9592
data_rx_ack_miss=0
```

Receiver (`strix-1`) counters:

```text
data_tx_ack_ok=12332
data_tx_ack_drop_checked=12315 data_tx_ack_drop_injected=6157
data_rx_duplicate_ack=10267
data_rx_ack_history_miss=0
data_rx_no_qp=17
data_rx_no_qp_reack=17
data_rx_no_qp_error_ack=0
data_tx_posted=13230 data_tx_completed=13230 data_tx_errors=0
data_rx_completed=28716 data_rx_canceled=0
```

Pstore was empty on both hosts. Netconsole recorded only deliberate markers.

Interpretation:

1. The tombstone fix scales through at least 1024 retransmits in this workload.
   The no-QP post-destroy invariant still holds exactly.
2. The RX-cancel/hard-reset concern still did not reappear under longer
   high-loss unfenced pressure. Current evidence points to the no-QP ACK-history
   gap as the reproducible correctness bug fixed here; the older hard reset is
   now lower-frequency or a separate trigger, not reproduced by this row.

### Post-Stress Normal Sanity

Reloaded both modules, restored no-injection defaults, and ran the ordinary
fenced row:

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=30000
receiver native_ack_drop_every=0
final_fence=1
port=18541
sender:   status=OK elapsed_sec=0.055204
receiver: status=OK elapsed_sec=0.055296
```

Counters after the row:

```text
sender data_wr_send=512 data_wr_retransmit=0 data_wr_retry_exhausted=0 data_wr_timeout=0
sender data_rx_ack_match_retried=0 data_rx_ack_miss=0 data_rx_canceled=0

receiver data_tx_ack_drop_injected=0 data_rx_duplicate_ack=0
receiver data_rx_no_qp=0 data_rx_no_qp_reack=0 data_rx_no_qp_error_ack=0
receiver data_rx_canceled=0
```

Interpretation: after the hostile unfenced rows, the clean fast path still
behaves normally and the live hosts were left with ACK injection disabled.

### Follow-Up: Sender Guard Counters and Tombstone Bounds

Code hardening after reviewing the first tombstone series:

```text
- tombstone expiry is now bounded by the destroyed QP's actual retry window:
  max(30s, tx_timeout * (max_retries + 2)).
- the 128-entry tombstone cap now increments data_qp_tombstone_evicted and logs
  the evicted QPN pair when the cap forces early eviction.
```

Important timeout nuance:

```text
qp_timeout_ms=200 was set in the stress rows, but this RC probe sets verbs
attr.timeout=14. Therefore the operative send ACK timeout is the verbs value,
about 67ms, not the fallback module parameter. The dmesg retransmit ages
around 70-73ms confirm this.
```

Retry/tombstone bound:

```text
TBV_SEND_MAX_RETRIES=7
tbv_rel_retry_interval(timeout, retries) == timeout

For the RC probe:
  verbs timeout 14 => ~67ms
  tombstone lifetime=max(30s, 9 * 67ms) = 30s

For a QP using the default fallback:
  qp_timeout_ms=5000
  tombstone lifetime=max(30s, 9 * 5000ms) = 45s

For the explicit fallback setting used in stress if a QP did not set verbs
timeout:
  qp_timeout_ms=200
  tombstone lifetime=max(30s, 9 * 200ms) = 30s
```

Explicit post-bound trigger row:

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=200
receiver native_ack_drop_every=2
final_fence=0
port=18550
sender:   status=OK elapsed_sec=18.432606
receiver: status=OK elapsed_sec=18.358310
booted strix-1=/nix/store/3h21xhxiyj85kvxgzna16y9lv1fi9zgf-nixos-system-strix-1-26.11pre-git
booted strix-2=/nix/store/jq5vzd96alzf4wx8pcp2mw57s7y2r5qy-nixos-system-strix-2-26.11pre-git
```

Sender (`strix-2`) counters:

```text
data_wr_send=512
data_wr_retransmit=256
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_wr_retry_enqueue_error=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_tx_posted=7010 data_tx_completed=7010 data_tx_errors=0
data_rx_completed=3134 data_rx_canceled=0
data_rx_ack=2903 data_rx_ack_matched=669
data_rx_ack_match_retried=256 data_rx_late_ack=2234
data_rx_ack_miss=0
data_qp_tombstone_evicted=0
```

Receiver (`strix-1`) counters:

```text
data_tx_ack_ok=2914
data_tx_ack_drop_checked=2897 data_tx_ack_drop_injected=1448
data_rx_duplicate_ack=2385
data_rx_ack_history_miss=0
data_rx_no_qp=17
data_rx_no_qp_reack=17
data_rx_no_qp_error_ack=0
data_qp_tombstone_evicted=0
data_tx_posted=3134 data_tx_completed=3134 data_tx_errors=0
data_rx_completed=7010 data_rx_canceled=0
```

Pstore was empty on both hosts. Netconsole recorded only deliberate markers
plus unrelated USB messages; no panic/oops/watchdog output.

Interpretation:

1. The sender-side teardown guards did **not** fire in this row, nor in the
   earlier `count=1024` row. Therefore the sender guard is instrumented
   coverage for the plausible crash path, but it is not yet demonstrated as the
   crash-fixing path. The precise statement is: sender teardown guard
   counters remained zero while the workload passed.
2. The receiver tombstone path is the directly exercised fix: all 17 no-QP
   post-destroy arrivals were re-acked from tombstone history, with no history
   misses, no error ACKs, and no evictions.
3. The old `data_rx_canceled=4096` signature still has not reappeared after the
   tombstone fix and dynamic lifetime/cap instrumentation, but without a fired
   sender guard or crash stack it remains a latent lower-frequency concern
   rather than a proven fixed sender-side race.

### Tombstone-Off A/B

Added `native_qp_tombstone_reack` as an A/B-only debug knob. Default is enabled.
Disabling it suppresses both tombstone publication and no-QP tombstone re-ack,
recreating the old behavior where late retransmits to a destroyed QP are dropped.

Both Strix hosts booted the new closure:

```text
strix-1=/nix/store/ssm5q3x6nbkl4yjnvns9kvx63rxyys74-nixos-system-strix-1-26.11pre-git
strix-2=/nix/store/dx1ckv6hs3a5fmsj6hj6lr1g5bxkdl6j-nixos-system-strix-2-26.11pre-git
kernel=7.0.10
```

Capture rig state:

```text
trex listener: nc -u -k -l 6666 -> /var/log/netconsole/strix.log
netconsole markers from both Strix hosts reached the log
pstore empty before and after both A/B rows
```

Tombstone re-ack disabled:

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=200
receiver native_ack_drop_every=2
native_qp_tombstone_reack=0 on both hosts
final_fence=0
port=18560
sender:   failed, wc error wr_id=513 status=12
receiver: status=OK elapsed_sec=18.362758
```

Sender (`strix-2`) counters:

```text
data_wr_send=512
data_wr_retransmit=262
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_wr_retry_exhausted=1
data_wr_timeout=1
data_rx_ack_matched=660
data_rx_ack_match_retried=255
data_rx_ack_match_over_64ms=255
data_rx_late_ack=2230
data_rx_canceled=0
```

Receiver (`strix-1`) counters:

```text
data_tx_ack_ok=2890
data_tx_ack_drop_checked=2890 data_tx_ack_drop_injected=1445
data_rx_duplicate_ack=2378
data_rx_no_qp=7
data_rx_no_qp_reack=0
data_rx_no_qp_error_ack=0
data_rx_canceled=0
```

Tombstone re-ack enabled, same trigger:

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=200
receiver native_ack_drop_every=2
native_qp_tombstone_reack=1 on both hosts
final_fence=0
port=18561
sender:   status=OK elapsed_sec=18.433667
receiver: status=OK elapsed_sec=18.361957
```

Sender (`strix-2`) counters:

```text
data_wr_send=512
data_wr_retransmit=256
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_rx_ack_matched=692
data_rx_ack_match_retried=256
data_rx_ack_match_over_64ms=256
data_rx_late_ack=2519
data_rx_canceled=0
```

Receiver (`strix-1`) counters:

```text
data_tx_ack_ok=3210
data_tx_ack_drop_checked=3193 data_tx_ack_drop_injected=1596
data_rx_duplicate_ack=2681
data_rx_no_qp=17
data_rx_no_qp_reack=17
data_rx_no_qp_error_ack=0
data_qp_tombstone_evicted=0
data_rx_canceled=0
```

Interpretation:

1. This is the first direct cause/effect A/B for the tombstone mechanism.
   With tombstone re-ack disabled, the old no-QP drop behavior returns and one
   sender WR exhausts retries. With tombstone re-ack enabled, the same
   unfenced trigger passes and every no-QP post-destroy arrival is re-acked
   from tombstone history. This proves the tombstone fixes the graceful WR
   timeout caused by retransmit-to-dead-QP.
2. The sender teardown guards remained zero in both halves of the A/B. For this
   workload, the demonstrated failure mechanism is receiver-side destroyed-QP
   no-QP handling, not sender-side retransmit into a closing path.
3. The tombstone-off half did not reproduce the older strix-2 hard reset. It
   failed gracefully with retry exhaustion and the box stayed up. Therefore
   this A/B does not prove the sender teardown guard is the crash-fixing path;
   the guard counters were zero precisely when tombstones were disabled.
4. The older hard reset remains uncaptured and unexplained by this row. A
   crash-era reproduction would need the sender guards disabled as well, with
   netconsole/pstore armed, if we decide the remaining risk justifies the
   destructive test.
5. Crash attribution also has uncontrolled kernel-side variables: these runs
   include the Nix-carried Thunderbolt XDomain hardening and resync patches.
   Those patches are part of the tested baseline, but they prevent attributing
   the absence of the old hard reset solely to GDA module changes.
6. After the A/B, both modules were reloaded and restored to safe defaults:
   `native_qp_tombstone_reack=1`, `native_ack_drop_every=0`,
   `qp_timeout_ms=30000`, `native_tx_max_inflight=6`.

Post-A/B clean sanity:

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=30000
native_ack_drop_every=0
native_qp_tombstone_reack=1
final_fence=1
port=18562
sender:   status=OK elapsed_sec=0.056416
receiver: status=OK elapsed_sec=0.056488

sender:
data_wr_send=512
data_wr_retransmit=0
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_rx_ack_match_retried=0
data_rx_ack_miss=0
data_rx_canceled=0
data_rx_no_qp=0

receiver:
data_tx_ack_drop_injected=0
data_rx_duplicate_ack=0
data_rx_no_qp=0
data_rx_no_qp_reack=0
data_rx_no_qp_error_ack=0
data_rx_canceled=0
```

### Unsafe Guard-Off Crash Reproduction

Added `native_unsafe_retransmit_teardown_guard_disable` as an explicitly
unsafe debug knob. Default is `0`, which keeps the sender retransmit teardown
guard enabled. Setting it to `1` keeps the observation/counter code but allows a
retry retransmit to proceed after selecting a tearing-down native path.

Both Strix hosts were redeployed/rebooted through Colmena with the new module:

```text
strix-1=/nix/store/4x5wcywcaxscxrds93hpbjq89cxw5h1g-nixos-system-strix-1-26.11pre-git
strix-2=/nix/store/f45362p11si1cx0fp6qbcmx958igwg1d-nixos-system-strix-2-26.11pre-git
kernel=7.0.10
```

Initial safe sanity, fenced:

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=30000
native_ack_drop_every=0
native_qp_tombstone_reack=1
native_retransmit_teardown_guard=1
final_fence=1
port=18563
sender:   status=OK elapsed_sec=0.054901
receiver: status=OK elapsed_sec=0.054981

sender:
data_wr_send=512
data_wr_retransmit=0
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_rx_ack=1024
data_rx_ack_matched=512
data_rx_ack_match_retried=0
data_rx_canceled=0
data_rx_no_qp=0

receiver:
data_tx_ack_ok=512
data_tx_ack_drop_checked=0 data_tx_ack_drop_injected=0
data_rx_duplicate_ack=0
data_rx_no_qp=0
data_rx_no_qp_reack=0
data_rx_no_qp_error_ack=0
data_rx_canceled=0
```

Unsafe crash-era row:

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=200
receiver native_ack_drop_every=2
native_qp_tombstone_reack=0 on both hosts
native_retransmit_teardown_guard=0 on both hosts
final_fence=0
port=18564
sender:   SSH died with exit code 255, no probe result
receiver: status=OK elapsed_sec=18.358444
```

`strix-2` reset during the sender run:

```text
pre-run boot:  155855bcc5c246dd81d6f6f9f74f3320
pre-run journal last persisted line: 2026-06-05 23:24:37.524
current boot:  73b5247a2da14cd992a024f0e2761e86
current boot start: 2026-06-05 23:26:29
post-reset uptime when checked: <1 minute
pstore: empty
netconsole: no panic/oops captured for this reset
```

Receiver (`strix-1`) counters after the reset:

```text
data_tx_posted=3136 data_tx_completed=3136
data_tx_canceled=0 data_tx_errors=0
data_rx_completed=7128
data_rx_canceled=4096
data_rx_repost_failed=0
data_rx_bad_frame=0 data_rx_bad_header=0
data_rx_send=7009
data_tx_ack_ok=2913
native_tx_send_ack=2914
data_tx_ack_drop_checked=2913 data_tx_ack_drop_injected=1456
data_rx_duplicate_ack=2401
data_rx_no_qp=119
data_rx_no_qp_reack=0
data_rx_no_qp_error_ack=0
```

Receiver journal showed the peer disappearing during the run:

```text
2026-06-05 23:25:42 thunderbolt retimers disconnected
2026-06-05 23:25:42 thunderbolt_ibverbs unregistered per-rail ib_devices
2026-06-05 23:25:42 thunderbolt 0-2/1-2 host disconnected
2026-06-05 23:26:37 new XDomain/peer bring-up after strix-2 reboot
```

After restoring safe defaults and reloading both modules, a final fenced sanity
row passed:

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=30000
native_ack_drop_every=0
native_qp_tombstone_reack=1
native_retransmit_teardown_guard=1
final_fence=1
port=18565
sender:   status=OK elapsed_sec=0.054349
receiver: status=OK elapsed_sec=0.054420

sender:
data_wr_send=512
data_wr_retransmit=0
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_rx_ack=1024
data_rx_ack_matched=512
data_rx_ack_match_retried=0
data_rx_canceled=0
data_rx_no_qp=0

receiver:
data_tx_ack_ok=512
data_tx_ack_drop_checked=0 data_tx_ack_drop_injected=0
data_rx_duplicate_ack=0
data_rx_no_qp=0
data_rx_no_qp_reack=0
data_rx_no_qp_error_ack=0
data_rx_canceled=0
```

Direct same-build guard-on contrast:

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=200
receiver native_ack_drop_every=2
native_qp_tombstone_reack=0 on both hosts
native_retransmit_teardown_guard=1 on both hosts
final_fence=0
port=18566
sender:   failed, wc error wr_id=513 status=12
receiver: status=OK elapsed_sec=18.357129
pstore: empty on both hosts
netconsole: explicit <0> markers from both hosts reached collector
```

Sender (`strix-2`) counters:

```text
data_wr_send=512
data_wr_retransmit=262
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_wr_retry_exhausted=1
data_wr_timeout=1
data_rx_ack=2802
data_rx_ack_matched=650
data_rx_ack_match_retried=255
data_rx_ack_match_over_64ms=255
data_rx_late_ack=2152
data_rx_canceled=0
data_rx_no_qp=0
```

Receiver (`strix-1`) counters:

```text
data_tx_posted=3017 data_tx_completed=3017
data_rx_completed=6905
data_rx_canceled=0
data_tx_ack_ok=2802
native_tx_send_ack=2801
data_tx_ack_drop_checked=2802 data_tx_ack_drop_injected=1401
data_rx_duplicate_ack=2290
data_rx_no_qp=7
data_rx_no_qp_reack=0
data_rx_no_qp_error_ack=0
data_qp_tombstone_evicted=0
```

Interpretation:

1. Disabling both tombstone re-ack and the sender retransmit teardown guard
   reproduced the destructive `strix-2` hard reset. On the same deployed build,
   the tombstone-off/guard-on contrast failed gracefully with retry exhaustion
   and the host stayed up. That is strong evidence the guard is not cosmetic:
   removing it reopens the crash-era behavior.
2. This still did not produce a stack. The sender rebooted without pstore
   contents and without a netconsole panic/oops. Persistent journald has no
   final kernel line after the unsafe parameters were written; normal logging
   stopped before reset.
3. The old receiver-side near-miss signature returned at the same time:
   `data_rx_canceled=4096` and `data_rx_no_qp=119`, followed by peer/ring
   teardown on `strix-1`. The tombstone/guard-safe rows keep this at zero.
4. The same-build guard-on/tombstone-off row still had sender guard counters at
   zero, so there is no positive non-destructive counter trace from the exact
   sender line that reset. The honest statement is: guard-off makes the crash
   reproducible again; guard-on has so far converted this test family to either
   graceful timeout (tombstone off) or success (tombstone on).
5. Netconsole requires explicit high-priority markers (`<0>...`) for reliable
   operator breadcrumbs. A post-run `<0>` live check from both hosts reached the
   collector; the plain `/dev/kmsg` marker used before this unsafe row did not.
6. Operational default remains safe: `native_qp_tombstone_reack=1`,
   `native_unsafe_retransmit_teardown_guard_disable=0`,
   `native_ack_drop_every=0`, and `qp_timeout_ms=30000`.

Post-breadcrumb redeploy sanity:

After adding the emergency-level breadcrumb for the unsafe teardown retry path,
the branch was redeployed to both Strix hosts. The paired Colmena deploy first
failed for `strix-2` because `strix-1` was being used as a remote builder while
also being rebooted; retrying `strix-2` alone completed cleanly.

```text
strix-1=/nix/store/g4ncxvshjv7m031z6i9pdq0xbrrl9zq2-nixos-system-strix-1-26.11pre-git
strix-2=/nix/store/i63c0v4snl1q7hyibj06wnmaasfrpvx0-nixos-system-strix-2-26.11pre-git
kernel=7.0.10
```

Final fenced sanity on the deployed baseline:

```text
count=256 timeout_ms=60000 native_tx_max_inflight=6 qp_timeout_ms=30000
native_ack_drop_every=0
native_qp_tombstone_reack=1
native_retransmit_teardown_guard=1
final_fence=1
port=18567
sender:   status=OK elapsed_sec=0.054835
receiver: status=OK elapsed_sec=0.055007

sender:
data_wr_send=512
data_wr_retransmit=0
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_rx_ack=1024
data_rx_ack_matched=512
data_rx_ack_match_retried=0
data_rx_canceled=0
data_rx_no_qp=0

receiver:
data_tx_ack_ok=512
data_tx_ack_drop_checked=0 data_tx_ack_drop_injected=0
data_rx_duplicate_ack=0
data_rx_no_qp=0
data_rx_no_qp_reack=0
data_rx_no_qp_error_ack=0
data_rx_canceled=0
```

## 2026-06-06 Safe Soak And App-Benchmark Readiness

Baseline after the breadcrumb redeploy:

```text
kernel=7.0.10
strix-1=/nix/store/g4ncxvshjv7m031z6i9pdq0xbrrl9zq2-nixos-system-strix-1-26.11pre-git
strix-2=/nix/store/i63c0v4snl1q7hyibj06wnmaasfrpvx0-nixos-system-strix-2-26.11pre-git
rdma devices per host: usb4_rdma0 usb4_rdma1 usb4_rdma5 usb4_rdma6
native_qp_tombstone_reack=1
native_retransmit_teardown_guard=1
native_ack_drop_every=0
```

First safe soak:

```text
5 rounds x 4 RDMA devices x count=1024, fenced, no injected drops
result: 20/20 passed

sender strix-2:
data_wr_send=40960
data_wr_retransmit=6
data_rx_ack_match_retried=6
data_wr_timeout=0
data_rx_canceled=0
data_rx_no_qp=0

receiver strix-1:
data_tx_ack_ok=40966
data_rx_duplicate_ack=6
data_wr_timeout=0
data_rx_canceled=0
data_rx_no_qp=0
```

Longer safe soak:

```text
10 rounds x 4 RDMA devices x count=8192, fenced, no injected drops
result: 40/40 passed

sender strix-2:
data_wr_send=655360
data_wr_retransmit=38
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_tx_posted=5898582
data_tx_completed=5898582
data_tx_errors=0
data_tx_canceled=0
data_rx_ack=1310794
data_rx_ack_matched=655360
data_rx_ack_match_retried=38
data_rx_ack_match_max_ms=135
data_rx_ack_match_over_10ms=38
data_rx_ack_match_over_64ms=38
data_rx_late_ack=655434
data_rx_ack_miss=0
data_rx_ack_history_miss=0
data_rx_canceled=0
data_rx_repost_failed=0
data_rx_bad_frame=0
data_rx_bad_header=0
data_rx_no_qp=2

receiver strix-1:
data_tx_posted=1495125
data_tx_completed=1495125
data_tx_errors=0
data_tx_canceled=0
data_rx_completed=5898582
data_tx_ack_ok=655398
data_rx_duplicate_ack=38
data_rx_no_qp=0
data_rx_canceled=0
data_rx_repost_failed=0
data_rx_bad_frame=0
data_rx_bad_header=0
```

No `BUG`, `Oops`, `panic`, watchdog, or lockup line appeared in the netconsole
collector after the longer soak. `strix-2` dmesg showed ordinary ACK-loss
retransmits that matched after retry at about 71 to 135 ms.

Interpretation:

1. The safe defaults have now survived both injected retransmit/teardown tests
   and a non-injected two-host soak that naturally exercised 38 lost-ACK
   retransmits. The retransmit/tombstone backstop is not just an artificial
   injector path.
2. The E2E-off data path stayed clean: posted/completed matched, no TX errors,
   no TX cancels, no RX repost failures, and no bad frames or headers.
3. The remaining correctness signal is small but not zero: sender
   `data_rx_no_qp=2` appeared during the long soak without user-visible failure.
   This is residual QP-lifecycle churn to keep watching in app-level tests, not
   a blocker for the next smoke.
4. This is enough to run cautious application-level smoke, but not enough to
   declare the stack ready for long vLLM benchmarks. The next benchmark should
   be RCCL/rocSHMEM all-to-all with driver counters, not vLLM first.

App-benchmark packaging state:

```text
old RCCL test wrapper:
/nix/store/4bhvq0qphnq9ardka495pmji5f5130a0-rccl-tests-usb4-hostheap-gfx1151-2.14.1-local

old vLLM/PyTorch wrapper:
/nix/store/3mr3fgrn6znah88jrc42r2wh692x24km-vllm-env-therock-usb4-hostheap-gfx1151
```

strix-1 has both closures.
strix-2 initially lacked both closures and the TheRock/rocSHMEM/RCCL
dependencies needed by those wrappers. The RCCL test closure and dependencies
were staged to strix-2 with:

```text
nix copy --to ssh-ng://strix-2 --no-check-sigs \
  /nix/store/4bhvq0qphnq9ardka495pmji5f5130a0-rccl-tests-usb4-hostheap-gfx1151-2.14.1-local
```

The pre-existing app-level benchmark scripts in
`/mnt/Home/src/rocm-systems/projects/rocshmem/scripts/functional_tests` are the
right next bridge:

```text
usb4_raw_rocshmem_on_stream.sh
usb4_rccl_alltoall_perf.sh
usb4_rccl_smoke.sh
usb4_pytorch_smoke.sh
usb4_app_benchmark_matrix.sh
```

Run order after staging closures to both hosts:

1. Raw rocSHMEM `Alltoallmem_On_Stream`.
2. `rccl-tests alltoall_perf` with ordinary RCCL fallback and expected
   `dv_poll_wqes=0`.
3. `rccl-tests alltoall_perf` with host-stream GDA and expected positive
   `dv_poll_wqes`.
4. PyTorch/RCCL all-to-all host-stream smoke.

Do not treat vLLM TP=2 as a USB4 GDA benchmark yet. vLLM tensor parallelism
primarily stresses all-reduce/all-gather, while the USB4 RCCL/rocSHMEM route
that has been validated is all-to-all/all-to-allv. vLLM can be run as a
separate end-to-end application sanity check after the RCCL/rocSHMEM counter
smoke passes on this rebased kernel.

RCCL/rocSHMEM app smoke after staging:

```text
log root: /tmp/usb4-rccl-gda-20260606
hosts: strix-1,strix-2
MPI TCP iface: eno1
RCCL tests:
  /nix/store/4bhvq0qphnq9ardka495pmji5f5130a0-rccl-tests-usb4-hostheap-gfx1151-2.14.1-local/bin
RCCL:
  /nix/store/gkickq51z5rlysmdlp74sxswhcyib740-rccl-usb4-hostheap-gfx1151-2.28.9-local
rocSHMEM:
  /nix/store/6k7p8rayvrpq95r89q9hakc5967m5xmh-rocshmem-usb4-hostheap-gfx1151-3.4.0-local
```

Raw rocSHMEM `Alltoallmem_On_Stream`, `-a 76 -s 524288 -z 64 -n 20
-noverif`:

```text
262144 B: 682.25 us, 1.43 GB/s
524288 B: 653.44 us, 2.99 GB/s
dv_poll_wqes sum       +1440
dv_hard_error sum         +0
data_wr_copy_error sum    +0
data_wr_timeout sum       +0
data_tx_errors sum        +0
```

`rccl-tests alltoall_perf`, ordinary RCCL fallback,
`RCCL_ROCSHMEM_ENABLE=0`, `-b 262144 -e 524288 -n 3 -w 1 -c 1`:

```text
262144 B out-of-place: 252.25 us, 1.04 GB/s, wrong=0
524288 B out-of-place: 442.95 us, 1.18 GB/s, wrong=0
dv_poll_wqes sum          +0
dv_hard_error sum         +0
data_wr_copy_error sum    +0
data_wr_timeout sum       +0
data_tx_errors sum        +0
```

`rccl-tests alltoall_perf`, RCCL host-stream GDA,
`RCCL_ROCSHMEM_HOST_STREAM_ALLTOALL=1`, same sizes:

```text
262144 B out-of-place: 1819.16 us, 0.14 GB/s, wrong=0
524288 B out-of-place: 29699.6 us, 0.02 GB/s, wrong=0
dv_poll_wqes sum         +88
dv_hard_error sum         +0
data_wr_copy_error sum    +0
data_wr_timeout sum       +0
data_tx_errors sum        +0
```

`rccl-tests alltoall_perf`, default RCCL device-side GDA,
`RCCL_ROCSHMEM_HOST_STREAM_ALLTOALL=0`, same sizes:

```text
262144 B out-of-place: 2539.87 us, 0.10 GB/s, wrong=0
524288 B out-of-place: 59259.1 us, 0.01 GB/s, wrong=0
dv_poll_wqes sum         +88
dv_hard_error sum         +0
data_wr_copy_error sum    +0
data_wr_timeout sum       +0
data_tx_errors sum        +0
```

Full RCCL smoke suite with default RCCL device-side GDA:

```text
broadcast/all_gather/all_reduce:
  completed, dv_poll_wqes sum +0 as expected

alltoall_perf -b 65536 -e 1048576 -n 3 -w 1 -c 1:
  65536 B out-of-place: 740.74 us, wrong=0
  262144 B out-of-place: 1772.59 us, wrong=0
  1048576 B out-of-place: 48256.4 us, wrong=0
  dv_poll_wqes sum +140

alltoallv_perf -b 65536 -e 1048576 -n 3 -w 1 -c 1:
  65536 B out-of-place: 694.86 us, wrong=0
  262144 B out-of-place: 1722.33 us, wrong=0
  1048576 B out-of-place: 5579.94 us, wrong=0
  dv_poll_wqes sum +140

all smoke labels:
  dv_hard_error sum         +0
  data_wr_copy_error sum    +0
  data_wr_timeout sum       +0
  data_tx_errors sum        +0
```

Post-run driver health:

```text
strix-1:
dv_poll_wqes=948
dv_hard_error=0
data_wr_copy_error=0
data_wr_retransmit=15
data_wr_retry_exhausted=0
data_wr_timeout=0
data_tx_posted=1524809
data_tx_completed=1524809
data_tx_errors=0
data_rx_canceled=0
data_rx_no_qp=0
data_qp_tombstone_evicted=350

strix-2:
dv_poll_wqes=948
dv_hard_error=0
data_wr_copy_error=0
data_wr_retransmit=39
data_wr_retry_exhausted=0
data_wr_timeout=0
data_tx_posted=5927276
data_tx_completed=5927276
data_tx_errors=0
data_rx_canceled=0
data_rx_no_qp=2
data_qp_tombstone_evicted=350
```

No matching `BUG`, `Oops`, `panic`, watchdog, lockup, hard-error, timeout, or
GPU-fault lines appeared in the last 300 dmesg lines on either host.

Interpretation:

1. This is the first post-rebase application-level RCCL/rocSHMEM smoke on the
   corrected GDA stack. It proves PyTorch-adjacent RCCL test binaries can drive
   all-to-all and all-to-allv through USB4 GDA on this kernel: DV counters move
   exactly on the routed collectives, and the fallback/control collectives stay
   off DV.
2. Correctness is good in these small runs: all checked RCCL tests report
   `wrong=0`, no DV hard errors, no copy errors, no WR timeouts, no TX errors,
   and posted/completed stay matched after the run.
3. Performance is still the application-level problem. Raw rocSHMEM at 524 KiB
   is about 653 us, ordinary RCCL fallback is about 443 us, while RCCL GDA is
   much slower and size-sensitive. That keeps the optimization target in the
   RCCL/rocSHMEM integration path, not the kernel data path.
4. App-level QP churn hit the tombstone LRU cap (`data_qp_tombstone_evicted=350`
   on both hosts). No late no-QP re-ack or timeout followed in this run, but the
   cap is no longer hypothetical under application workloads. Before longer
   benchmark sweeps, raise the cap or make it tunable so heavy QP churn cannot
   evict a tombstone that a late retransmit still needs.

## 2026-06-06 Tombstone Cap Deploy And RCCL Re-Smoke

Implemented and deployed a tunable destroyed-QP tombstone cap:

```text
default native_qp_tombstone_max=4096
module param: /sys/module/thunderbolt_ibverbs/parameters/native_qp_tombstone_max
debugfs summary: native_qp_tombstone_max: 4096
```

Deployment used a clean detached worktree at `89bcec0` and a temporary
`nixos-config` lock pointing at that path, then sequential Colmena boot/reboot:

```text
strix-1=/nix/store/9zfprpxqf4dhl0r0i04yw4ffrv7nfkmz-nixos-system-strix-1-26.11pre-git
strix-2=/nix/store/w7smq1fxv81dm8wmldxnq6j5bq209k9c-nixos-system-strix-2-26.11pre-git
kernel=7.0.10
rdma devices per host: usb4_rdma0 usb4_rdma1 usb4_rdma5 usb4_rdma6
native_qp_tombstone_reack=Y
native_unsafe_retransmit_teardown_guard_disable=N
native_ack_drop_every=0
native_qp_tombstone_max=4096
native_e2e=-1
```

Baseline note: `strix-1` showed `data_rx_canceled=4096` immediately after boot,
before the RCCL re-smoke. During the re-smoke the `data_rx_canceled` delta stayed
zero, so this was treated as a boot/reload baseline counter rather than a new
runtime regression.

Post-deploy full RCCL smoke:

```text
log root: /tmp/usb4-rccl-gda-20260606-89bcec0/smoke-device
MPI TCP iface: eno1
RCCL tests:
  /nix/store/4bhvq0qphnq9ardka495pmji5f5130a0-rccl-tests-usb4-hostheap-gfx1151-2.14.1-local/bin
RCCL:
  /nix/store/gkickq51z5rlysmdlp74sxswhcyib740-rccl-usb4-hostheap-gfx1151-2.28.9-local
rocSHMEM:
  /nix/store/6k7p8rayvrpq95r89q9hakc5967m5xmh-rocshmem-usb4-hostheap-gfx1151-3.4.0-local
```

Result:

```text
broadcast/all_gather/all_reduce:
  completed, no DV assertion expected

alltoall_perf -b 65536 -e 1048576 -n 3 -w 1 -c 1:
  65536 B out-of-place: 567.55 us, wrong=0
  262144 B out-of-place: 1294.55 us, wrong=0
  1048576 B out-of-place: 4342.33 us, wrong=0
  dv_poll_wqes sum +140

alltoallv_perf -b 65536 -e 1048576 -n 3 -w 1 -c 1:
  65536 B out-of-place: 1271.59 us, wrong=0
  262144 B out-of-place: 82503.1 us, wrong=0
  1048576 B out-of-place: 188222 us, wrong=0
  dv_poll_wqes sum +140

all smoke labels:
  dv_hard_error sum              +0
  data_wr_copy_error sum         +0
  data_wr_timeout sum            +0
  data_wr_retry_exhausted sum    +0
  data_tx_errors sum             +0
  data_rx_canceled sum           +0
  data_rx_no_qp sum              +0
  data_rx_no_qp_reack sum        +0
  data_rx_no_qp_error_ack sum    +0
  data_qp_tombstone_evicted sum  +0
```

Post-run driver health:

```text
strix-1:
dv_poll_wqes=140
dv_hard_error=0
data_wr_copy_error=0
data_wr_retransmit=18
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_tx_posted=18234
data_tx_completed=18234
data_tx_errors=0
data_rx_canceled=4096
data_rx_duplicate_ack=1
data_rx_no_qp=0
data_rx_no_qp_reack=0
data_rx_no_qp_error_ack=0
data_qp_tombstone_evicted=0

strix-2:
dv_poll_wqes=140
dv_hard_error=0
data_wr_copy_error=0
data_wr_retransmit=1
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_tx_posted=16449
data_tx_completed=16449
data_tx_errors=0
data_rx_canceled=0
data_rx_duplicate_ack=10
data_rx_no_qp=0
data_rx_no_qp_reack=0
data_rx_no_qp_error_ack=0
data_qp_tombstone_evicted=0
```

No matching `BUG`, `Oops`, `panic`, watchdog, lockup, hard-error, timeout,
GPU-fault, or tombstone-cap warning appeared in the last 400 dmesg lines on
either host.

Interpretation:

1. The 4096 tombstone cap closes the immediate app-level QP-churn issue seen
   with the old 128 cap: the same RCCL smoke that previously evicted 350
   tombstones per host now evicts zero.
2. RCCL/rocSHMEM all-to-all and all-to-allv are now reasonable application-level
   correctness smoke targets on this GDA branch. They exercise DV, report
   `wrong=0`, and leave the driver in a clean state.
3. This is still smoke, not a performance-ready conclusion. The GDA collective
   path is functionally alive, but performance remains volatile and much slower
   than the fallback path in several rows, especially alltoallv.

### RCCL All-To-All Comparison After Cap Deploy

Ran a small post-cap comparison on the same deployed kernel:

```text
log root: /tmp/usb4-rccl-gda-20260606-89bcec0/compare
sizes: 262144, 524288
iters/warmup: 20/5
validation: disabled for the three-way comparison
MPI TCP iface: eno1
```

Results:

```text
fallback RCCL, RCCL_ROCSHMEM_ENABLE=0:
  262144 B out-of-place: 218816 us, in-place: 219.99 us
  524288 B out-of-place: 423.41 us, in-place: 218990 us
  dv_poll_wqes sum +0

host-stream GDA, RCCL_ROCSHMEM_HOST_STREAM_ALLTOALL=1:
  262144 B out-of-place: 1233.93 us, in-place: 1238.99 us
  524288 B out-of-place: 2335.24 us, in-place: 2342.16 us
  dv_poll_wqes sum +376

default/device GDA, RCCL_ROCSHMEM_HOST_STREAM_ALLTOALL=0:
  262144 B out-of-place: 1653.54 us, in-place: 6800.15 us
  524288 B out-of-place: 20536.6 us, in-place: 17430.8 us
  dv_poll_wqes sum +376
```

The fallback row had two obvious ~219 ms outliers. A validation-enabled fallback
rerun with 5/2 iters/warmup immediately returned to the expected range:

```text
log root: /tmp/usb4-rccl-gda-20260606-89bcec0/compare/fallback-check
262144 B out-of-place: 201.14 us, in-place: 187.57 us, wrong=0
524288 B out-of-place: 365.50 us, in-place: 363.80 us, wrong=0
dv_poll_wqes sum +0
```

All four comparison runs were correctness-clean:

```text
dv_hard_error sum              +0
data_wr_copy_error sum         +0
data_wr_timeout sum            +0
data_wr_retry_exhausted sum    +0
data_tx_errors sum             +0
data_rx_canceled sum           +0
data_rx_no_qp sum              +0
data_rx_no_qp_reack sum        +0
data_rx_no_qp_error_ack sum    +0
data_qp_tombstone_evicted sum  +0
```

Post-comparison health:

```text
strix-1:
dv_poll_wqes=516
data_wr_retransmit=20
data_wr_retry_exhausted=0
data_wr_timeout=0
data_tx_posted=42227
data_tx_completed=42227
data_tx_errors=0
data_rx_canceled=4096
data_rx_no_qp=0
data_qp_tombstone_evicted=0

strix-2:
dv_poll_wqes=516
data_wr_retransmit=11
data_wr_retry_exhausted=0
data_wr_timeout=0
data_tx_posted=40873
data_tx_completed=40873
data_tx_errors=0
data_rx_canceled=0
data_rx_no_qp=0
data_qp_tombstone_evicted=0
```

Interpretation:

1. The app-level all-to-all path is now suitable for cautious correctness and
   performance characterization: fallback stays off DV, GDA modes move DV
   counters, and all modes leave the driver clean.
2. Performance conclusions still need repeated, validation-enabled sweeps. The
   fallback outliers show that single rows are not stable enough to rank
   implementations without repetitions and outlier handling.
3. At these sizes, host-stream GDA is consistently slower than the validated
   fallback rerun, and default/device GDA is slower and more volatile than
   host-stream GDA. The next optimization work should stay in RCCL/rocSHMEM
   integration before moving to vLLM-scale measurements.

### Repeated RCCL All-To-All Gate

Ran a repeated, validation-enabled all-to-all gate after the one-off comparison:

```text
sizes: 262144, 524288, 1048576
iters/warmup: 5/2
validation: enabled (-c 1)
MPI TCP iface: eno1
counter hosts: strix-1,strix-2
```

The first mixed sequence interleaved fallback, host-stream GDA, and device GDA:

```text
log root: /tmp/usb4-rccl-gda-20260606-89bcec0/repeated-alltoall-050803
completed:
  fallback: reps 1-3 complete
  host-stream GDA: reps 1-3 complete
  device GDA: reps 1-3 complete
stopped:
  fallback rep 4 returned RCCL/NCCL unhandled system error after completing
  262144 and 524288 rows
```

The fallback failure did not show a driver failure signature:

```text
dv_poll_wqes sum              +0
dv_hard_error sum             +0
data_wr_copy_error sum        +0
data_wr_timeout sum           +0
data_wr_retry_exhausted sum   +0
data_tx_errors sum            +0
data_rx_canceled sum          +0
data_rx_no_qp sum             +0
data_qp_tombstone_evicted sum +0
data_tx_posted sum            +13599
data_tx_completed sum         +13599
```

No leftover MPI/RCCL processes remained afterwards. A fallback-only rerun with
`NCCL_DEBUG=INFO` passed 5/5:

```text
log root: /tmp/usb4-rccl-gda-20260606-89bcec0/fallback-only-051108

fallback RCCL, RCCL_ROCSHMEM_ENABLE=0:
  262144 B out-of-place median: 2307.05 us (min 2006.47, max 2608.32)
  524288 B out-of-place median: 2395.38 us (min 1995.69, max 2796.42)
  1048576 B out-of-place median: 2594.82 us (min 2391.38, max 2795.56)
  dv_poll_wqes sum +0 for every rep
```

Host-stream GDA isolated rerun:

```text
log root: /tmp/usb4-rccl-gda-20260606-89bcec0/hoststream-only-051211

host-stream GDA, RCCL_ROCSHMEM_HOST_STREAM_ALLTOALL=1:
  262144 B out-of-place median: 3327.68 us (min 3121.81, max 3727.74)
  524288 B out-of-place median: 4466.82 us (min 4248.42, max 5019.20)
  1048576 B out-of-place median: 7479.26 us (min 7203.55, max 7597.54)
  dv_poll_wqes sum +192 for every rep
```

Device/default GDA isolated rerun:

```text
log root: /tmp/usb4-rccl-gda-20260606-89bcec0/device-only-051300

device/default GDA, RCCL_ROCSHMEM_HOST_STREAM_ALLTOALL=0:
  262144 B out-of-place median: 5580.57 us (min 5570.04, max 6770.07)
  524288 B out-of-place median: 6586.57 us (min 6188.54, max 6590.24)
  1048576 B out-of-place median: 9384.91 us (min 9184.05, max 9982.36)
  dv_poll_wqes sum +192 for every rep
```

All isolated repeated runs were correctness-clean:

```text
wrong=0 for every out-of-place validated row
dv_hard_error sum              +0
data_wr_copy_error sum         +0
data_wr_timeout sum            +0
data_wr_retry_exhausted sum    +0
data_tx_errors sum             +0
data_rx_canceled sum           +0
data_rx_no_qp sum              +0
data_rx_no_qp_reack sum        +0
data_rx_no_qp_error_ack sum    +0
data_qp_tombstone_evicted sum  +0
```

Final post-gate driver health:

```text
strix-1:
dv_poll_wqes=2052
dv_hard_error=0
data_wr_copy_error=0
data_wr_retransmit=21
data_wr_retry_exhausted=0
data_wr_timeout=0
data_tx_posted=177183
data_tx_completed=177183
data_tx_errors=0
data_rx_canceled=4096
data_rx_no_qp=0
data_qp_tombstone_evicted=0

strix-2:
dv_poll_wqes=2052
dv_hard_error=0
data_wr_copy_error=0
data_wr_retransmit=22
data_wr_retry_exhausted=0
data_wr_timeout=0
data_tx_posted=176749
data_tx_completed=176749
data_tx_errors=0
data_rx_canceled=0
data_rx_no_qp=0
data_qp_tombstone_evicted=0
```

No matching `BUG`, `Oops`, `panic`, watchdog, lockup, hard-error, timeout,
GPU-fault, or tombstone-cap warning appeared in the last 400 dmesg lines on
either host.

Interpretation:

1. This is the first balanced repeated application benchmark gate on the rebased
   GDA stack. It clears the kernel correctness bar: repeated validated
   all-to-all through fallback, host-stream GDA, and device/default GDA leaves
   the driver clean with no tombstone evictions.
2. The performance ranking is now stable enough to act on for this size range:
   fallback is fastest, host-stream GDA is about 1.4x/1.9x/2.9x slower at
   256 KiB/512 KiB/1 MiB, and device/default GDA is slower again.
3. The mixed-sequence fallback failure is real but not yet attributed to GDA:
   it happened with DV inactive, the kernel counters were clean, and a
   fallback-only 5x rerun passed. Treat it as an app/RCCL benchmark-harness
   flake to watch, not a GDA correctness failure.

### Repeated RCCL All-To-Allv Gate

Ran a smaller repeated all-to-allv gate because `alltoallv_perf` has shown much
larger latency swings than `alltoall_perf`:

```text
log root: /tmp/usb4-rccl-gda-20260606-89bcec0/repeated-alltoallv-051614
sizes: 65536, 131072, 262144, 524288
iters/warmup: 5/2
validation: enabled (-c 1)
MPI TCP iface: eno1
counter hosts: strix-1,strix-2
```

The benchmark wrapper itself completed all nine runs successfully. The enclosing
login shell returned non-zero only after the run because `/etc/bash_logout`
references an unset variable under `set -u`; no RCCL row failed.

Fallback RCCL:

```text
fallback RCCL, RCCL_ROCSHMEM_ENABLE=0:
  65536 B out-of-place median: 1980.56 us (min 1830.12, max 1995.29)
  131072 B out-of-place median: 2197.82 us (min 1999.63, max 2298.53)
  262144 B out-of-place median: 2093.38 us (min 1998.46, max 2199.13)
  524288 B out-of-place median: 2396.52 us (min 1996.82, max 2595.33)
  dv_poll_wqes sum +0 for every rep
```

Host-stream GDA:

```text
host-stream GDA, RCCL_ROCSHMEM_HOST_STREAM_ALLTOALL=1:
  65536 B out-of-place median: 2769.82 us (min 2549.08, max 2782.54)
  131072 B out-of-place median: 2893.60 us (min 2718.38, max 2913.57)
  262144 B out-of-place median: 3316.22 us (min 3091.31, max 4092.92)
  524288 B out-of-place median: 4729.65 us (min 4465.75, max 5730.65)
  dv_poll_wqes sum +256 for every rep
```

Device/default GDA:

```text
device/default GDA, RCCL_ROCSHMEM_HOST_STREAM_ALLTOALL=0:
  65536 B out-of-place median: 5176.12 us (min 4577.30, max 5578.00)
  131072 B out-of-place median: 4791.44 us (min 3987.76, max 5192.52)
  262144 B out-of-place median: 5591.29 us (min 5393.34, max 5993.62)
  524288 B out-of-place median: 6587.20 us (min 6389.95, max 7200.99)
  dv_poll_wqes sum +256 for every rep
```

All all-to-allv runs were correctness-clean:

```text
wrong=0 for every out-of-place validated row
dv_hard_error sum              +0
data_wr_copy_error sum         +0
data_wr_timeout sum            +0
data_wr_retry_exhausted sum    +0
data_tx_errors sum             +0
data_rx_canceled sum           +0
data_rx_no_qp sum              +0
data_rx_no_qp_reack sum        +0
data_rx_no_qp_error_ack sum    +0
data_qp_tombstone_evicted sum  +0
```

Final post-all-to-allv health:

```text
strix-1:
dv_poll_wqes=2820
dv_hard_error=0
data_wr_copy_error=0
data_wr_retransmit=21
data_wr_retry_exhausted=0
data_wr_timeout=0
data_tx_posted=205647
data_tx_completed=205647
data_tx_errors=0
data_rx_canceled=4096
data_rx_no_qp=0
data_qp_tombstone_evicted=0

strix-2:
dv_poll_wqes=2820
dv_hard_error=0
data_wr_copy_error=0
data_wr_retransmit=25
data_wr_retry_exhausted=0
data_wr_timeout=0
data_tx_posted=205254
data_tx_completed=205254
data_tx_errors=0
data_rx_canceled=0
data_rx_no_qp=0
data_qp_tombstone_evicted=0
```

No matching `BUG`, `Oops`, `panic`, watchdog, lockup, hard-error, timeout,
GPU-fault, or tombstone-cap warning appeared in the last 400 dmesg lines on
either host.

Interpretation:

1. `alltoallv_perf` now clears the same cautious app-level correctness bar as
   `alltoall_perf`: fallback stays off DV, both GDA modes exercise DV, and the
   kernel remains clean after repeated validated rows.
2. The performance ranking matches all-to-all: fallback fastest, host-stream GDA
   slower, device/default GDA slower again.
3. The GDA path is now ready for broader RCCL/rocSHMEM characterization. vLLM is
   still a separate end-to-end sanity target, not the best next diagnostic,
   because the validated GDA collectives are all-to-all/all-to-allv rather than
   vLLM's dominant all-reduce/all-gather traffic.

### PyTorch/RCCL All-To-All Smoke

Staged the vLLM/PyTorch wrapper closure to `strix-2`:

```text
wrapper:
  /nix/store/3mr3fgrn6znah88jrc42r2wh692x24km-vllm-env-therock-usb4-hostheap-gfx1151
closure size:
  37194634176 bytes, 451 paths
copied missing paths to strix-2:
  151
```

The wrapper explicitly preloads the USB4 RCCL package used in the PyTorch smoke:

```text
RCCL:
  /nix/store/74kx31vim3ynwpivjlxq04wbdl62nrbg-rccl-usb4-hostheap-gfx1151-2.28.9-local/lib/librccl.so.1
rocSHMEM:
  /nix/store/78p5x7mgw4clcr567p99h9bz8r9783lc-rocshmem-usb4-hostheap-gfx1151-3.4.0-local
```

Ran three PyTorch distributed all-to-all smokes:

```text
collective: all_to_all
sizes: 65536, 262144
iters: 2
validation: enabled
MPI/control iface: eno1
master: 192.168.23.136
```

Fallback PyTorch, GDA disabled:

```text
log root: /tmp/usb4-rccl-gda-20260606-89bcec0/pytorch-fallback-smoke
Librccl path:
  /nix/store/74kx31vim3ynwpivjlxq04wbdl62nrbg-rccl-usb4-hostheap-gfx1151-2.28.9-local/lib/librccl.so.1

all_to_all_single bytes=65536:
  1697.2 us/iter, gpu=1410.4 us/iter
all_to_all_single bytes=262144:
  349.5 us/iter, gpu=345.0 us/iter

dv_poll_wqes sum              +0
dv_hard_error sum             +0
data_wr_copy_error sum        +0
data_wr_timeout sum           +0
data_tx_errors sum            +0
```

Host-stream GDA PyTorch:

```text
log root: /tmp/usb4-rccl-gda-20260606-89bcec0/pytorch-hoststream-smoke
RCCL_ROCSHMEM_HOST_STREAM_ALLTOALL=1
RCCL_ROCSHMEM_SOURCE_HEAP=1
RCCL_ROCSHMEM_DEST_HEAP=1

all_to_all_single bytes=65536:
  880.0 us/iter, gpu=871.0 us/iter
all_to_all_single bytes=262144:
  5894.5 us/iter, gpu=5890.1 us/iter

dv_poll_wqes sum              +24
dv_admission_attempts sum     +24
dv_backpressure_retry sum     +0
dv_fence_retry sum            +0
dv_hard_error sum             +0
data_wr_copy_error sum        +0
data_wr_timeout sum           +0
data_tx_errors sum            +0
```

Device/default GDA PyTorch:

```text
log root: /tmp/usb4-rccl-gda-20260606-89bcec0/pytorch-device-smoke
RCCL_ROCSHMEM_HOST_STREAM_ALLTOALL=0
RCCL_ROCSHMEM_SOURCE_HEAP=1
RCCL_ROCSHMEM_DEST_HEAP=1

all_to_all_single bytes=65536:
  71930.9 us/iter, gpu=71916.2 us/iter
all_to_all_single bytes=262144:
  139192.2 us/iter, gpu=139188.0 us/iter

dv_poll_wqes sum              +24
dv_admission_attempts sum     +24
dv_backpressure_retry sum     +0
dv_fence_retry sum            +0
dv_hard_error sum             +0
data_wr_copy_error sum        +0
data_wr_timeout sum           +0
data_tx_errors sum            +0
```

Final post-PyTorch driver health:

```text
strix-1:
dv_poll_wqes=2844
dv_hard_error=0
data_wr_copy_error=0
data_wr_retransmit=22
data_wr_retry_exhausted=0
data_wr_timeout=0
data_tx_posted=207751
data_tx_completed=207751
data_tx_errors=0
data_rx_canceled=4096
data_rx_no_qp=0
data_qp_tombstone_evicted=0

strix-2:
dv_poll_wqes=2844
dv_hard_error=0
data_wr_copy_error=0
data_wr_retransmit=27
data_wr_retry_exhausted=0
data_wr_timeout=0
data_tx_posted=207467
data_tx_completed=207467
data_tx_errors=0
data_rx_canceled=0
data_rx_no_qp=0
data_qp_tombstone_evicted=0
```

No matching `BUG`, `Oops`, `panic`, watchdog, lockup, hard-error, timeout,
GPU-fault, or tombstone-cap warning appeared in the last 500 dmesg lines on
either host.

Interpretation:

1. This is now an actual PyTorch/RCCL application-level smoke, not just
   `rccl-tests`: PyTorch distributed all-to-all loaded the USB4 RCCL wrapper,
   validated successfully, and exercised the GDA path when enabled.
2. Kernel correctness still holds through the PyTorch layer: no DV hard errors,
   copy errors, WR timeouts, TX errors, no-QP events, or tombstone evictions.
3. Performance remains the blocker. Host-stream GDA beats fallback at 64 KiB in
   this tiny run but is much slower at 256 KiB; device/default GDA is unusably
   slow here. That points back at RCCL/rocSHMEM integration and mode selection,
   not a kernel correctness blocker.
4. vLLM can now be staged/run as an end-to-end sanity check because the Python
   closure is present on both Strix hosts, but it should not be treated as the
   primary GDA benchmark until the all-to-all/all-to-allv RCCL path is faster and
   less mode-sensitive.

### Reusable app gate runner

Added `tbv_app_gate.sh` and `tbv_pytorch_smoke.py` to `bench-tools`, exposed the
runner as the `tbv-app-gate` flake app, and extended the script syntax check to
cover the new shell/Python entrypoints.

Local/package checks:

```text
bash -n userspace/bench/tbv_app_gate.sh
nix build .#bench-tools --no-link --print-out-paths
  /nix/store/zyc7yj9pqahk5bcas4w2iza452v1wzdv-thunderbolt-ibverbs-bench-tools-0.1.0
nix build .#checks.x86_64-linux.script-syntax --no-link --print-out-paths
  /nix/store/czly4i1kh8q66byi21n1pmyd2ym7gnba-thunderbolt-ibverbs-script-syntax-0.1.0
nix run .#tbv-app-gate -- --help
```

RCCL live gate, all-to-all:

```text
log root: /tmp/tbv-app-gate-smoke-20260606-053328
collective: alltoall
modes: fallback, hoststream
sizes: 65536, 262144
iters/warmup/reps: 1/1/1
status: pass

fallback:
  65536: 137.97 us
  262144: 186.14 us
  dv_poll_wqes sum +0
  data_tx_posted/completed sum +2151/+2151

hoststream:
  65536: 155.40 us
  262144: 241.18 us
  dv_poll_wqes sum +84
  data_tx_posted/completed sum +1062/+1062
```

RCCL live gate, all-to-allv:

```text
log root: /tmp/tbv-app-gate-smoke-alltoallv-20260606-053408
collective: alltoallv
modes: fallback, hoststream, device
sizes: 65536, 131072
iters/warmup/reps: 1/1/1
status: pass

fallback:
  65536: 145.69 us
  131072: 160.95 us
  dv_poll_wqes sum +0
  data_tx_posted/completed sum +1150/+1150

hoststream:
  65536: 1166.24 us
  131072: 1242.55 us
  dv_poll_wqes sum +56
  data_tx_posted/completed sum +517/+517

device:
  65536: 1092.73 us
  131072: 1653.52 us
  dv_poll_wqes sum +56
  data_tx_posted/completed sum +517/+517
```

PyTorch live gate:

```text
log root: /tmp/tbv-app-gate-pytorch-smoke-20260606-053500
collective: all_to_all
sizes: 65536
iters/reps: 1/3
expected RCCL:
  /nix/store/74kx31vim3ynwpivjlxq04wbdl62nrbg-rccl-usb4-hostheap-gfx1151-2.28.9-local/lib/librccl.so.1
status: pass

fallback:
  rep1: 291.5 us, dv_poll_wqes sum +0, retransmit sum +0, tx +525/+525
  rep2: 224.0 us, dv_poll_wqes sum +0, retransmit sum +0, tx +526/+526
  rep3: 230.9 us, dv_poll_wqes sum +0, retransmit sum +0, tx +526/+526

hoststream:
  rep1: 1795.5 us, dv_poll_wqes sum +8, retransmit sum +0, tx +397/+397
  rep2: 1933.6 us, dv_poll_wqes sum +8, retransmit sum +0, tx +397/+397
  rep3: 73734.1 us, dv_poll_wqes sum +8, retransmit sum +1, tx +416/+416
```

For all three gates:

```text
dv_hard_error sum                    +0
data_wr_copy_error sum               +0
data_wr_timeout sum                  +0
data_wr_retry_exhausted sum          +0
data_wr_retransmit_closing_qp sum    +0
data_wr_retransmit_no_live_path sum  +0
data_wr_retransmit_teardown_path sum +0
data_tx_errors sum                   +0
data_rx_canceled sum                 +0
data_rx_no_qp sum                    +0
data_rx_no_qp_reack sum              +0
data_qp_tombstone_evicted sum        +0
```

Final health after the app-gate smokes:

```text
strix-1:
dv_poll_wqes=2954
dv_hard_error=0
data_wr_copy_error=0
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_tx_posted=211850
data_tx_completed=211850
data_tx_errors=0
data_rx_canceled=4096
data_rx_no_qp=0
data_qp_tombstone_evicted=0

strix-2:
dv_poll_wqes=2954
dv_hard_error=0
data_wr_copy_error=0
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_wr_retry_exhausted=0
data_wr_timeout=0
data_tx_posted=211552
data_tx_completed=211552
data_tx_errors=0
data_rx_canceled=0
data_rx_no_qp=0
data_qp_tombstone_evicted=0
```

No matching `BUG`, `Oops`, `panic`, watchdog, lockup, hard-error, timeout,
GPU-fault, or AMDGPU reset/timeout appeared in the recent dmesg tail on either
host after the gates.

Interpretation:

1. The app-level gate is now reusable and Nix-packaged; it captures before/after
   debugfs summaries and fails the gate if the fallback path uses DV, the GDA
   path does not use DV, or any hard correctness counter increments.
2. The gate exercises the current correctness stack successfully through
   `rccl-tests` and PyTorch distributed all-to-all, with clean hard counters and
   balanced TX posted/completed deltas.
3. The performance signal remains poor/unstable above the kernel: host-stream
   PyTorch GDA can validate cleanly while taking 1.8-1.9 ms at 64 KiB, and one
   rep hit 73 ms with a single successful retransmit. That is not a kernel
   correctness failure, but it is the next application-level bottleneck to
   instrument in RCCL/rocSHMEM.

### PyTorch non-all-to-all scope check

Follow-up runner hardening:

- added `--dv-check auto|require|forbid|off`, so exploratory collectives can
  still capture DV deltas without turning "no DV" into a gate failure;
- changed PyTorch smoke staging from fixed `/tmp/tbv_pytorch_smoke.py` to a
  unique `/tmp/tbv_pytorch_smoke_${USER}_${pid}.py` default. The fixed path had
  become unwritable on one host after earlier runs.

Validation after the runner change:

```text
bash -n userspace/bench/tbv_app_gate.sh
nix build .#bench-tools --no-link --print-out-paths
  /nix/store/zyc7yj9pqahk5bcas4w2iza452v1wzdv-thunderbolt-ibverbs-bench-tools-0.1.0
nix build .#checks.x86_64-linux.script-syntax --no-link --print-out-paths
  /nix/store/czly4i1kh8q66byi21n1pmyd2ym7gnba-thunderbolt-ibverbs-script-syntax-0.1.0
```

Source fact checked in RCCL:

```text
projects/rccl/src/collectives.cc:
  ncclAlltoAll_impl selects ncclFuncAlltoAllGda when
  rcclUseAlltoAllGda(comm) && msgSize <= comm->rocshmemThreshold.

  ncclAlltoAllv_impl selects ncclFuncAlltoAllvGda, or routes regular
  all-to-allv through ncclAlltoAll_impl.

  ncclAllGather_impl selects AllGather/direct AllGather paths.
  ncclAllReduce_impl selects regular AllReduce or DDA IPC paths.

projects/rccl/src/enqueue.cc:
  rocSHMEM device work is populated only for ncclFuncAlltoAllGda and
  ncclFuncAlltoAllvGda.
```

Ran PyTorch all-reduce/all-gather with rocSHMEM env disabled and enabled:

```text
log root: /tmp/tbv-app-gate-pytorch-ar-ag-20260606-054048
collectives: all_reduce, all_gather
sizes: 65536, 262144
iters/reps: 2/1
dv_check: off
expected RCCL:
  /nix/store/74kx31vim3ynwpivjlxq04wbdl62nrbg-rccl-usb4-hostheap-gfx1151-2.28.9-local/lib/librccl.so.1
status: pass

fallback:
  all_reduce 65536: 3420.6 us/iter
  all_gather 65536: 126.0 us/iter
  all_reduce 262144: 415.6 us/iter
  all_gather 262144: 240.9 us/iter
  dv_poll_wqes sum +0
  data_tx_posted/completed sum +4515/+4515

hoststream env enabled:
  all_reduce 65536: 3603.3 us/iter
  all_gather 65536: 127.1 us/iter
  all_reduce 262144: 713.3 us/iter
  all_gather 262144: 1036.1 us/iter
  dv_poll_wqes sum +0
  data_tx_posted/completed sum +4515/+4515
```

Hard correctness deltas were zero in both modes:

```text
data_wr_retransmit sum                 +0
data_wr_timeout sum                    +0
data_wr_retry_exhausted sum            +0
data_wr_retransmit_closing_qp sum      +0
data_wr_retransmit_no_live_path sum    +0
data_wr_retransmit_teardown_path sum   +0
data_tx_errors sum                     +0
data_rx_canceled sum                   +0
data_rx_no_qp sum                      +0
data_qp_tombstone_evicted sum          +0
```

Final health:

```text
strix-1:
dv_poll_wqes=2954
data_tx_posted=216366
data_tx_completed=216366
data_tx_errors=0
data_rx_canceled=4096

strix-2:
dv_poll_wqes=2954
data_tx_posted=216066
data_tx_completed=216066
data_tx_errors=0
data_rx_canceled=0
```

Interpretation:

1. Current rocSHMEM GDA integration is scoped to RCCL all-to-all/all-to-allv.
   PyTorch all-reduce/all-gather do not exercise the DV/GDA path even with the
   rocSHMEM env enabled; `dv_poll_wqes` stayed flat.
2. A vLLM run is now useful as an end-to-end stack/lifecycle smoke, but it is
   unlikely to benchmark the GDA path unless the workload issues all-to-all or
   the RCCL integration grows GDA support for vLLM-relevant collectives.
3. The kernel/driver path remains clean under these PyTorch collectives:
   software verbs traffic moves (`data_tx_posted/completed` grows), TX
   completions balance, and no hard correctness counter moves.

### vLLM tiny TP=2/Ray smoke

Follow-up runner hardening:

- added `--torch-validate 0|1`, so exploratory partial-mode PyTorch/RCCL runs
  can bypass value validation while still capturing timings and driver deltas.

Validation after the runner change:

```text
bash -n userspace/bench/tbv_app_gate.sh
nix build .#bench-tools --no-link --print-out-paths
  /nix/store/c1gfxcv78qzmsxx1irgsl9vx3z3jb45w-thunderbolt-ibverbs-bench-tools-0.1.0
nix build .#checks.x86_64-linux.script-syntax --no-link --print-out-paths
  /nix/store/cwwaih19zn82ndgcxqnvsglqj5qpv5wi-thunderbolt-ibverbs-script-syntax-0.1.0
```

The packaged vLLM/PyTorch wrapper contains `vllm`, `ray`, `transformers`, and
`torch`:

```text
/nix/store/3mr3fgrn6znah88jrc42r2wh692x24km-vllm-env-therock-usb4-hostheap-gfx1151
```

No real cached HF model was suitable for a network-free TP=2 smoke, so both
hosts were given a local dummy-load Qwen3-shaped model:

```text
/tmp/tbv-vllm-tiny-qwen3
source tokenizer/config: mlx-community/Qwen3-0.6B-4bit
hidden_size=128 intermediate_size=384 layers=2 heads=2 kv_heads=2
max_position_embeddings=512 dtype=float16
```

The first single-node vLLM smoke failed in vLLM's memory profiler because ROCm
reports the APU's unified memory as roughly `121.0 GiB` GPU memory and the
profiler asserts when initialization does not reduce the free-memory sample.
Explicit KV cache sizing bypasses that profiler path:

```text
single-node output:
  /tmp/tbv-vllm-tiny-single-kv-20260606-054728.json
  elapsed_time: 15.1156 s
  requests_per_second: 0.1323
  tokens_per_second: 1.5884
```

Two-node TP=2/Ray run:

```text
log root: /tmp/tbv-vllm-tiny-tp2-20260606-054849
model: /tmp/tbv-vllm-tiny-qwen3
load_format: dummy
tensor_parallel_size: 2
distributed_executor_backend: ray
kv_cache_memory_bytes: 16777216
num_prompts: 2
random_input_len/random_output_len: 8/4
dtype: float16
flags: --enforce-eager --disable-custom-all-reduce --disable-detokenize
status: pass
```

Relevant vLLM/Ray facts from the log:

```text
connected to Ray at 192.168.23.136:6379
world_size=2 backend=nccl
rank 0: strix-1, TP rank 0
rank 1: ip=192.168.23.192, TP rank 1
vLLM is using nccl==2.28.9
manual KV cache: reserved 0.02 GiB, skipped memory profiling
```

Benchmark output:

```text
elapsed_time: 20.450205951998214
num_requests: 2
total_num_tokens: 24
requests_per_second: 0.09779852607325833
tokens_per_second: 1.1735823128790999
```

Driver deltas across the TP=2 run:

```text
dv_poll_wqes sum                         +0
data_wr_retransmit sum                   +0
data_wr_timeout sum                      +0
data_wr_retry_exhausted sum              +0
data_wr_retransmit_closing_qp sum        +0
data_wr_retransmit_no_live_path sum      +0
data_wr_retransmit_teardown_path sum     +0
data_tx_errors sum                       +0
data_rx_canceled sum                     +0
data_rx_no_qp sum                        +0
data_qp_tombstone_evicted sum            +0
data_tx_posted/completed strix-1         +2467/+2467
data_tx_posted/completed strix-2         +2469/+2469
```

The Ray teardown prints an expected SIGTERM stack in the vLLM log, followed by
vLLM's own "please ignore" Ray shutdown message. The command exited 0 and both
hosts reported no running Ray instance afterward.

Interpretation:

1. vLLM TP=2 over Ray/NCCL now passes as an end-to-end lifecycle smoke on the
   Strix pair with the USB4 RCCL wrapper in the environment.
2. `dv_poll_wqes` stayed flat, matching the source-level scope check above:
   this vLLM smoke exercises the distributed stack, not the current GDA
   all-to-all path.
3. The explicit `--kv-cache-memory-bytes` flag is required for this APU smoke
   unless vLLM's ROCm memory profiler is fixed or taught about unified memory.

### PyTorch host-stream component matrix

Ran the PyTorch all-to-all smoke against the RCCL
`RCCL_ROCSHMEM_GDA_BENCH_MODE` component switch to isolate where the host-stream
GDA time is going. Validation was disabled because modes 1-5 intentionally run
partial pieces of the algorithm, not a full semantic all-to-all.

```text
log root: /tmp/tbv-app-gate-pytorch-hoststream-components-20260606-055226
collective: all_to_all
mode: hoststream
sizes: 65536, 262144
iters/reps: 2/1
torch validation: disabled
expected RCCL:
  /nix/store/74kx31vim3ynwpivjlxq04wbdl62nrbg-rccl-usb4-hostheap-gfx1151-2.28.9-local/lib/librccl.so.1
status: pass for modes 1, 2, 3, 4, 5, 0
```

Mode meanings:

```text
0: full copy-in + rocSHMEM exchange + copy-out
1: copy-in only
2: rocSHMEM exchange only
3: copy-out only
4: copy-in + copy-out
5: rocSHMEM exchange + copy-out
```

Timing and selected driver deltas:

```text
mode 1 copy-in only:
  65536:  1630.2 us/iter, gpu 1600.5
  262144: 39835.6 us/iter, gpu 39831.0
  dv_poll_wqes +16, retransmit +2, tx +823/+823

mode 2 rocSHMEM exchange only:
  65536:  1669.0 us/iter, gpu 1651.8
  262144: 39725.1 us/iter, gpu 39720.2
  dv_poll_wqes +16, retransmit +1, tx +753/+753

mode 3 copy-out only:
  65536:  3102.4 us/iter, gpu 3085.2
  262144: 39834.6 us/iter, gpu 39829.8
  dv_poll_wqes +16, retransmit +1, tx +754/+754

mode 4 copy-in + copy-out:
  65536:  1057.3 us/iter, gpu 1040.2
  262144: 3146.7 us/iter, gpu 3142.8
  dv_poll_wqes +16, retransmit +0, tx +686/+686

mode 5 rocSHMEM exchange + copy-out:
  65536:  1257.2 us/iter, gpu 1239.9
  262144: 79984.3 us/iter, gpu 79971.5
  dv_poll_wqes +16, retransmit +2, tx +823/+823

mode 0 full:
  65536:  2099.3 us/iter, gpu 2080.3
  262144: 43573.7 us/iter, gpu 43569.2
  dv_poll_wqes +16, retransmit +1, tx +753/+753
```

Hard correctness deltas were zero in all six modes:

```text
data_wr_timeout sum                    +0
data_wr_retry_exhausted sum            +0
data_wr_retransmit_closing_qp sum      +0
data_wr_retransmit_no_live_path sum    +0
data_wr_retransmit_teardown_path sum   +0
data_tx_errors sum                     +0
data_rx_canceled sum                   +0
data_rx_no_qp sum                      +0
data_qp_tombstone_evicted sum          +0
dv_backpressure_retry sum              +0
dv_fence_retry sum                     +0
```

Live health after the component matrix:

```text
strix-1:
verbs_qps=4
dv_poll_wqes=3002
data_wr_retransmit=27
data_wr_timeout=0
data_wr_retry_exhausted=0
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_tx_posted=221160
data_tx_completed=221160
data_tx_errors=0
data_rx_canceled=4096
data_rx_no_qp=0
data_qp_tombstone_evicted=0

strix-2:
verbs_qps=4
dv_poll_wqes=3002
data_wr_retransmit=30
data_wr_timeout=0
data_wr_retry_exhausted=0
data_wr_retransmit_closing_qp=0
data_wr_retransmit_no_live_path=0
data_wr_retransmit_teardown_path=0
data_tx_posted=220800
data_tx_completed=220800
data_tx_errors=0
data_rx_canceled=0
data_rx_no_qp=0
data_qp_tombstone_evicted=0
```

No matching `BUG`, `Oops`, `panic`, watchdog, lockup, hard-error, timeout,
GPU-fault, or AMDGPU reset/timeout appeared in the dmesg error scan after the
vLLM/component runs. The visible ACPI/Overdrive/watchdog/ramoops lines are boot
baseline noise, not new driver faults.

Interpretation:

1. The host-stream GDA path is still correctness-clean under PyTorch, but the
   256 KiB timing is dominated by a roughly 39-40 ms serialized component that
   appears in copy-in, exchange-only, and copy-out-only modes individually.
2. Copy-in+copy-out together is only 3.1 ms at 256 KiB, which contradicts a
   simple "both HIP copies are inherently 40 ms" explanation. The benchmark
   switch is likely changing synchronization/lifetime behavior around each
   partial component, so the next measurement needs RCCL/rocSHMEM-side phase
   timing rather than more end-to-end PyTorch repetition.
3. `dv_poll_wqes` is exactly +16 per mode and admission/fence/backpressure
   counters are clean, so the kernel DV path is not the obvious source of the
   40 ms plateau.
4. Small successful ACK retransmits still occur under these application-level
   runs (`data_wr_retransmit` +0..+2 per mode), but no timeout, retry
   exhaustion, teardown guard, or tombstone eviction counter moved. This remains
   expected control-channel loss handled by the current software reliability
   layer, not a correctness failure.

### 2026-06-06 RCCL host-stream timing packaging correction

Follow-up inspection found that the live RCCL package used for the PyTorch
host-stream component matrix was not built from the same host-stream diagnostic
source now in `/mnt/Home/src/rocm-systems`.

Evidence:

```text
tested librccl:
  /nix/store/74kx31vim3ynwpivjlxq04wbdl62nrbg-rccl-usb4-hostheap-gfx1151-2.28.9-local/lib/librccl.so.1

deriver source:
  /nix/store/axg3z5n0w1bhbfiza209bbj9rmd2i963-source

present in tested lib:
  RCCL_ROCSHMEM_HOST_STREAM_ALLTOALL
  RCCL_ROCSHMEM_GDA_BENCH_MODE

absent from tested lib:
  RCCL_ROCSHMEM_HOST_STREAM_TIMING
```

The deriver source's host-stream `ncclAlltoAll_impl()` path does not consult
`RCCL_ROCSHMEM_GDA_BENCH_MODE`; that knob is defined for the device enqueue
path. Therefore the 2026-06-06 PyTorch "component matrix" remains a useful
correctness/stability run, but its per-component timing interpretation is not
load-bearing until the host-stream mode/timing RCCL patch is rebuilt and used.

Packaging fix landed in the `nix-strix-halo` GDA worktree:

```text
/mnt/Home/src/nix-strix-halo/.worktrees/gda
commit 2811e61 therock: carry RCCL host-stream diagnostics patch
```

The patch is carried in both Nix source build paths:

```text
pkgs/therock/rocm-modules/rccl/rccl-usb4-host-stream-alltoall-diagnostics.diff
pkgs/therock/rocm-from-source/patches/rccl-usb4-host-stream-alltoall-diagnostics.patch
```

Validation:

1. The source-module patch dry-runs against the pinned sliced RCCL source:
   `/nix/store/vy4xg916mxq44hwfw29b78wh9vbdshd8-therock-rocm-source-rocm-systems-projects-rccl`.
2. The monolithic TheRock patch dry-runs against the full source tree:
   `/nix/store/gqqzrj0n81q0713k29wxm2pg8vi6kn0n-therock-rocm-source-gfx1151-full-6d2136cd12be`.
3. `therock-rocm-from-source-gfx1151-configure` applied the new RCCL patch to
   `collectives.cc` and `enqueue.cc`, reached `Configuring done`, and then
   failed only in configure-only fixup because no `therock-hip-clang++` wrapper
   is installed when the build phase is intentionally skipped.

Next app-level timing run must use a rebuilt RCCL whose `librccl.so.1` contains
`RCCL_ROCSHMEM_HOST_STREAM_TIMING`; otherwise `RCCL_ROCSHMEM_GDA_BENCH_MODE`
cannot be assumed to split host-stream copy-in/exchange/copy-out phases.

### 2026-06-06 PyTorch with rebuilt host-stream timing RCCL

Built RCCL from `/mnt/Home/src/rocm-systems/projects/rccl` commit
`a9c630f886` into:

```text
/mnt/Home/tmp/rccl-hoststream-timing-install-a9c630/lib/librccl.so.1
```

The rebuilt library contains:

```text
RCCL_ROCSHMEM_HOST_STREAM_TIMING
RCCL_ROCSHMEM_HOST_STREAM_ALLTOALL
RCCL_ROCSHMEM_GDA_BENCH_MODE
```

PyTorch-specific loader finding:

1. The raw TheRock SDK `lib` and `lib/rocm_sysdeps/lib` directories must not be
   placed on PyTorch's `LD_LIBRARY_PATH`; doing so segfaults the dynamic loader
   during `import torch`.
2. `LD_PRELOAD=/mnt/Home/tmp/.../librccl.so.1` is not sufficient. The TheRock
   PyTorch wheel calls `torch/_rocm_init.py`, which asks `rocm_sdk` to preload
   the shortname `rccl` by absolute wheel path. With only `LD_PRELOAD`, both the
   packaged RCCL and the rebuilt RCCL are mapped.
3. The app gate now installs a narrow PyTorch smoke override before importing
   torch: `TBV_TORCH_RCCL_LIB` intercepts only the `rocm_sdk` `rccl` preload
   shortname and substitutes the rebuilt RCCL. All other ROCm wheel preloads are
   left intact.
4. The smoke now reports mapped collective libraries from `/proc/self/maps`, and
   `--expected-rccl-lib` checks that `loaded_collective_lib=...` line rather
   than relying on RCCL log text or the command line.

Loader validation:

```text
log root: /tmp/tbv-app-gate-pytorch-standalone-rccl-smoke-a9c630-20260606-070325
status: pass
loaded_collective_lib:
  /mnt/Home/tmp/rccl-hoststream-timing-install-a9c630/lib/librccl.so.1.0
```

Corrected host-stream GDA smoke, with `RCCL_ROCSHMEM_THRESHOLD=1048576`
(`msgSize <= threshold` is the GDA condition):

```text
log root: /tmp/tbv-app-gate-pytorch-standalone-rccl-gda-smoke-a9c630-20260606-070422
status: pass
dv_poll_wqes sum: +4
hard counters: clean
timing emitted: yes
```

The previous exploratory run with `RCCL_ROCSHMEM_THRESHOLD=0` did not exercise
GDA; threshold is an upper bound, not a lower bound.

Load-bearing PyTorch host-stream component matrix:

```text
log root: /tmp/tbv-app-gate-pytorch-hoststream-timing-a9c630-20260606-070458
collective: torch.distributed all_to_all_single
sizes: 65536,262144 bytes/rank
iters: 2
expected RCCL: /mnt/Home/tmp/rccl-hoststream-timing-install-a9c630/lib/librccl.so.1
status: pass
```

Phase timing means across ranks/iterations:

```text
mode msgSize rankOffset n total_ms copyIn_ms exchange_ms copyOut_ms
0    131072      65536 4    1.124     0.008       1.106      0.010
0    524288     262144 4    3.704     0.015       3.675      0.014
1    131072      65536 4    0.014     0.008       0.002      0.003
1    524288     262144 4    0.022     0.017       0.003      0.003
2    131072      65536 4    1.278     0.003       1.273      0.003
2    524288     262144 4    5.018     0.003       5.013      0.003
3    131072      65536 4    0.014     0.002       0.002      0.009
3    524288     262144 4    0.020     0.002       0.002      0.015
4    131072      65536 4    0.019     0.008       0.002      0.008
4    524288     262144 4    0.032     0.016       0.002      0.013
5    131072      65536 4    1.198     0.003       1.187      0.008
5    524288     262144 4   39.317     0.002      39.300      0.014
```

Interpretation:

1. The corrected timing matrix falsifies the earlier copy-dominated reading.
   Copy-in and copy-out are consistently tens of microseconds. The dominant
   cost is the rocSHMEM exchange phase.
2. Mode 0 and mode 2 agree qualitatively: full host-stream and exchange-only
   are exchange-bound. Mode 4 (copy-in + copy-out) is cheap.
3. Mode 5 at 256 KiB had one severe outlier and one small software-reliability
   event (`data_wr_retransmit +1`, `data_rx_duplicate_ack +1`). Repeating mode 5
   gave normal exchange timings but exposed a lifecycle counter:

```text
log root: /tmp/tbv-app-gate-pytorch-hoststream-mode5-rerun-a9c630-20260606-070605
application: completed
gate status: fail
failure: data_rx_no_qp +2 on strix-1
data_rx_no_qp_reack: +0
data_rx_no_qp_error_ack: +0
data_wr_timeout: +0
data_wr_retry_exhausted: +0
data_tx_posted == data_tx_completed
```

Because the no-QP increments did not pair with `data_rx_no_qp_reack` or
`data_rx_no_qp_error_ack`, they are not from the SEND/WRITE tombstone re-ACK
path. The remaining no-QP sites include MAD and non-ackable native opcodes
arriving after QP teardown. Add opcode/site-specific no-QP instrumentation
before relaxing the gate or treating this as benign teardown noise.

### 2026-06-06 no-QP split deployment and mode-5 retest

Added and deployed site/opcode-specific no-QP counters:

```text
GDA branch commit: 8261572 kernel: split no-QP receive counters
NixOS config commits:
  e8e4cb2 thunderbolt: ignore self UUID XDomain peers
  60bcb9a thunderbolt: pin GDA no-QP diagnostics
deploy command:
  nix run .#colmena -- --impure apply --on strix-1,strix-2 --reboot boot
post-reboot reload:
  sudo thunderbolt-ibverbs-reload-system
```

Both Strix hosts rebooted into the new `7.0.10` generation, the reload helper
reported `thunderbolt-ibverbs-check: ok`, and the new debugfs counters were
present and zero before testing:

```text
data_rx_no_qp_apple: 0
data_rx_no_qp_mad: 0
data_rx_no_qp_native_ackable: 0
data_rx_no_qp_native_non_ack: 0
data_rx_no_qp_opcode_0..15: 0
```

First classified mode-5 rerun:

```text
log root: /tmp/tbv-app-gate-pytorch-hoststream-mode5-noqp-split-a9c630-20260606-071858
status: pass
loaded_collective_lib: /mnt/Home/tmp/rccl-hoststream-timing-install-a9c630/lib/librccl.so.1.0
dv_poll_wqes sum: +16
data_rx_no_qp: +0
data_rx_no_qp_mad: +0
data_rx_no_qp_native_ackable: +0
data_rx_no_qp_native_non_ack: +0
data_rx_canceled: +0
data_wr_retransmit: +0
data_wr_timeout: +0
data_tx_posted == data_tx_completed == +680
```

Eight additional mode-5 reps, same rebuilt RCCL and PyTorch loader override:

```text
log root: /tmp/tbv-app-gate-pytorch-hoststream-mode5-repeat-a9c630-20260606-071941
status: pass
reps: 8/8
data_rx_no_qp: +0 in every rep
data_rx_no_qp_mad/native_ackable/native_non_ack: +0 in every rep
data_rx_canceled: +0 in every rep
data_wr_timeout: +0 in every rep
data_tx_posted == data_tx_completed in every rep
data_wr_retransmit: +2 total, both completed without teardown-guard hits
```

Per-rep key deltas:

```text
rep retransmit timeout no_qp canceled tx_posted/tx_completed
1   +0         +0      +0    +0       +684/+684
2   +0         +0      +0    +0       +684/+684
3   +0         +0      +0    +0       +688/+688
4   +1         +0      +0    +0       +753/+753
5   +0         +0      +0    +0       +684/+684
6   +0         +0      +0    +0       +688/+688
7   +1         +0      +0    +0       +753/+753
8   +0         +0      +0    +0       +684/+684
```

Mode-5 phase means across the 8-rep set:

```text
msgSize symId n  copyIn_ms exchange_ms copyOut_ms total_ms
131072  0     16 0.002     0.253       0.008      0.264
131072  1     16 0.002     1.596       0.008      1.607
524288  0     16 0.002     9.602       0.014      9.618
524288  1     16 0.002     15.588      0.014      15.605
```

Interpretation:

1. The earlier `data_rx_no_qp +2` did not reproduce after reboot plus clean
   helper reload, so it remains an intermittent lifecycle event rather than a
   classified steady-state mode-5 failure.
2. The split counters are now deployed and included in the gate, so the next
   no-QP occurrence will identify whether it is MAD, ackable native traffic, or
   non-ackable native traffic.
3. The PyTorch all-to-all GDA path is good enough for cautious application-level
   smoke and short benchmark runs. The current hard stop is no longer basic
   correctness of PyTorch/RCCL all-to-all, but performance variability in the
   rocSHMEM exchange phase and the fact that vLLM's dominant collectives still
   do not exercise this GDA path.

### 2026-06-06 PyTorch validation-mode correction and QP timeout sweep

Follow-up retesting found a methodology error in the previous mode-5 PyTorch
validation interpretation. `RCCL_ROCSHMEM_GDA_BENCH_MODE=5` is a phase probe,
not a full all-to-all implementation: the host-stream path runs exchange plus
copy-out, but intentionally skips copy-in. Payload validation of PyTorch
`all_to_all`/`all_to_all_single` is therefore meaningful only in bench mode 0.
The app gate now rejects validated PyTorch all-to-all runs in host-stream phase
modes 1-5 and tells the caller to use `--torch-validate 0` for phase timing or
bench mode 0 for correctness.

The same gate now forwards the rocSHMEM GDA RC QP attributes when present:

```text
ROCSHMEM_GDA_QP_TIMEOUT
ROCSHMEM_GDA_QP_RETRY_CNT
ROCSHMEM_GDA_QP_RNR_RETRY
```

Valid mode-0 controls:

```text
known-good RCCL/rocSHMEM, 4 reps:
  /tmp/tbv-app-gate-pytorch-hoststream-mode0-known-good-validate-20260606-080405
  status: pass 4/4
  data_wr_retransmit: +1
  data_wr_timeout/retry_exhausted/no_qp/canceled: +0
  data_tx_posted == data_tx_completed

local QP-env rocSHMEM/RCCL build, timeout exponent 14, 4 reps:
  /tmp/tbv-app-gate-pytorch-hoststream-mode0-qptimeout14-goodbc-validate-20260606-080445
  status: pass 4/4
  data_wr_retransmit: +6
  data_wr_timeout/retry_exhausted/no_qp/canceled: +0
  data_tx_posted == data_tx_completed == +7443
```

QP timeout exponent sweep, all with PyTorch payload validation enabled, bench
mode 0, sizes `131072,524288`, iters `2`:

```text
timeout 10:
  /tmp/tbv-app-gate-pytorch-hoststream-mode0-qptimeout10-goodbc-validate-20260606-080809
  status: fail 7/8
  failure: HSA hardware exception in rocshmem_alltoallmem_kernel
  dv_hard_error: +1
  data_wr_retransmit: +12
  data_wr_timeout/retry_exhausted/no_qp/canceled: +0

timeout 11:
  /tmp/tbv-app-gate-pytorch-hoststream-mode0-qptimeout11-goodbc-validate-20260606-080940
  status: fail 7/8
  failure: HSA hardware exception in rocshmem_alltoallmem_kernel
  dv_hard_error: +1
  data_wr_retransmit: +30
  data_wr_timeout/retry_exhausted/no_qp/canceled: +0

timeout 12:
  /tmp/tbv-app-gate-pytorch-hoststream-mode0-qptimeout12-goodbc-validate-long-20260606-081213
  status: fail 31/32
  failure: HSA hardware exception in rocshmem_alltoallmem_kernel
  dv_hard_error: +1
  data_wr_retransmit: +104
  data_wr_timeout/retry_exhausted/no_qp/canceled: +0

timeout 13:
  /tmp/tbv-app-gate-pytorch-hoststream-mode0-qptimeout13-goodbc-validate-long-20260606-081901
  status: fail 30/32
  failure: strict counter gate only
  data_rx_no_qp: +6
  data_rx_no_qp_native_non_ack: +6
  data_rx_no_qp_opcode_2: +6
  dv_hard_error: +0
  data_wr_timeout/retry_exhausted/canceled: +0

timeout 14:
  /tmp/tbv-app-gate-pytorch-hoststream-mode0-qptimeout14-goodbc-validate-long-20260606-082224
  status: fail 31/32
  failure: strict counter gate only
  data_rx_no_qp: +2
  data_rx_no_qp_native_non_ack: +2
  data_rx_no_qp_opcode_2: +2
  dv_hard_error: +0
  data_wr_retransmit: +53
  data_wr_timeout/retry_exhausted/canceled: +0
  data_tx_posted == data_tx_completed == +60006
```

Opcode 2 is `TBV_NATIVE_DATA_OP_SEND_ACK`. The `native_non_ack` bucket is
therefore a poor human label for this case; it means the opcode is not one of
the data opcodes that requires a SEND_ACK response, not that the frame is not an
ACK. The timeout-13/14 failures are late SEND_ACK frames arriving after the
target QP has been destroyed. They did not produce payload mismatches, WR
timeouts, retry exhaustion, RX cancels, DV hard errors, or TX imbalance. They
remain a lifecycle/accounting wart, but they are distinct from the earlier
retransmit-to-dead-QP tombstone correctness bug.

A dependency-complete packaged known-good long control stayed clean over the
same validated mode-0 shape:

```text
log root:
  /tmp/tbv-app-gate-pytorch-hoststream-mode0-known-good-validate-long-clean-20260606-082834
status: pass 32/32
data_wr_retransmit: +67
data_rx_no_qp/opcode_2: +0
dv_hard_error: +0
data_wr_timeout/retry_exhausted/canceled: +0
data_tx_posted == data_tx_completed == +61469
exchangeMs n=384 avg=32.622 p50=4.445 p90=95.936 p95=169.700 p99=351.379 max=417.483
```

Conclusions:

1. The payload-correct application benchmark baseline is mode 0. Phase modes are
   timing probes only.
2. QP timeout exponents 10, 11, and 12 are too aggressive for the current
   GPU-origin/DV application path: software retransmit may recover the WR, but
   the GPU can still observe a hard DV/HSA error.
3. Exponents 13 and 14 avoid the observed DV hard-error class in these 32-rep
   runs, but the local QP-env build still exposes intermittent late SEND_ACK
   no-QP teardown noise under the strict gate.
4. The packaged known-good stack does not show that no-QP teardown noise in the
   matching long control, so do not attribute it to the kernel baseline alone.
   It needs a sharper local ROCm/RCCL/rocSHMEM comparison before changing
   kernel behavior or relaxing the hard gate.

### 2026-06-06 Late SEND_ACK no-QP classification

Deployed the SEND_ACK-specific no-QP counters through the NixOS recipe:

```text
nixos-config: 6c937da thunderbolt: pin no-QP SEND_ACK diagnostics
deployed thunderbolt-ibverbs source: 93f61fa kernel: classify no-QP SEND_ACKs
active GDA branch at classification test: 20f714a kernel: classify no-QP SEND_ACKs
```

Both Strix hosts rebooted into kernel `7.0.10`, the module reload helper passed
`thunderbolt-ibverbs-check: ok` on both hosts, and the new debugfs counters were
present on both hosts before testing:

```text
data_rx_no_qp_send_ack
data_rx_no_qp_send_ack_ok
data_rx_no_qp_send_ack_rnr
data_rx_no_qp_send_ack_error
data_rx_no_qp_send_ack_bad_status
```

Re-ran the local QP-env PyTorch host-stream mode-0 validation at QP timeout
exponent 14:

```text
log root:
  /tmp/tbv-app-gate-pytorch-hoststream-mode0-qptimeout14-sendack-split-20260606-084213
status: fail 31/32
failure: strict counter gate only, rep 29
data_rx_no_qp: +2
data_rx_no_qp_native_non_ack: +2
data_rx_no_qp_opcode_2: +2
data_rx_no_qp_send_ack: +2
data_rx_no_qp_send_ack_ok: +2
data_rx_no_qp_send_ack_rnr/error/bad_status: +0
data_wr_retransmit: +38 total, +0 in the failing rep
data_wr_timeout/retry_exhausted/canceled: +0
data_rx_canceled: +0
dv_hard_error: +0
data_tx_posted == data_tx_completed == +58374
exchangeMs n=384 avg=20.225 p50=2.754 p90=77.613 p95=94.640 p99=293.369 max=353.356
```

This cleanly classifies the intermittent timeout-14 strict-gate failure: the
two no-QP frames were both late `SEND_ACK_OK` frames, not retransmitted data,
not tombstone re-ACK traffic, not RNR/error ACKs, and not a hard application
failure. The failing rep had no retransmits, no WR timeout, no retry exhaustion,
no RX cancel, no DV hard error, and balanced TX accounting. The most precise
statement is therefore: path-diverse duplicate OK ACK fanout can arrive after
the sender-side QP is destroyed; this is a teardown/accounting artifact, not a
payload-correctness failure.

Added an app-gate opt-in:

```text
active GDA branch: e070fb8 bench: allow late OK SEND_ACK no-QP opt-in
flag: --allow-late-send-ack-no-qp
env:  TBV_ALLOW_LATE_SEND_ACK_NO_QP=1
```

The opt-in is intentionally narrow. It skips the hard failure for
`data_rx_no_qp` only when the no-QP delta is positive and exactly equals both
`data_rx_no_qp_send_ack` and `data_rx_no_qp_send_ack_ok`. Non-ACK no-QP frames,
RNR/error/bad-status ACKs, tombstone errors, RX cancels, DV hard errors, WR
timeouts, retry exhaustion, and TX errors remain hard failures.

Validation with the opt-in enabled:

```text
log root:
  /tmp/tbv-app-gate-pytorch-hoststream-mode0-qptimeout14-allow-late-sendack-20260606-084834
status: pass 32/32
data_wr_retransmit: +48
data_rx_no_qp: +0
data_rx_no_qp_send_ack/ok/rnr/error/bad_status: +0
data_rx_canceled: +0
dv_hard_error: +0
data_wr_timeout/retry_exhausted: +0
data_wr_retransmit_closing_qp/no_live_path/teardown_path: +0
data_tx_posted == data_tx_completed == +59729
exchangeMs n=384 avg=23.048 p50=3.289 p90=81.047 p95=96.945 p99=293.016 max=356.449
```

The allowance did not need to fire in this clean run, which is useful: the flag
does not hide any observed failure class by default, and the strict gate remains
available for lifecycle work. For application-level benchmark runs, use mode 0
with payload validation and QP timeout exponent 14; include
`--allow-late-send-ack-no-qp` only when the benchmark harness should tolerate
classified late duplicate OK ACK teardown noise.

### 2026-06-06 PyTorch App-Level Threshold Routing

Ran a PyTorch all-to-all host-stream-vs-fallback application matrix with
payload validation enabled, QP timeout exponent 14, and the classified late
`SEND_ACK_OK` no-QP allowance enabled:

```text
log root:
  /tmp/tbv-app-gate-pytorch-hoststream-vs-fallback-qptimeout14-sizes-20260606-085448
sizes: 65536,131072,262144,524288,1048576,2097152
iters: 8
reps: 5
RCCL_ROCSHMEM_THRESHOLD=1048576
```

Fallback mode did not exercise GDA and failed two later reps in regular RCCL
NET/IB handling, not in the USB4 GDA counters:

```text
fallback status: fail reps 4/5
fallback DV WQEs: +0
fallback dv_hard_error/data_wr_retransmit/data_wr_timeout/data_rx_canceled: +0
fallback data_tx_posted == data_tx_completed == +114795
fallback data_rx_no_qp: +2, both data_rx_no_qp_send_ack_ok from rep 1
observed RCCL errors: IBV_WC_GENERAL_ERR on RECV, IBV_WC_WR_FLUSH_ERR on RDMA_WRITE,
  and one ibv_post_send() Invalid argument / Bad WR
```

Host-stream mode passed all five reps:

```text
hoststream status: pass 5/5
dv_poll_wqes: +720
dv_hard_error: +0
data_wr_retransmit: +39
data_wr_timeout/retry_exhausted: +0
data_rx_no_qp: +0
data_tx_posted == data_tx_completed == +148676
```

PyTorch rank-0 application timing still has large tail variance:

```text
fallback app us/iter:
  65536   n=4 avg=325.8  p50=131.3  p90=909.7   max=909.7
  131072  n=4 avg=515.3  p50=171.2  p90=937.8   max=937.8
  262144  n=4 avg=374.2  p50=318.0  p90=548.5   max=548.5
  524288  n=3 avg=1494.1 p50=1284.3 p90=2558.0  max=2558.0
  1048576 n=3 avg=1884.2 p50=2121.5 p90=2129.2  max=2129.2
  2097152 n=3 avg=3026.8 p50=2871.7 p90=3583.8  max=3583.8

hoststream app us/iter:
  65536   n=5 avg=4715.7   p50=1287.6 p90=19045.8   max=19045.8
  131072  n=5 avg=10831.8  p50=11023.6 p90=20543.4  max=20543.4
  262144  n=5 avg=12638.5  p50=13836.5 p90=22943.5  max=22943.5
  524288  n=5 avg=25802.7  p50=6868.1 p90=84153.5  max=84153.5
  1048576 n=5 avg=1199.3   p50=1077.6 p90=1487.7   max=1487.7
  2097152 n=5 avg=221233.5 p50=3114.3 p90=1094822.6 max=1094822.6
```

The internal host-stream timing exposed a routing problem in the benchmark
recipe rather than a scratch-buffer limit. With `RCCL_ROCSHMEM_THRESHOLD=1048576`
and two ranks, RCCL sees `msgSize = per_peer_bytes * world_size`, so PyTorch
per-peer sizes above 512 KiB fell back inside RCCL. Timing lines appeared only
up to `msgSize=1048576`:

```text
msgSize 131072  n=90 exchange_avg=4.193  exchange_p50=0.841  exchange_p90=2.365  exchange_max=76.615  total_avg=4.211
msgSize 262144  n=90 exchange_avg=9.981  exchange_p50=2.634  exchange_p90=68.550 exchange_max=78.810  total_avg=10.002
msgSize 524288  n=90 exchange_avg=12.893 exchange_p50=4.854  exchange_p90=70.695 exchange_max=87.471  total_avg=12.920
msgSize 1048576 n=90 exchange_avg=25.913 exchange_p50=10.151 exchange_p90=90.106 exchange_max=164.807 total_avg=25.953
```

Code/recipe fix carried forward:

```text
rocm-systems: c896cdeb24 rccl: allow larger GDA alltoall thresholds
nix-strix-halo gda: e58fc85 therock: carry GDA threshold routing patch
```

The durable RCCL change removes the redundant 1 MiB cap in
`rcclUseAlltoAllGda()`. The call site already checks `msgSize <=
comm->rocshmemThreshold`, and the host-stream implementation separately checks
`msgSize <= comm->bufThreshold`. The nix-strix patch also carries
`RCCL_ROCSHMEM_FORCE_ENABLE` through the pinned TheRock source so the 2-host
Strix topology can exercise the path intentionally.

After rebuilding the local standalone RCCL install with the cap removed, ran a
focused threshold-raised validation:

```text
log root:
  /tmp/tbv-app-gate-pytorch-hoststream-threshold4m-qptimeout14-20260606-091337
status: pass 3/3
sizes: 1048576,2097152
iters: 4
RCCL_ROCSHMEM_THRESHOLD=4194304
loaded RCCL:
  /mnt/Home/tmp/rccl-hoststream-qptimeout-host-goodbc-install/lib/librccl.so.1.0
```

Counters stayed clean while the larger sizes exercised host-stream GDA:

```text
dv_poll_wqes: +120
dv_hard_error: +0
data_wr_retransmit: +10
data_wr_timeout/retry_exhausted: +0
data_wr_retransmit_closing_qp/no_live_path/teardown_path: +0
data_rx_no_qp/send_ack/send_ack_ok: +0
data_rx_canceled: +0
data_tx_posted == data_tx_completed == +30803
```

The key routing proof is that host-stream timing lines now include the two
larger RCCL message sizes:

```text
hoststream internal timing:
  msgSize 2097152 n=30 exchange_avg=24.599 exchange_p50=19.176 exchange_p90=75.578  exchange_max=148.830 total_avg=24.667
  msgSize 4194304 n=30 exchange_avg=74.066 exchange_p50=38.766 exchange_p90=279.285 exchange_max=360.107 total_avg=74.187

PyTorch rank-0 app timing:
  1048576 n=3 avg=30308.1 p50=12639.8 min=10467.1 p90=67817.3  max=67817.3
  2097152 n=3 avg=72236.6 p50=23570.7 min=21737.4 p90=171401.7 max=171401.7
```

Conclusion: the stack is now close enough for application-level benchmark
experiments in the correctness sense: validated PyTorch all-to-all can use the
USB4 GDA host-stream path at 1 MiB and 2 MiB per-peer sizes with clean hard
counters. It is not performance-stable yet. The next bottleneck is the
host-stream exchange variance itself, not copy-in/copy-out: at 4 MiB RCCL
`msgSize`, average copy-in plus copy-out is about 0.12 ms while exchange averages
74 ms and tails to 360 ms.

### 2026-06-06 ACK Repeat And rocSHMEM Wait-Budget Follow-Up

After a Colmena boot/reboot deployment of the rebased stack, both Strix hosts
came back on kernel 7.0.10. One operational issue remains: after manual reboots,
`thunderbolt_ibverbs` did not autoload until `thunderbolt-ibverbs-reload-system`
was run on each host. After reload, `thunderbolt-ibverbs-check` was clean and
each host exposed four USB4 RDMA devices. Treat this as a Nix/module-service
ordering issue to fix before unattended benchmark loops.

`native_ack_repeat` is now packaged and runtime-tunable, but repeat greater than
one is not a correctness fix under PyTorch host-stream pressure:

```text
repeat=2, qptimeout=14, 4 MiB threshold:
  /tmp/tbv-app-gate-pytorch-hoststream-ackrepeat2-threshold4m-qptimeout14-20260606-093200
  pass 5/5, data_wr_retransmit=7, no timeout/hard error

repeat=3, same row:
  /tmp/tbv-app-gate-pytorch-hoststream-ackrepeat3-threshold4m-qptimeout14-20260606-093332
  fail rep 5, dv_hard_error=1, data_wr_retransmit=28,
  data_wr_timeout=1, data_wr_retry_exhausted=1

repeat=2 rerun with expanded counters:
  /tmp/tbv-app-gate-pytorch-hoststream-ackrepeat2-counters-threshold4m-qptimeout14-20260606-093733
  fail rep 4, data_tx_ack_error=2, data_rx_active_timeout=1,
  data_wr_retransmit=18, no WR timeout
```

The repeat-2 rerun is the important correction: the failing symptom was not
plain lost OK ACKs. The receiver hit `data_rx_active_timeout`, sent an error ACK,
and the sender surfaced a DV/CQ/HSA failure. Increasing ACK copies can add
reverse-channel pressure and can make this worse. Keep `native_ack_repeat=1` as
the packaged baseline.

The QP timeout sweep also exposed an application-layer timeout independent of
kernel hard counters. With QP timeout exponent 15, full mode and exchange-only
mode failed without `dv_hard_error`, `data_rx_active_timeout`, `data_tx_errors`,
WR timeout, or retry exhaustion. The decisive exchange-only error was:

```text
/tmp/tbv-app-gate-pytorch-hoststream-exchangeonly-threshold4m-qptimeout15-20260606-094353
USB4 alltoall DATA wait timed out ctx=8 my_pe=1 src_team=0 src_world=0
expected=8 observed=6 sync_word=... seq=7
projects/rocshmem/src/gda/context_gda_tmpl_device.hpp:997
```

That made the rocSHMEM device wait budget load-bearing. Increasing
`USB4_ALLTOALLV_WAIT_ABORT_SPINS` from `1 << 15` to `1 << 20` was validated and
committed in rocm-systems:

```text
rocm-systems: d308b46e28 rocshmem: relax USB4 alltoall wait budget
local rocSHMEM install:
  /mnt/Home/tmp/rocshmem-waitbudget-install
local RCCL install linked against it:
  /mnt/Home/tmp/rccl-hoststream-waitbudget-install
```

Post-fix PyTorch results with payload validation:

```text
exchange-only mode 2, qptimeout=15:
  /tmp/tbv-app-gate-pytorch-hoststream-exchangeonly-waitbudget-threshold4m-qptimeout15-20260606-100408
  pass 5/5, data_wr_retransmit=31, data_wr_timeout/retry_exhausted=0,
  data_tx_posted == data_tx_completed == 49084,
  data_rx_active_timeout/reorder fatal/dv_hard_error=0

full mode 0, qptimeout=15:
  /tmp/tbv-app-gate-pytorch-hoststream-full-waitbudget-threshold4m-qptimeout15-20260606-100559
  pass 5/5, data_wr_retransmit=9, data_wr_timeout/retry_exhausted=0,
  data_tx_posted == data_tx_completed == 48567,
  dv_admission_attempts=dv_poll_wqes=200, dv_hard_error=0

full mode 0, qptimeout=14:
  /tmp/tbv-app-gate-pytorch-hoststream-full-waitbudget-threshold4m-qptimeout14-20260606-100744
  pass 5/5, data_wr_retransmit=11, data_wr_timeout/retry_exhausted=0,
  data_tx_posted == data_tx_completed == 48837,
  dv_admission_attempts=dv_poll_wqes=200, dv_hard_error=0
```

The larger 4 MiB / 8 MiB PyTorch smoke needed a methodology correction. The
first run succeeded at the application level but failed the app gate because
`--dv-check auto` expected DV WQEs:

```text
/tmp/tbv-app-gate-pytorch-hoststream-large-waitbudget-threshold4m-qptimeout14-20260606-100932
app logs: successful timings for 4 MiB and 8 MiB in all reps
gate failure: expected dv_poll_wqes delta >= 1, got 0
dv_admission_attempts=dv_poll_wqes=dv_hard_error=0
data_tx_posted == data_tx_completed == 92109
data_wr_retransmit=16, data_wr_timeout/retry_exhausted=0
```

Re-running the same row with `--dv-check forbid` turned it into a clean routing
discriminator:

```text
/tmp/tbv-app-gate-pytorch-hoststream-large-waitbudget-threshold4m-qptimeout14-dvforbid-20260606-101338
status: pass 3/3
dv_admission_attempts=dv_poll_wqes=dv_hard_error=0
data_wr_retransmit=15, data_wr_timeout/retry_exhausted=0
data_tx_posted == data_tx_completed == 92099
data_rx_active_timeout/reorder fatal/data_tx_ack_error=0
```

Source explains the route: `ncclAlltoAll_impl()` computes
`msgSize = count * ncclTypeSize(datatype) * comm->nRanks` and uses GDA only when
`msgSize <= comm->rocshmemThreshold`. With two ranks and
`RCCL_ROCSHMEM_THRESHOLD=4194304`, PyTorch's 1 MiB and 2 MiB per-rank cases map
to 2 MiB and 4 MiB RCCL message sizes and exercise host-stream DV. The 4 MiB and
8 MiB per-rank cases map to 8 MiB and 16 MiB and correctly fall back.

Current app-level benchmark readiness:

1. PyTorch/RCCL all-to-all through USB4 GDA host-stream is now correctness-clean
   for the 1 MiB and 2 MiB per-rank threshold window under repeated validation.
2. Larger 4 MiB and 8 MiB PyTorch all-to-all passes in this configuration but is
   not a GDA/DV benchmark at the 4 MiB threshold; use `--dv-check forbid` to
   record that intentionally.
3. The reusable Nix packaging task is now done:
   `nix-strix-halo` commit `a6b010a` carries the tested rocm-systems USB4 GDA
   branch state, including the wait-budget fix, as a TheRock source patch. The
   configure-only derivation applied the patch stack and completed CMake
   configure; it failed only in the known configure-only fixup path because no
   `bin/therock-hip-clang++` is installed when the build is intentionally
   skipped.
4. The next performance task is still the rocSHMEM exchange-phase tail, not a
   currently observed kernel correctness counter.

### 2026-06-06 Post-Boot Autoload And App Gate

The operational boot gap above is fixed and deployed.

Code/config commits:

```text
thunderbolt-ibverbs GDA:
  5f3ddb2 nix: wait for full ibverbs readiness in boot check
deploy input used by nixos-config:
  693ae75 nix: wait for full ibverbs readiness in boot check
nixos-config:
  06dc1ce strix: load thunderbolt ibverbs at boot
```

The check helper now polls the complete requested runtime state until timeout
instead of failing as soon as `debugfs/summary` exists. This matters because
`thunderbolt_ibverbs` loads several seconds before XDomain discovery registers
the verbs devices and marks rails data-ready.

Colmena deployment:

```text
nix run .#colmena -- --impure apply boot --reboot --on strix-1,strix-2

strix-1 system:
  /nix/store/32gn8141ninvllihvr28k2lxjmx8dcgf-nixos-system-strix-1-26.11pre-git
strix-2 system:
  /nix/store/c0z10al6fpp8gp7chr34ipc0g6xizsg9-nixos-system-strix-2-26.11pre-git
```

Both hosts rebooted and the boot-time check succeeded without manual reload:

```text
strix-1:
  thunderbolt-ibverbs-check.service: success
  wall clock: 7.112s
  ibv_devices: usb4_rdma5 usb4_rdma6 usb4_rdma0 usb4_rdma1

strix-2:
  thunderbolt-ibverbs-check.service: success
  wall clock: 7.100s
  ibv_devices: usb4_rdma5 usb4_rdma6 usb4_rdma0 usb4_rdma1

both:
  profile=linux_perf
  native_control=source_aware
  native_qp_tombstone_reack=1
  native_qp_tombstone_max=4096
  native_retransmit_teardown_guard=1
  native_ack_repeat=1
  verbs_registered=1
  dv_poll_running=1
  all four native rails active/data_ready
  native E2E disabled on every rail
```

Post-reboot PyTorch host-stream GDA gate, using the boot-loaded module state:

```text
/tmp/tbv-app-gate-pytorch-hoststream-postboot-waitbudget-threshold4m-qptimeout14-20260606-104300
status: pass 3/3
sizes: 1048576,2097152
iters: 4
RCCL_ROCSHMEM_THRESHOLD=4194304
RCCL_ROCSHMEM_GDA_BENCH_MODE=0
ROCSHMEM_GDA_QP_TIMEOUT=14
local RCCL:
  /mnt/Home/tmp/rccl-hoststream-waitbudget-install/lib/librccl.so.1
local rocSHMEM:
  /mnt/Home/tmp/rocshmem-waitbudget-install
```

Final counters were symmetric on both hosts:

```text
dv_poll_wqes: 60
dv_poll_errors/dv_hard_error: 0/0
data_wr_send: 300
data_wr_retransmit: 14
data_wr_retry_exhausted/data_wr_timeout: 0/0
data_wr_retransmit_closing_qp/no_live_path/teardown_path: 0/0/0
data_rx_ack: 810
data_rx_ack_matched: 300
data_rx_ack_match_retried: 9
data_rx_ack_match_max_ms: 287ms on strix-1, 323ms on strix-2
data_rx_late_ack: 318
data_rx_duplicate_ack: 9
data_rx_no_qp_reack/data_rx_no_qp_error_ack/data_rx_no_qp: 0/0/0
data_rx_active_timeout/data_rx_reorder_timeout/data_rx_canceled: 0/0/0
```

Interpretation: boot autoload is now suitable for unattended GDA testing, and
validated PyTorch all-to-all still exercises the host-stream DV/GDA path after a
fresh reboot. The recovered-loss path is active under application pressure
(`data_wr_retransmit` and `data_rx_ack_match_retried` move), but it recovers
without retry exhaustion, WR timeout, RX reorder timeout, canceled RX frames, or
DV hard errors in this gate. Keep those recovery counters in every app benchmark
summary; they are evidence of real transport loss being handled, not noise.

### 2026-06-06 DV WRITE source-bucket TX timing

Added kernel-side DV WRITE timing instrumentation:

```text
thunderbolt-ibverbs:
  d4a217f kernel: bucket DV write TX timing by source offset

debugfs counters:
  dv_write_tx_mr_bucket_<0..7>_{count,ns,bytes}
  dv_write_tx_addr_bucket_<0..7>_{count,ns,bytes}
bucket size:
  64 MiB
timing span:
  tbv_post_send_one() accepts a DV-origin RDMA WRITE
  -> all native TX fragments for that send context drain successfully
```

NixOS deployment was repaired to use the active GDA worktree rather than an
older thunderbolt-ibverbs checkout:

```text
nixos-config thunderbolt-ibverbs-kernel:
  path:/mnt/Home/src/thunderbolt-ibverbs-gda-iommu-revive

deploy:
  nix run .#colmena -- --impure apply boot --reboot --on strix-1,strix-2

strix-1 system:
  /nix/store/7lhnxzhy00i4zwg2fsgxz6l2b57096dr-nixos-system-strix-1-26.11pre-git
strix-2 system:
  /nix/store/cfsjm8lgscvbkkqzyhr66a12l59wx9pf-nixos-system-strix-2-26.11pre-git
module:
  /nix/store/21gc4a0jvalpclcgakvanzza3acsrihn-thunderbolt-ibverbs-0.1.0
```

Post-reboot baseline:

```text
both hosts:
  kernel: 7.0.10
  verbs_qps: 4
  dv_hard_error: 0
  data_wr_timeout/data_wr_retry_exhausted: 0/0
  data_tx_posted == data_tx_completed
  data_tx_errors: 0
  data_rx_canceled: 0
  dv_write_tx_* counters present and initially zero
```

Payload-only PyTorch host-stream all-to-all, 1 MiB, two reps, four iterations
per rep, `RCCL_ROCSHMEM_GDA_BENCH_MODE=2`, `usb4_alltoall_mode=1`,
`usb4_alltoall_ack=0`, `RCCL_ROCSHMEM_THRESHOLD=4194304`,
`num_sym_buf=4`, coherent source/destination backing:

```text
roots:
  /mnt/Home/tmp/tbv-app-gate-logs/pytorch-kernelbucket-src0-dst0-20260606-212220
  /mnt/Home/tmp/tbv-app-gate-logs/pytorch-kernelbucket-src0-dst3-20260606-212220
  /mnt/Home/tmp/tbv-app-gate-logs/pytorch-kernelbucket-src3-dst0-20260606-212220

case       pytorch_avg_us  exchange_avg_ms  kernel_mr_bucket  writes  bytes     kernel_avg_ms
src0/dst0        2428.3            4.044       0                 16  16777216        1.513
src0/dst3        3260.0            2.351       0                 16  16777216        1.235
src3/dst0       28499.2           28.570       3                 16  16777216       27.680
```

All three roots passed with clean hard counters: no DV hard error, no WR
timeout, no retry exhaustion, no TX error, and TX posted/completed balanced.
Small RNR/write-gap recovery moved in the source-0 controls, but recovered
without failed WRs.

Interpretation:

1. The source-slot penalty is visible inside the kernel on the DV WRITE native
   TX-drain span. Source slot 3 maps to MR bucket 3 and costs about 27.7 ms per
   1 MiB WRITE, while source slot 0 maps to MR bucket 0 and costs about
   1.2-1.5 ms.
2. Holding the source at slot 0 while moving the destination to slot 3 remains
   fast in the kernel bucket timing. That separates the dominant effect from
   remote destination offset.
3. This rules out RCCL host-stream address selection, rocSHMEM pSync
   signal/wait, and userspace WQE post as the primary source of the large
   slot-3 latency. The remaining span is source-MR copy plus native frame
   enqueue/TX completion in the module.

Next discriminator: split the new `dv_write_tx_mr_bucket_*_ns` into source
copy time and enqueue/TX-complete time. If bucket 3 is already slow during
`tbv_copy_send_range()`, the issue is CPU/coherent-memory read behavior from
that rocSHMEM source slot. If copy time is flat and the post-copy TX drain is
slow, the issue is in native frame queueing/TX/DMA completion.

### 2026-06-06 DV WRITE source-copy split

Added per-bucket split timing:

```text
thunderbolt-ibverbs:
  ca32618 kernel: split DV write copy and TX timing

debugfs counters:
  dv_write_copy_mr_bucket_<0..7>_ns
  dv_write_postcopy_mr_bucket_<0..7>_ns
```

The same payload-only PyTorch host-stream all-to-all gate was rerun after
deploying the split counters:

```text
roots:
  /mnt/Home/tmp/tbv-app-gate-logs/pytorch-copybucket-src0-dst0-20260606-2150
  /mnt/Home/tmp/tbv-app-gate-logs/pytorch-copybucket-src3-dst0-20260606-2150
  /mnt/Home/tmp/tbv-app-gate-logs/pytorch-copybucket-src0-dst3-20260606-2150

case       pytorch_avg_us  exchange_avg_ms range        bucket writes bytes     total_ms copy_ms postcopy_ms
src0/dst0        1875.7        2.5-5.4     source MR    0       16  16777216     1.493   0.229     1.264
src3/dst0       42508.8       29.3-38.9    source MR    3       16  16777216    30.665  29.582     1.082
src0/dst3        2197.9        1.7-2.5     source MR    0       16  16777216     1.293   0.240     1.053
```

All three runs completed with balanced TX posted/completed counters, no DV hard
error, no WR timeout, no retry exhaustion, and no TX error.

Interpretation:

1. The source-slot penalty is almost entirely source-copy time inside
   `tbv_copy_send_range()`. Moving from source bucket 0 to source bucket 3 adds
   about 29.35 ms to the copy span for a 1 MiB DV WRITE.
2. The post-copy/native-TX span stays flat, about 1.1-1.3 ms, across the fast
   and slow source cases. Native frame queueing, Thunderbolt TX completion, and
   the remote write path are not the dominant cause of the slot-3 regression.
3. Moving only the destination to slot 3 remains fast, with source bucket 0 copy
   time still about 0.24 ms. The slow path is local source-read behavior from
   the rocSHMEM coherent source slot, not remote destination placement.

Next discriminator: determine whether the slow CPU read is tied to virtual
address/slot offset, physical backing, NUMA/CCX locality, PAT/cacheability, or
first-touch/migration behavior of the coherent rocSHMEM scratch allocation.

### 2026-06-06 DV WRITE SG cursor fix

The source-copy split suggested a local source-read problem, but the full source
slot curve exposed the real shape:

```text
pre-fix payload-only roots:
  /mnt/Home/tmp/tbv-app-gate-logs/pytorch-copybucket-src0-dst0-20260606-2150
  /mnt/Home/tmp/tbv-app-gate-logs/pytorch-copybucket-src1-dst0-20260606-2210
  /mnt/Home/tmp/tbv-app-gate-logs/pytorch-copybucket-src2-dst0-20260606-2210
  /mnt/Home/tmp/tbv-app-gate-logs/pytorch-copybucket-src3-dst0-20260606-2150

source bucket  copy_ms  postcopy_ms  total_ms  pytorch_avg_us
0                0.229       1.264     1.493        1875.7
1                9.133       1.071    10.204       11116.1
2               19.121       1.075    20.197       20714.1
3               29.582       1.082    30.665       42508.8
```

The near-linear +10 ms per 64 MiB source bucket contradicted a localized
cacheability, NUMA, or physical-backing cliff. The kernel implementation then
confirmed the mechanism: `ib_umem_copy_from()` calls `sg_pcopy_to_buffer()` with
`skip = offset + ib_umem_offset(umem)`, so every high-offset WRITE walks the
registered MR scatterlist from the beginning before copying the 1 MiB payload.
The rocSHMEM host scratch pool is one large registered MR, so rotating to later
64 MiB slots paid an O(MR offset) cost on every copied DV WRITE.

Fix:

```text
kernel/ibdev.c:
  tbv_send_segment now stores the starting scatterlist entry, entry index, and
  entry-local offset resolved once in tbv_prepare_send_segments().

  tbv_copy_send_range() copies from that prepared cursor instead of calling the
  generic ib_umem_copy_from() helper for copied send/WRITE payloads.
```

Post-fix deployment:

```text
thunderbolt-ibverbs derivation:
  /nix/store/xvfhqm5x28h8cryn6bp32a27lbpnaii5-thunderbolt-ibverbs-0.1.0

strix-1 system:
  /nix/store/4rnwyrmial9y1ncgl08giyrq6wx4ni04-nixos-system-strix-1-26.11pre-git
strix-2 system:
  /nix/store/0saia7gi984q937sccfdmn0znrk1gxk5-nixos-system-strix-2-26.11pre-git
```

Post-fix payload-only controls:

```text
roots:
  /mnt/Home/tmp/tbv-app-gate-logs/pytorch-sgcursor-src0-dst0-20260606-2203
  /mnt/Home/tmp/tbv-app-gate-logs/pytorch-sgcursor-src3-dst0-20260606-2203
  /mnt/Home/tmp/tbv-app-gate-logs/pytorch-sgcursor-rotate-20260606-2203

case          pytorch_avg_us  bucket writes bytes    total_ms copy_ms postcopy_ms
src0/dst0           1596.9      0      16  16777216    1.094   0.052     1.041
src3/dst0           1619.0      3      16  16777216    1.088   0.052     1.036
rotate              1680.8      0       4   4194304    1.188   0.061     1.127
rotate              1680.8      1       4   4194304    1.094   0.061     1.033
rotate              1680.8      2       4   4194304    1.089   0.064     1.025
rotate              1680.8      3       4   4194304    1.083   0.061     1.022
```

Validated full host-stream PyTorch all-to-all, with copy-in/exchange/copy-out
enabled and rotating source/destination slots:

```text
root:
  /mnt/Home/tmp/tbv-app-gate-logs/pytorch-sgcursor-full-rotate-20260606-2203

pytorch all_to_all_single 1 MiB:
  reps: 2
  avg: 2783.5 us
  min/max: 1577.6 / 3989.4 us
  validation: pass

kernel buckets:
  bucket 0: total 1.122 ms, copy 0.054 ms, postcopy 1.067 ms
  bucket 1: total 1.104 ms, copy 0.046 ms, postcopy 1.058 ms
  bucket 2: total 1.073 ms, copy 0.043 ms, postcopy 1.030 ms
  bucket 3: total 1.057 ms, copy 0.043 ms, postcopy 1.014 ms
```

All post-fix roots completed with balanced TX posted/completed counters, no DV
hard error, no WR timeout, no retry exhaustion, no TX error, and no RX canceled
frames. Small RNR/write-gap recovery still appears in some runs and recovers
cleanly.

Interpretation:

1. The source-slot performance bug is fixed at root cause. The copy time no
   longer scales with MR offset; bucket 3 improved from 29.582 ms to 0.052 ms.
2. Source-slot pinning is no longer required for the 1 MiB host-stream
   all-to-all path. Rotating through all four scratch slots is flat at the
   kernel copy layer and passes validated PyTorch.
3. The remaining per-WR span is post-copy/native TX completion, about 1.0-1.1
   ms for 1 MiB. That is now the next performance target for application-level
   benchmarking, not the rocSHMEM scratch slot selection.
